package io.dingodb.sdk.service.caller;

import io.dingodb.sdk.service.Caller;
import io.dingodb.sdk.service.Service;
import io.dingodb.sdk.service.entity.Message;
import io.dingodb.sdk.service.entity.Message.Request;
import io.dingodb.sdk.service.entity.Message.Response;
import io.grpc.CallOptions;
import io.grpc.Channel;
import io.grpc.ClientCall;
import io.grpc.Metadata;
import io.grpc.MethodDescriptor;
import io.grpc.StatusRuntimeException;
import io.grpc.stub.ClientCalls;
import lombok.AllArgsConstructor;
import lombok.extern.slf4j.Slf4j;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Supplier;

@Slf4j
@AllArgsConstructor
public class RpcCaller<S extends Service<S>> implements Caller<S>, InvocationHandler {

    private static final Map<String, RpcHandler> handlers = new ConcurrentHashMap<>();

    public static <REQ extends Request, RES extends Response> void addHandler(
        io.dingodb.sdk.service.RpcHandler<REQ, RES> handler
    ) {
        MethodDescriptor method = handler.matchMethod();
        handlers.computeIfAbsent(method.getFullMethodName(), n -> new RpcHandler<>(method)).addHandler(handler);
    }

    public static <REQ extends Request, RES extends Response> void removeHandler(
        io.dingodb.sdk.service.RpcHandler<REQ, RES> handler
    ) {
        MethodDescriptor method = handler.matchMethod();
        handlers.computeIfAbsent(method.getFullMethodName(), n -> new RpcHandler<>(method)).removeHandler(handler);
    }

    private final Channel channel;
    private final CallOptions options;
    private final S service;

    public RpcCaller(Channel channel, CallOptions options, Class<S> genericClass) {
        this.channel = channel;
        this.options = options;
        this.service = proxy(genericClass);
    }

    private S proxy(Class<S> genericType) {
        for (Class<?> child : genericType.getClasses()) {
            if (!child.getSuperclass().equals(genericType)) {
                try {
                    return (S) child.getConstructor(Caller.class).newInstance(this);
                } catch (Exception e) {
                    throw new RuntimeException(e);
                }
            }
        }
        throw new RuntimeException("Not found " + genericType.getName() + " impl.");
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        return method.invoke(service, args);
    }

    @Override
    public <REQ extends Request, RES extends Response> RES call(
        MethodDescriptor<REQ, RES> method, Supplier<REQ> provider
    ) {
        return call(method, provider.get());
    }

    @Override
    public <REQ extends Request, RES extends Response> RES call(
        MethodDescriptor<REQ, RES> method, REQ request
    ) {
        return call(method, request, options, channel, System.identityHashCode(request));
    }

    @Override
    public <REQ extends Request, RES extends Response> RES call(
        MethodDescriptor<REQ, RES> method, long requestId, Supplier<REQ> provider
    ) {
        REQ request = provider.get();
        return call(method, request, options, channel, System.identityHashCode(request));
    }

    @Override
    public <REQ extends Request, RES extends Response> RES call(
        MethodDescriptor<REQ, RES> method, long requestId, REQ request
    ) {
        return call(method, request, options, channel, System.identityHashCode(request));
    }

    protected static <REQ extends Message, RES extends Response> RpcFuture<RES> asyncCall(
        MethodDescriptor<REQ, RES> method, REQ request, CallOptions options, Channel channel
    ) {
        RpcFuture<RES> future = new RpcFuture<>();
        ClientCall<REQ, RES> call = channel.newCall(method, options);
        call.start(future.listener, new Metadata());
        call.request(2);
        call.sendMessage(request);
        call.halfClose();
        return future;
    }

    public static <REQ extends Request, RES extends Response> RES call(
        MethodDescriptor<REQ, RES> method, REQ request, CallOptions options, Channel channel, long trace
    ) {
        String methodName = method.getFullMethodName();
        RpcHandler<REQ, RES> handler = handlers.computeIfAbsent(methodName, n -> new RpcHandler<>(method));

        handler.enter(request, options, channel == null ? null : channel.authority(), trace);
        if (channel == null) {
            return null;
        }
        handler.before(request, options, channel.authority(), trace);
        RES response;
        try {
            response = ClientCalls.blockingUnaryCall(channel, method, options, request);
        } catch (StatusRuntimeException e) {
            handler.onNonResponse(request, options, channel.authority(), trace, e.getMessage());
            return null;
        }

        handler.after(request, response, options, channel.authority(), trace);
        return response;
    }

}
