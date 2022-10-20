#ifndef zstd_proxy_uring_H
#define zstd_proxy_uring_H

#include "zstd-proxy.h"

void zstd_proxy_uring_options(zstd_proxy_options *options);
int zstd_proxy_uring_run(zstd_proxy_connection* connection);

#endif
