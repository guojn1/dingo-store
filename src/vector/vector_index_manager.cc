// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vector/vector_index_manager.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "butil/binary_printer.h"
#include "butil/status.h"
#include "common/helper.h"
#include "common/logging.h"
#include "config/config_manager.h"
#include "fmt/core.h"
#include "log/segment_log_storage.h"
#include "meta/store_meta_manager.h"
#include "proto/common.pb.h"
#include "proto/error.pb.h"
#include "proto/file_service.pb.h"
#include "proto/node.pb.h"
#include "proto/raft.pb.h"
#include "server/file_service.h"
#include "server/server.h"
#include "vector/codec.h"
#include "vector/vector_index.h"
#include "vector/vector_index_factory.h"
#include "vector/vector_index_snapshot.h"

namespace dingodb {

bool VectorIndexManager::Init(std::vector<store::RegionPtr> regions) {
  for (auto& region : regions) {
    // init vector index map
    const auto& definition = region->InnerRegion().definition();
    if (definition.index_parameter().index_type() == pb::common::IndexType::INDEX_TYPE_VECTOR) {
      DINGO_LOG(INFO) << fmt::format("Init load region {} vector index", region->Id());

      // When raft leader start, may load vector index,
      // so check vector index wherther exist, if exist then don't load vector index.
      auto vector_index = GetVectorIndex(region->Id());
      if (vector_index == nullptr) {
        continue;
      }
      auto status = LoadOrBuildVectorIndex(region);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("Load region {} vector index failed, ", region->Id());
        return false;
      }
    }
  }

  return true;
}

bool VectorIndexManager::AddVectorIndex(uint64_t region_id, std::shared_ptr<VectorIndex> vector_index) {
  return vector_indexs_.Put(region_id, vector_index) > 0;
}

bool VectorIndexManager::AddVectorIndex(uint64_t region_id, const pb::common::IndexParameter& index_parameter) {
  auto vector_index = VectorIndexFactory::New(region_id, index_parameter);
  if (vector_index == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("New vector index failed, vector index id: {} parameter: {}", region_id,
                                    index_parameter.ShortDebugString());
    return false;
  }

  auto ret = AddVectorIndex(region_id, vector_index);
  if (!ret) {
    DINGO_LOG(ERROR) << fmt::format("Add region {} vector index failed", region_id);
    return false;
  }

  // Update vector index status NORMAL
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);

  DINGO_LOG(INFO) << fmt::format("Add region {} vector index success", region_id);

  return true;
}

// Deletes the vector index for the specified region ID.
// @param region_id The ID of the region whose vector index is to be deleted.
void VectorIndexManager::DeleteVectorIndex(uint64_t region_id) {
  // Log the deletion of the vector index.
  DINGO_LOG(INFO) << fmt::format("Delete region's vector index {}", region_id);

  // Remove the vector index from the vector index map.
  vector_indexs_.Erase(region_id);

  // The vector index dir.
  std::string snapshot_parent_path = fmt::format("{}/{}", Server::GetInstance()->GetIndexPath(), region_id);
  if (std::filesystem::exists(snapshot_parent_path)) {
    DINGO_LOG(INFO) << fmt::format("Delete region's vector index snapshot {}", snapshot_parent_path);
    Helper::RemoveAllFileOrDirectory(snapshot_parent_path);
  }

  // Delete the vector index metadata from the metadata store.
  meta_writer_->Delete(GenKey(region_id));
}

std::shared_ptr<VectorIndex> VectorIndexManager::GetVectorIndex(uint64_t region_id) {
  return vector_indexs_.Get(region_id);
}

std::vector<std::shared_ptr<VectorIndex>> VectorIndexManager::GetAllVectorIndex() {
  std::vector<std::shared_ptr<VectorIndex>> vector_indexs;
  if (vector_indexs_.GetAllValues(vector_indexs) < 0) {
    DINGO_LOG(ERROR) << "Get all vector index failed";
  }
  return vector_indexs;
}

