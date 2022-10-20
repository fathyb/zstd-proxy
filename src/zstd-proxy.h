#ifndef zstd_proxy_H
#define zstd_proxy_H

#include <stdlib.h>
#include <stdbool.h>

#include <zstd.h>

typedef struct {
    int fd;
    void* data;
    size_t data_length;
} zstd_proxy_descriptor;

typedef struct {
    bool enabled;

    size_t depth;
    bool zero_copy;
    bool fixed_buffers;
} zstd_proxy_io_uring_options;

typedef struct {
    bool enabled;

    size_t level;
} zstd_proxy_zstd_options;

typedef struct {
    bool stop;
    size_t buffer_size;

    zstd_proxy_zstd_options zstd;
    zstd_proxy_io_uring_options io_uring;
} zstd_proxy_options;

typedef struct {
    zstd_proxy_options options;
    zstd_proxy_descriptor listen;
    zstd_proxy_descriptor connect;
} zstd_proxy;

typedef int (*zstd_proxy_process_callback)(void *process_data, ZSTD_inBuffer *input, ZSTD_outBuffer *output);

typedef struct {
    zstd_proxy_options *options;
    zstd_proxy_descriptor *listen;
    zstd_proxy_descriptor *connect;

    zstd_proxy_process_callback process;
    void *process_data;
} zstd_proxy_connection;

void zstd_proxy_init(zstd_proxy *proxy);
int zstd_proxy_run(zstd_proxy *proxy);

#endif
