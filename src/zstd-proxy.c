#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#ifndef ENABLE_URING
#define ENABLE_URING 1
#endif

#ifndef DISABLE_ZSTD
#define DISABLE_ZSTD 0
#endif

#ifndef __linux__
#undef ENABLE_URING
#define ENABLE_URING 0
#endif

#if ENABLE_URING
#include "zstd-proxy-uring.h"
#endif

#include "zstd-proxy-posix.h"
#include "zstd-proxy-utils.h"

typedef struct {
    zstd_proxy *proxy;

    int compress_error;
    int decompress_error;
} zstd_proxy_thread;

static inline int zstd_proxy_remove_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        log_error("error getting socket flags on fd %d: %s", fd, strerror(errno));

        return errno;
    }

    if (flags & O_NONBLOCK) {
        int error = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        if (error == -1) {
            log_error("error setting socket flags on fd %d: %s", fd, strerror(errno));

            return errno;
        }
    }

    return 0;
}

static inline int zstd_proxy_prepare(int listen_fd, int connect_fd) {
    int error = zstd_proxy_remove_nonblock(listen_fd);

    if (error != 0) {
        return error;
    }

    error = zstd_proxy_remove_nonblock(connect_fd);

    if (error != 0) {
        return error;
    }

    return 0;
}

static inline bool zstd_proxy_is_socket(int fd) {
    int type;
    socklen_t length = sizeof(type);

    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0;
}

static inline int zstd_proxy_platform_run(zstd_proxy_connection* connection) {
#if ENABLE_URING
    if (connection->options->io_uring.enabled) {
        return zstd_proxy_uring_run(connection);
    }
#endif

    return zstd_proxy_posix_run(connection);
}

int zstd_proxy_io(
    zstd_proxy_thread *data,
    int (*process)(void *process_data, ZSTD_inBuffer *input, ZSTD_outBuffer *output),
    void *process_data,
    bool invert
) {
    zstd_proxy *proxy = data->proxy;
    zstd_proxy_options *options = &proxy->options;
    zstd_proxy_descriptor *listen = invert ? &proxy->connect : &proxy->listen;
    zstd_proxy_descriptor *connect = invert ? &proxy->listen : &proxy->connect;
    zstd_proxy_connection connection = {
        .listen = listen,
        .connect = connect,
        .options = options,
        .process = process,
        .process_data = process_data,
    };

    int listen_fd = listen->fd;
    int connect_fd = connect->fd;

    if (!zstd_proxy_is_socket(listen_fd)) {
        return 0;
    }

    int error = zstd_proxy_platform_run(&connection);

    options->stop = true;

    shutdown(listen_fd, SHUT_RDWR);
    shutdown(connect_fd, SHUT_RDWR);

    return error;
}

int zstd_proxy_compress_stream(void *ctx, ZSTD_inBuffer *input, ZSTD_outBuffer *output) {
    if (ctx == NULL) {
        output->pos = input->pos = input->size;

        memcpy(output->dst, input->src, input->size);
    } else {
        size_t size = ZSTD_compressStream2(ctx, output, input, ZSTD_e_flush);

        if (ZSTD_isError(size)) {
            log_error("error compressing data: %s", ZSTD_getErrorName(size));

            return size;
        }
    }

    return 0;
}

int zstd_proxy_decompress_stream(void *ctx, ZSTD_inBuffer *input, ZSTD_outBuffer *output) {
    if (ctx == NULL) {
        output->pos = input->pos = input->size;

        memcpy(output->dst, input->src, input->size);
    } else {
        size_t size = ZSTD_decompressStream(ctx, output, input);

        if (ZSTD_isError(size)) {
            log_error("error decompressing data: %s", ZSTD_getErrorName(size));

            return size;
        }
    }

    return 0;
}