// Load vector index for already exist vector index at bootstrap.
// Priority load from snapshot, if snapshot not exist then load from rocksdb.
butil::Status VectorIndexManager::LoadOrBuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);

  auto online_vector_index = GetVectorIndex(region->Id());
  auto update_online_vector_index_status = [online_vector_index](pb::common::RegionVectorIndexStatus status) {
    if (online_vector_index != nullptr) {
      online_vector_index->SetStatus(status);
    }
  };

  // Update vector index status LOADING
  update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_LOADING);

  // try to LoadVectorIndexFromSnapshot
  auto new_vector_index = VectorIndexSnapshot::LoadVectorIndexSnapshot(region);
  if (new_vector_index != nullptr) {
    // replay wal
    DINGO_LOG(INFO) << fmt::format("Load vector index from snapshot, id {} success, will ReplayWal", region->Id());
    auto status = ReplayWalToVectorIndex(new_vector_index, new_vector_index->ApplyLogIndex() + 1, UINT64_MAX);
    if (status.ok()) {
      DINGO_LOG(INFO) << fmt::format("ReplayWal success, id {}, log_id {}", region->Id(),
                                     new_vector_index->ApplyLogIndex());
      new_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
      // set vector index to vector index map
      vector_indexs_.Put(region->Id(), new_vector_index);

      // Update vector index status NORMAL
      update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

      return status;
    }
  }

  DINGO_LOG(INFO) << fmt::format("Load vector index from snapshot, id {} failed, will build vector_index",
                                 region->Id());

  // build a new vector_index from rocksdb
  new_vector_index = BuildVectorIndex(region);
  if (new_vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format("Build vector index failed, vector index id {}", region->Id());
    // Update vector index status NORMAL
    update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed, vector index id %lu", region->Id());
  }

  // add vector index to vector index map
  new_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
  vector_indexs_.Put(region->Id(), new_vector_index);

  // Update vector index status NORMAL
  update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

  DINGO_LOG(INFO) << fmt::format("Build vector index success, id {}", region->Id());

  return butil::Status();
}

// Replay vector index from wal
butil::Status VectorIndexManager::ReplayWalToVectorIndex(std::shared_ptr<VectorIndex> vector_index,
                                                         uint64_t start_log_id, uint64_t end_log_id) {
  assert(vector_index != nullptr);
  DINGO_LOG(INFO) << fmt::format("Replay vector index {} from log id {} to log id {}", vector_index->Id(), start_log_id,
                                 end_log_id);

  uint64_t start_time = Helper::TimestampMs();
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() != pb::common::ENG_RAFT_STORE) {
    return butil::Status(pb::error::Errno::EINTERNAL, "Engine is not raft store.");
  }
  auto raft_kv_engine = std::dynamic_pointer_cast<RaftStoreEngine>(engine);
  auto node = raft_kv_engine->GetNode(vector_index->Id());
  if (node == nullptr) {
    return butil::Status(pb::error::Errno::ERAFT_NOT_FOUND, fmt::format("Not found node {}", vector_index->Id()));
  }

  auto log_stroage = Server::GetInstance()->GetLogStorageManager()->GetLogStorage(vector_index->Id());
  if (log_stroage == nullptr) {
    return butil::Status(pb::error::Errno::EINTERNAL, fmt::format("Not found log stroage {}", vector_index->Id()));
  }

  std::vector<pb::common::VectorWithId> vectors;
  vectors.reserve(10000);
  uint64_t last_log_id = vector_index->ApplyLogIndex();
  auto log_entrys = log_stroage->GetEntrys(start_log_id, end_log_id);
  for (const auto& log_entry : log_entrys) {
    auto raft_cmd = std::make_shared<pb::raft::RaftCmdRequest>();
    butil::IOBufAsZeroCopyInputStream wrapper(log_entry->data);
    CHECK(raft_cmd->ParseFromZeroCopyStream(&wrapper));
    for (auto& request : *raft_cmd->mutable_requests()) {
      switch (request.cmd_type()) {
        case pb::raft::VECTOR_ADD: {
          for (auto& vector : *request.mutable_vector_add()->mutable_vectors()) {
            vectors.push_back(vector);
          }

          if (vectors.size() == 10000) {
            vector_index->Upsert(vectors);
            vectors.resize(0);
          }
          break;
        }
        case pb::raft::VECTOR_DELETE: {
          if (!vectors.empty()) {
            vector_index->Upsert(vectors);
            vectors.resize(0);
          }
          std::vector<uint64_t> ids;
          for (auto vector_id : request.vector_delete().ids()) {
            ids.push_back(vector_id);
          }
          vector_index->Delete(ids);
          break;
        }
        default:
          break;
      }
    }

    last_log_id = log_entry->index;
  }
  if (!vectors.empty()) {
    vector_index->Upsert(vectors);
  }

  vector_index->SetApplyLogIndex(last_log_id);

  DINGO_LOG(INFO) << fmt::format(
      "Replay vector index {} from log id {} to log id {} finish, last_log_id {} elapsed time {}ms", vector_index->Id(),
      start_log_id, end_log_id, last_log_id, Helper::TimestampMs() - start_time);

  return butil::Status();
}

