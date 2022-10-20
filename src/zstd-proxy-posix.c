#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "zstd-proxy-posix.h"

int zstd_proxy_posix_run(zstd_proxy_connection* connection) {
    int error = 0;
    int recv_fd = connection->listen->fd;
    int send_fd = connection->connect->fd;
    size_t size = connection->options->buffer_size;
    ZSTD_inBuffer input = { .src = malloc(size) };
    ZSTD_outBuffer output = { .dst = malloc(size), .size = size };

    if (input.src == NULL || output.dst == NULL) {
        error = errno;

        goto cleanup;
    }

    while (!error && !connection->options->stop) {
        ssize_t received = recv(recv_fd, (void *)input.src, size, 0);

        if (received == 0) {
            break;
        } else if (received < 0) {
            error = errno;

            break;
        }

        input.pos = 0;
        input.size = received;

        while (input.pos < input.size) {
            output.pos = 0;

            error = connection->process(connection->process_data, &input, &output);

            if (error != 0) {
                break;
            }

            ssize_t sent = send(send_fd, output.dst, output.pos, 0);

            if (sent < 0) {
                error = errno;

                break;
            }
        }
    }

    cleanup:

    free((void *)input.src);
    free((void *)output.dst);

    return error;
}
