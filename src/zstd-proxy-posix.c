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
#include "zstd-proxy-utils.h"


int zstd_proxy_posix_process(zstd_proxy_connection* connection, ZSTD_inBuffer *input, ZSTD_outBuffer *output) {
    int error = 0;

    while (input->pos < input->size) {
        output->pos = 0;

        error = connection->process(connection->process_data, input, output);

        if (error != 0) {
            break;
        }

        ssize_t sent = send(connection->connect->fd, output->dst, output->pos, 0);

        if (sent < 0) {
            error = errno;
            log_error("error writing to fd %d: %s", connection->connect->fd, strerror(error));

            break;
        }
    }

    return error;
}

int zstd_proxy_posix_run(zstd_proxy_connection* connection) {
    int error = 0;
    int recv_fd = connection->listen->fd;
    size_t size = connection->options->buffer_size;
    ZSTD_inBuffer input = { .src = malloc(size) };
    ZSTD_outBuffer output = { .dst = malloc(size), .size = size };

    if (input.src == NULL || output.dst == NULL) {
        error = errno;

        goto cleanup;
    }

    if (connection->listen->data_length > 0) {
        const void *buffer = input.src;

        input.pos = 0;
        input.src = connection->listen->data;
        input.size = connection->listen->data_length;

        error = zstd_proxy_posix_process(connection, &input, &output);

        input.src = buffer;
    }

    while (!error && !connection->options->stop) {
        ssize_t received = recv(recv_fd, (void *)input.src, size, 0);

        if (received == 0) {
            break;
        } else if (received < 0) {
            error = errno;
            log_error("error reading fd %d: %s", recv_fd, strerror(error));

            break;
        }

        input.pos = 0;
        input.size = received;

        error = zstd_proxy_posix_process(connection, &input, &output);
    }

    cleanup:

    free((void *)input.src);
    free((void *)output.dst);

    return error;
}