// Build vector index with original all data(store rocksdb).
std::shared_ptr<VectorIndex> VectorIndexManager::BuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);

  std::string start_key;
  std::string end_key;
  VectorCodec::EncodeVectorId(region->Id(), 0, start_key);
  VectorCodec::EncodeVectorId(region->Id(), UINT64_MAX, end_key);

  IteratorOptions options;
  options.lower_bound = start_key;
  options.upper_bound = end_key;

  auto vector_index = VectorIndexFactory::New(region->Id(), region->InnerRegion().definition().index_parameter());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("New vector index failed, vector id {}", region->Id());
    return nullptr;
  }

  // set snapshot_log_index and apply_log_index
  uint64_t snapshot_log_index = 0;
  uint64_t apply_log_index = 0;
  auto status = GetVectorIndexLogIndex(region->Id(), snapshot_log_index, apply_log_index);
  if (!status.ok()) {
    return nullptr;
  }
  vector_index->SetSnapshotLogIndex(snapshot_log_index);
  vector_index->SetApplyLogIndex(apply_log_index);

  DINGO_LOG(INFO) << fmt::format("Build vector index {}, snapshot_log_index({}) apply_log_index({})", region->Id(),
                                 snapshot_log_index, apply_log_index);

  // load vector data to vector index
  auto iter = raw_engine_->NewIterator(Constant::kStoreDataCF, options);
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorWithId vector;

    std::string key(iter->Key());
    vector.set_id(VectorCodec::DecodeVectorId(key));

    std::string value(iter->Value());
    if (!vector.mutable_vector()->ParseFromString(value)) {
      DINGO_LOG(WARNING) << fmt::format("vector ParseFromString failed, id {}", vector.id());
      continue;
    }

    if (vector.vector().float_values_size() <= 0) {
      DINGO_LOG(WARNING) << fmt::format("vector values_size error, id {}", vector.id());
      continue;
    }

    std::vector<pb::common::VectorWithId> vectors;
    vectors.push_back(vector);

    vector_index->Upsert(vectors);
  }

  DINGO_LOG(INFO) << fmt::format("Build vector index {} finish, snapshot_log_index({}) apply_log_index({})",
                                 region->Id(), snapshot_log_index, apply_log_index);

  return vector_index;
}

