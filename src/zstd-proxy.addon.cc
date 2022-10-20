#include <nan.h>
#include <iostream>
#include <signal.h>
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" {
    #include "zstd-proxy.h"
    #include "zstd-proxy-utils.h"
}

namespace zstdProxy {
    using v8::Context;
    using v8::FunctionCallbackInfo;
    using v8::Isolate;
    using v8::Local;
    using v8::Object;
    using v8::Value;

    struct thread_data {
        int error;
        uv_async_t async;
        Nan::AsyncResource async_resource;
        Nan::Callback callback;
        zstd_proxy proxy;

        thread_data(): async_resource("ZstdProxy") {}
    };

    void HandleAbortSignal(int sig) {
        void *array[64];
        size_t size = backtrace(array, 64);

        log_error("Fatal error: Signal %d; %lu frames found:", sig, size);
        backtrace_symbols_fd(array, size, STDERR_FILENO);
        exit(128 + sig);
    }

    void *Start(void *data_ptr) {
        auto data = (thread_data *)data_ptr;

        data->error = zstd_proxy_run(&data->proxy);

        uv_async_send(&data->async);

        return nullptr;
    }

    static inline Local<Value> GetOption(Local<Context> context, Local<Object> options, const char *name) {
        return options->
            Get(context, v8::String::NewFromUtf8(context->GetIsolate(), name).ToLocalChecked()).
            ToLocalChecked();
    }

    static inline bool GetBoolOption(Local<Context> context, Local<Object> options, const char *name, bool defaultValue) {
        auto option = GetOption(context, options, name);

        if (option->IsUndefined()) {
            return defaultValue;
        } else {
            return option->BooleanValue(context->GetIsolate());
        }
    }

    static inline unsigned GetUnsignedOption(Local<Context> context, Local<Object> options, const char *name, unsigned defaultValue) {
        auto option = GetOption(context, options, name);

        if (option->IsUndefined()) {
            return defaultValue;
        } else {
            return option->NumberValue(context).ToChecked();
        }
    }

#if DEBUG
    bool registered = false;
#endif

    void Proxy(const FunctionCallbackInfo<Value> &args) {
#if DEBUG
        if (!registered) {
            registered = true;

            signal(SIGSEGV, HandleAbortSignal);
            signal(SIGABRT, HandleAbortSignal);
        }
#endif

        Isolate *isolate = args.GetIsolate();
        Local<Context> context = isolate->GetCurrentContext();
        auto *data = new thread_data();
        auto async = &data->async;

        zstd_proxy_init(&data->proxy);

        async->data = data;
        data->proxy.listen.fd = args[0]->NumberValue(context).ToChecked();
        data->proxy.connect.fd = args[1]->NumberValue(context).ToChecked();

        if (!args[2]->IsUndefined()) {
            data->proxy.listen.data = node::Buffer::Data(args[2]);
            data->proxy.listen.data_length = node::Buffer::Length(args[2]);
        }

        if (!args[3]->IsUndefined()) {
            data->proxy.connect.data = node::Buffer::Data(args[3]);
            data->proxy.connect.data_length = node::Buffer::Length(args[3]);
        }

        if (!args[4]->IsUndefined()) {
            auto options = args[4]->ToObject(context).ToLocalChecked();
            auto zstd = GetBoolOption(context, options, "zstd", true);
            auto io_uring = GetBoolOption(context, options, "io_uring", true);
            auto buffer_size = GetUnsignedOption(context, options, "buffer_size", 0);

            data->proxy.options.zstd.enabled = zstd;
            data->proxy.options.io_uring.enabled = io_uring;

            if (zstd) {
                auto level = GetUnsignedOption(context, options, "zstd_level", 1);
                
                data->proxy.options.zstd.level = level;
            }

            if (buffer_size > 0) {
                data->proxy.options.buffer_size = buffer_size;
            }

            if (io_uring) {
                auto depth = GetUnsignedOption(context, options, "io_uring_depth", 0);
                auto zero_copy = GetBoolOption(context, options, "io_uring_zero_copy", true);
                auto fixed_buffers = GetBoolOption(context, options, "io_uring_fixed_buffers", true);

                data->proxy.options.io_uring.zero_copy = zero_copy;
                data->proxy.options.io_uring.fixed_buffers = fixed_buffers;

                if (depth > 0) {
                    data->proxy.options.io_uring.depth = depth;
                }
            }
        }

        data->callback.Reset(args[5].As<v8::Function>());

        uv_async_init(uv_default_loop(), async, [](uv_async_t *async) {
            Isolate *isolate = Isolate::GetCurrent();
            v8::HandleScope scope(isolate);
            auto data = (thread_data *)async->data;

            if (data->error == 0) {
                data->callback.Call(0, nullptr, &data->async_resource);
            } else {
                Local<Value> argv[] = { v8::Number::New(isolate, data->error) };

                data->callback.Call(1, argv, &data->async_resource);
            }

            uv_close((uv_handle_t *)async, [](uv_handle_t *handle) {
                auto data = (thread_data *)handle->data;

                delete data;
            });
        });

        pthread_t thread_id = 0;

        int error = pthread_create(&thread_id, nullptr, Start, data);

        if (error != 0) {
            std::cerr << "error creating read thread: " << strerror(errno) << std::endl;

            exit(EXIT_FAILURE);
        }
    }

    void Initialize(Local<Object> exports, v8::Local<v8::Value>, void *) {
        NODE_SET_METHOD(exports, "proxy", Proxy);
    }

    NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)
}
