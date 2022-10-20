#ifndef zstd_proxy_utils_H
#define zstd_proxy_utils_H

#include <stdio.h>

#define log_buffer(fmt, buffer, size, ...) \
    do { \
        printf(fmt, ##__VA_ARGS__); \
        for(size_t i = 0; i < size; i++) printf("%02X ", ((char *)buffer)[i]); \
        printf("\n"); \
    } while(0)

#define log_error(fmt, ...) fprintf(stderr, "error: %s in " __FILE__ ": " fmt "\n", __func__, ##__VA_ARGS__)

#if DEBUG
#define debug_assert(x) assert(x)
#define log_debug(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
#define debug_assert(x)
#define log_debug(fmt, ...)
#endif

#endif