butil::Status VectorIndexManager::CheckAndSetRebuildStatus(store::RegionPtr region, bool is_initial_build) {
  // lock vector_index add/delete, to catch up and switch to new vector_index
  auto online_vector_index = GetVectorIndex(region->Id());
  if (online_vector_index == nullptr) {
    if (is_initial_build) {
      return butil::Status::OK();
    }

    DINGO_LOG(ERROR) << fmt::format("online_vector_index is not found, this is an illegal rebuild, stop, id {}",
                                    region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL,
                         "online_vector_index is not found, cannot do rebuild, try to set is_initial_build to true");
  }

  if (!online_vector_index->IsOnline()) {
    DINGO_LOG(WARNING) << fmt::format("online_vector_index is not online, skip rebuild, id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "online_vector_index is not online, skip rebuild");
  }

  if (online_vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_NORMAL &&
      online_vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_ERROR &&
      online_vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_NONE) {
    DINGO_LOG(WARNING) << fmt::format(
        "online_vector_index status is not normal/error/none, this is an illegal rebuild, stop, id {}, status {}",
        region->Id(), pb::common::RegionVectorIndexStatus_Name(online_vector_index->Status()));
    return butil::Status(pb::error::Errno::EINTERNAL,
                         "online_vector_index status is not normal/error/none, cannot do rebuild");
  }

  online_vector_index->SetStatus(pb::common::RegionVectorIndexStatus::VECTOR_INDEX_STATUS_REBUILDING);

  online_vector_index = GetVectorIndex(region->Id());
  if (online_vector_index == nullptr) {
    DINGO_LOG(ERROR) << fmt::format(
        "online_vector_index is not found after set_status, this is an illegal rebuild, stop, id {}", region->Id());
    return butil::Status(
        pb::error::Errno::EINTERNAL,
        "online_vector_index is not found after set_status, cannot do rebuild, try to set is_initial_build to true");
  }

  if (online_vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_REBUILDING) {
    DINGO_LOG(INFO) << fmt::format(
        "online_vector_index status is not rebuilding after set_status, this is an illegal rebuild, stop, id {}, "
        "status {}",
        region->Id(), pb::common::RegionVectorIndexStatus_Name(online_vector_index->Status()));

    return butil::Status(pb::error::Errno::EINTERNAL,
                         "online_vector_index status is not rebuilding after set_status, cannot do rebuild");
  }

  return butil::Status::OK();
}

// Rebuild vector index
butil::Status VectorIndexManager::RebuildVectorIndex(store::RegionPtr region, bool need_save, bool is_initial_build) {
  assert(region != nullptr);

  DINGO_LOG(INFO) << fmt::format("Rebuild vector index id {}", region->Id());

  // check and set rebuild status
  auto status = CheckAndSetRebuildStatus(region, is_initial_build);
  if (!status.ok()) {
    return status;
  }

  // lock vector_index add/delete, to catch up and switch to new vector_index
  auto online_vector_index = GetVectorIndex(region->Id());
  if (online_vector_index == nullptr && !is_initial_build) {
    DINGO_LOG(ERROR) << fmt::format("online_vector_index is not found, this is an illegal rebuild, stop, id {}",
                                    region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL,
                         "online_vector_index is not found, cannot do rebuild, try to set is_initial_build to true");
  }

  // update vector index status rebuilding
  online_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_REBUILDING);

  uint64_t start_time = Helper::TimestampMs();
  // Build vector index with original all data.
  auto vector_index = BuildVectorIndex(region);
  if (vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format("Build vector index failed, id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed");
  }

  DINGO_LOG(INFO) << fmt::format("Build vector index success, id {}, log_id {} elapsed time: {}ms", region->Id(),
                                 vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

  // we want to eliminate the impact of the blocking during replay wal in catch-up round
  // so save is done before replay wal first-round
  if (need_save) {
    start_time = Helper::TimestampMs();
    auto status = SaveVectorIndex(vector_index);
    if (!status.ok()) {
      DINGO_LOG(WARNING) << fmt::format("Save vector index {} failed, message: {}", region->Id(), status.error_str());
      return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed");
    }

    DINGO_LOG(INFO) << fmt::format("Save vector index snapshot success, id {}, snapshot_log_id {} elapsed time: {}ms",
                                   region->Id(), vector_index->SnapshotLogIndex(), Helper::TimestampMs() - start_time);
  }

  start_time = Helper::TimestampMs();
  // first ground replay wal
  status = ReplayWalToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("ReplayWal failed first-round, id {}, log_id {}", region->Id(),
                                    vector_index->ApplyLogIndex());
    return butil::Status(pb::error::Errno::EINTERNAL, "ReplayWal failed first-round");
  }

  DINGO_LOG(INFO) << fmt::format("ReplayWal success first-round, id {}, log_id {} elapsed time: {}ms", region->Id(),
                                 vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

  // set online_vector_index to offline, so it will reject all vector add/del, raft handler will usleep and try to
  // switch to new vector_index to add/del
  if (online_vector_index != nullptr) {
    online_vector_index->SetOffline();
  }

  start_time = Helper::TimestampMs();
  // second ground replay wal
  status = ReplayWalToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("ReplayWal failed catch-up round, id {}, log_id {}", region->Id(),
                                    vector_index->ApplyLogIndex());
    return status;
  }
  // set the new vector_index's status to NORMAL
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);

  DINGO_LOG(INFO) << fmt::format("ReplayWal success catch-up round, id {}, log_id {} elapsed time: {}ms", region->Id(),
                                 vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

  // set vector index to vector index map
  int ret = vector_indexs_.PutIfExists(region->Id(), vector_index);
  if (ret < 0) {
    DINGO_LOG(ERROR) << fmt::format(
        "ReplayWal catch-up round finish, but online_vector_index maybe delete by others, so stop to update "
        "vector_indexes map, id {}, log_id {}",
        region->Id(), vector_index->ApplyLogIndex());
    return butil::Status(pb::error::Errno::EINTERNAL,
                         "ReplayWal catch-up round finish, but online_vector_index "
                         "maybe delete by others, so stop to update vector_indexes map");
  }

  return butil::Status();
}