void *zstd_proxy_compress_thread(void *data_ptr) {
    int error = 0;
    zstd_proxy_thread *data = data_ptr;
    ZSTD_CCtx *ctx = data->proxy->options.zstd.enabled ? ZSTD_createCCtx() : NULL;

    if (ctx != NULL) {
        error = ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, data->proxy->options.zstd.level);

        if (ZSTD_isError(error)) {
            log_error("failed to set compression level: %s", ZSTD_getErrorName(error));

            goto cleanup;
        }
    }

    error = zstd_proxy_io(data, zstd_proxy_compress_stream, ctx, false);

    cleanup:

    if (ctx != NULL) {
        ZSTD_freeCCtx(ctx);
    }

    data->compress_error = error;

    return NULL;
}

void *zstd_proxy_decompress_thread(void *data_ptr) {
    int error = 0;
    zstd_proxy_thread *data = data_ptr;
    ZSTD_DCtx *ctx = data->proxy->options.zstd.enabled ? ZSTD_createDCtx() : NULL;

    error = zstd_proxy_io(data, zstd_proxy_decompress_stream, ctx, true);

    if (ctx != NULL) {
        ZSTD_freeDCtx(ctx);
    }

    data->decompress_error = error;

    return NULL;
}

void zstd_proxy_init(zstd_proxy *proxy) {
    proxy->listen.fd = -1;
    proxy->listen.data = NULL;
    proxy->listen.data_length = 0;

    proxy->connect.fd = -1;
    proxy->connect.data = NULL;
    proxy->connect.data_length = 0;

    proxy->options.stop = false;
    proxy->options.buffer_size = 4 * 1024 * 1024;

    proxy->options.zstd.enabled = true;
    proxy->options.zstd.level = 1;

    proxy->options.io_uring.enabled = true;
    proxy->options.io_uring.depth = 4;
    proxy->options.io_uring.zero_copy = true;
    proxy->options.io_uring.fixed_buffers = true;
}

/** Connection thread. */
int zstd_proxy_run(zstd_proxy *proxy) {
    int error = 0;
    int listen_fd = proxy->listen.fd;
    int connect_fd = proxy->connect.fd;
    pthread_t compress_thread_id = 0, decompress_thread_id = 0;
    zstd_proxy_thread *thread = malloc(sizeof(zstd_proxy_thread));

    thread->proxy = proxy;
    thread->compress_error = 0;
    thread->decompress_error = 0;

    error = zstd_proxy_prepare(listen_fd, connect_fd);

    if (error != 0) {
        goto cleanup;
    }

#if ENABLE_URING
    if (proxy->options.io_uring.enabled) {
        zstd_proxy_uring_options(&proxy->options);
    }
#endif

    error = pthread_create(&compress_thread_id, NULL, zstd_proxy_compress_thread, thread);

    if (error != 0) {
        log_error("error creating read thread: %s", strerror(error));

        goto cleanup;
    }

    error = pthread_create(&decompress_thread_id, NULL, zstd_proxy_decompress_thread, thread);

    if (error != 0) {
        log_error("error creating write thread: %s", strerror(error));

        goto cleanup;
    }

    cleanup:

    if (error != 0) {
        shutdown(listen_fd, SHUT_RDWR);
        shutdown(connect_fd, SHUT_RDWR);
    } else {
        if (compress_thread_id != 0) {
            error = pthread_join(compress_thread_id, NULL);

            if (error != 0) {
                log_error("error running compress thread: %s", strerror(error));
            }
        }

        if (decompress_thread_id != 0) {
            error = pthread_join(decompress_thread_id, NULL);

            if (error != 0) {
                log_error("error running decompress thread: %s", strerror(error));
            }
        }
    }

    int compress_error = thread->compress_error;
    int decompress_error = thread->decompress_error;

    close(listen_fd);
    close(connect_fd);
    free(thread);

    if (compress_error != 0) {
        return compress_error;
    }

    if (decompress_error != 0) {
        return decompress_error;
    }

    return error;
}