butil::Status VectorIndexManager::SaveVectorIndex(std::shared_ptr<VectorIndex> vector_index, bool can_overwrite) {
  assert(vector_index != nullptr);
  DINGO_LOG(INFO) << fmt::format("Save vector index id {}", vector_index->Id());

  // Update vector index status SNAPSHOTTING
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_SNAPSHOTTING);

  uint64_t snapshot_log_index = 0;
  auto status = VectorIndexSnapshot::SaveVectorIndexSnapshot(vector_index, snapshot_log_index, can_overwrite);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("Save vector index snapshot failed, id {}, errno: {}, errstr: {}",
                                    vector_index->Id(), status.error_code(), status.error_str());
    return status;
  } else {
    UpdateSnapshotLogIndex(vector_index, snapshot_log_index);
  }

  // update vector index status NORMAL
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
  DINGO_LOG(INFO) << fmt::format("Save vector index success, id {}", vector_index->Id());

  // Install vector index snapshot to followers.
  status = VectorIndexSnapshot::InstallSnapshotToFollowers(vector_index->Id());
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("Install snapshot to followers failed, region {} error {}", vector_index->Id(),
                                    status.error_str());
  }

  return butil::Status();
}

void VectorIndexManager::UpdateApplyLogIndex(std::shared_ptr<VectorIndex> vector_index, uint64_t log_index) {
  assert(vector_index != nullptr);

  vector_index->SetApplyLogIndex(log_index);
  meta_writer_->Put(TransformToKv(vector_index));
}

void VectorIndexManager::UpdateApplyLogIndex(uint64_t region_id, uint64_t log_index) {
  auto vector_index = GetVectorIndex(region_id);
  if (vector_index != nullptr) {
    UpdateApplyLogIndex(vector_index, log_index);
  }
}

void VectorIndexManager::UpdateSnapshotLogIndex(std::shared_ptr<VectorIndex> vector_index, uint64_t log_index) {
  assert(vector_index != nullptr);

  vector_index->SetSnapshotLogIndex(log_index);
  meta_writer_->Put(TransformToKv(vector_index));
}

void VectorIndexManager::UpdateSnapshotLogIndex(uint64_t region_id, uint64_t log_index) {
  auto vector_index = GetVectorIndex(region_id);
  if (vector_index != nullptr) {
    UpdateSnapshotLogIndex(vector_index, log_index);
  }
}

std::shared_ptr<pb::common::KeyValue> VectorIndexManager::TransformToKv(std::any obj) {
  auto vector_index = std::any_cast<std::shared_ptr<VectorIndex>>(obj);
  auto kv = std::make_shared<pb::common::KeyValue>();
  kv->set_key(GenKey(vector_index->Id()));
  kv->set_value(
      VectorCodec::EncodeVectorIndexLogIndex(vector_index->SnapshotLogIndex(), vector_index->ApplyLogIndex()));

  return kv;
}

void VectorIndexManager::TransformFromKv(const std::vector<pb::common::KeyValue>& kvs) {
  for (const auto& kv : kvs) {
    uint64_t region_id = ParseRegionId(kv.key());
    uint64_t snapshot_log_index = 0;
    uint64_t apply_log_index = 0;

    VectorCodec::DecodeVectorIndexLogIndex(kv.value(), snapshot_log_index, apply_log_index);

    DINGO_LOG(INFO) << fmt::format("TransformFromKv, region_id {}, snapshot_log_index {}, apply_log_index {}",
                                   region_id, snapshot_log_index, apply_log_index);

    auto vector_index = GetVectorIndex(region_id);
    if (vector_index != nullptr) {
      vector_index->SetSnapshotLogIndex(snapshot_log_index);
      vector_index->SetApplyLogIndex(apply_log_index);
    }
  }
}

butil::Status VectorIndexManager::GetVectorIndexLogIndex(uint64_t region_id, uint64_t& snapshot_log_index,
                                                         uint64_t& apply_log_index) {
  auto kv = meta_reader_->Get(GenKey(region_id));
  if (kv == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("Get vector index log id failed, region_id {}", region_id);
    return butil::Status(pb::error::EINTERNAL, "Get vector index log id failed, region_id %lu", region_id);
  }

  if (kv->value().empty()) {
    return butil::Status();
  }

  auto ret = VectorCodec::DecodeVectorIndexLogIndex(kv->value(), snapshot_log_index, apply_log_index);
  if (ret < 0) {
    DINGO_LOG(ERROR) << fmt::format("Decode vector index log id failed, region_id {}", region_id);
    return butil::Status(pb::error::EINTERNAL, "Decode vector index log id failed, region_id %lu", region_id);
  }

  return butil::Status();
}

butil::Status VectorIndexManager::ScrubVectorIndex() {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  if (store_meta_manager == nullptr) {
    return butil::Status(pb::error::Errno::EINTERNAL, "Get store meta manager failed");
  }

  auto regions = store_meta_manager->GetStoreRegionMeta()->GetAllAliveRegion();
  if (regions.empty()) {
    DINGO_LOG(INFO) << "No alive region, skip scrub vector index";
    return butil::Status::OK();
  }

  DINGO_LOG(INFO) << "Scrub vector index start, alive region_count is " << regions.size();

  for (const auto& region : regions) {
    auto vector_index = GetVectorIndex(region->Id());
    if (vector_index == nullptr) {
      continue;
    }

    uint64_t last_snapshot_log_id = VectorIndexSnapshot::GetLastVectorIndexSnapshotLogId(vector_index->Id());
    if (last_snapshot_log_id == UINT64_MAX) {
      DINGO_LOG(ERROR) << fmt::format("Get last vector index snapshot log id failed, region_id {}", region->Id());
      continue;
    }

    auto last_save_log_behind = vector_index->ApplyLogIndex() - last_snapshot_log_id;

    bool need_rebuild = false;
    vector_index->NeedToRebuild(need_rebuild, last_save_log_behind);

    bool need_save = false;
    vector_index->NeedToSave(need_save, last_save_log_behind);

    if (need_rebuild || need_save) {
      DINGO_LOG(INFO) << fmt::format("vector index {} need rebuild({}) and need save({})", region->Id(), need_rebuild,
                                     need_save);
      auto status = ScrubVectorIndex(region, need_rebuild, need_save);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("Scrub vector index failed, id {} error: {}", region->Id(), status.error_str());
        continue;
      }
    }
  }

  return butil::Status::OK();
}

butil::Status VectorIndexManager::ScrubVectorIndex(store::RegionPtr region, bool need_rebuild, bool need_save) {
  // check vector index status
  auto vector_index = GetVectorIndex(region->Id());
  if (vector_index == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("Get vector index failed, vector index id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "Get vector index failed");
  }
  auto status = vector_index->Status();
  if (status != pb::common::RegionVectorIndexStatus::VECTOR_INDEX_STATUS_NORMAL) {
    DINGO_LOG(INFO) << fmt::format("vector index status is not normal, skip to ScrubVectorIndex, region_id {}",
                                   region->Id());
    return butil::Status::OK();
  }

  if (need_rebuild) {
    DINGO_LOG(INFO) << fmt::format("need rebuild, do rebuild vector index, region_id {}", region->Id());
    auto status = RebuildVectorIndex(region);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("Rebuild vector index failed, region_id {} error {}", region->Id(),
                                      status.error_str());
      return status;
    }
  } else if (need_save) {
    DINGO_LOG(INFO) << fmt::format("need save, do save vector index, region_id {}", region->Id());
    auto status = SaveVectorIndex(vector_index);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("Save vector index failed, region_id {} error {}", region->Id(),
                                      status.error_str());
      return status;
    }
  }

  DINGO_LOG(INFO) << fmt::format("ScrubVectorIndex success, region_id {}", region->Id());

  return butil::Status::OK();
}

}  // namespace dingodb