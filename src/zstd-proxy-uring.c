#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <liburing.h>

#include "zstd-proxy-uring.h"
#include "zstd-proxy-utils.h"

typedef struct zstd_proxy_uring_queue zstd_proxy_uring_queue;
typedef struct zstd_proxy_uring_buffer zstd_proxy_uring_buffer;

typedef enum {
    zstd_proxy_uring_recv_buffer,
    zstd_proxy_uring_send_buffer
} zstd_proxy_uring_buffer_type;

struct zstd_proxy_uring_buffer {
    /** Incremental ID to sort submission events. */
    size_t id;
    /** Buffer type: recv or send. */
    zstd_proxy_uring_buffer_type type;
    /** Size of `buffer`. */
    size_t size;
    /** I/O fixed buffer index. */
    size_t index;
    /** Start position in `buffer`. */
    size_t offset;
    /** Ring buffer data. */
    char *data;
    /** Ring buffer data. */
    char *queue_data;

    /** Return code. */
    int result;
    /** `true` if `buffer` is being filled by the kernel. */
    bool running;
    /** `true` if the `buffer` is not used. */
    bool available;

    /** Reference to the queue that owns this buffer. */
    zstd_proxy_uring_queue *queue;
};

struct zstd_proxy_uring_queue {
    /** Incremental ID to sort submission events. */
    size_t id;
    /** `buffers` size. */
    size_t size;
    /** How many items are currently running. */
    size_t running;
    /** `buffers[].data` size. */
    size_t buffer_size;

    /** Pointer passed to `process. */
    void *process_data;
    /** Function pointer called to transform `input` into `output`. */
    int (*process)(void *process_data, ZSTD_inBuffer *input, ZSTD_outBuffer *output);

    struct io_uring uring;
    zstd_proxy_connection *connection;
    zstd_proxy_uring_buffer buffers[];
};

#define zstd_proxy_uring_foreach(queue, type) \
    for (zstd_proxy_uring_buffer *buffer = NULL, *buffers = queue->buffers; buffers != NULL; buffers = NULL) \
        for ( \
            size_t \
                queue_size = queue->size, \
                buffer_index = type * queue_size, \
                queue_end = buffer_index + queue_size; \
            buffer = &buffers[buffer_index], buffer_index < queue_end; \
            buffer_index++ \
        )

void zstd_proxy_uring_options(zstd_proxy_options *options) {
    static bool configured = false;
    static bool enabled = false;
    static bool zero_copy = false;
    static bool fixed_buffers = false;

    // Try to run the probe only once
    if (!configured) {
        struct io_uring_probe *probe = io_uring_get_probe();

        if (probe == NULL) {
            log_error("failed to get io_uring probe, support disabled");
        } else {
            if (
                io_uring_opcode_supported(probe, IORING_OP_READ) &&
                io_uring_opcode_supported(probe, IORING_OP_WRITE)
            ) {
                enabled = true;

                if (
                    io_uring_opcode_supported(probe, IORING_OP_READ_FIXED) &&
                    io_uring_opcode_supported(probe, IORING_OP_WRITE_FIXED)
                ) {
                    fixed_buffers = true;

                    if (io_uring_opcode_supported(probe, IORING_OP_SEND_ZC)) {
                        zero_copy = true;
                    }
                }
            }

            io_uring_free_probe(probe);
        }

        configured = true;
    }

    if (!enabled) {
        log_debug("disabling io_uring after failed probe");

        options->io_uring.enabled = false;
    } else {
        if (!zero_copy) {
            options->io_uring.zero_copy = false;
        }

        if (!fixed_buffers) {
            options->io_uring.fixed_buffers = false;
        }
    }
}

static inline void zstd_proxy_uring_destroy(zstd_proxy_uring_queue *queue) {
    if (queue == NULL) {
        return;
    }

    io_uring_queue_exit(&queue->uring);
    free(queue);
}

int zstd_proxy_uring_create(zstd_proxy_uring_queue **queue_ptr, zstd_proxy_connection *connection) {
    int error = 0;
    size_t size = connection->options->io_uring.depth;
    size_t depth = size * 2;
    size_t buffer_size = connection->options->buffer_size;
    size_t ring_size = sizeof(zstd_proxy_uring_queue) + sizeof(zstd_proxy_uring_buffer) * depth;
    char *mem = malloc(ring_size + depth * buffer_size);
    char *buffer_mem = &mem[ring_size];
    zstd_proxy_uring_queue *queue = (zstd_proxy_uring_queue *)mem;
    struct io_uring *uring = &queue->uring;
    struct iovec vecs[depth];

    if (queue == NULL) {
        error = errno;
        log_error("failed to alloc io_uring memory: %s", strerror(error));

        goto cleanup;
    }

    queue->id = 0;
    queue->size = size;
    queue->running = 0;
    queue->connection = connection;
    queue->buffer_size = buffer_size;

    for (size_t i = 0; i < depth; i++) {
        char *data = &buffer_mem[i * buffer_size];
        struct iovec *vec = &vecs[i];
        zstd_proxy_uring_buffer *buffer = &queue->buffers[i];

        buffer->id = 0;
        buffer->size = buffer_size;
        buffer->data = data;
        buffer->queue_data = data;
        buffer->type = i < size ? zstd_proxy_uring_recv_buffer : zstd_proxy_uring_send_buffer;
        buffer->queue = queue;
        buffer->index = i;
        buffer->running = false;
        buffer->available = true;

        vec->iov_len = buffer_size;
        vec->iov_base = buffer->data;
    }

    error = io_uring_queue_init(depth, uring, 0);

    if (error != 0) {
        error = -error;
        log_error("failed to init io_uring queue: %s", strerror(error));

        goto cleanup;
    }

    if (connection->options->io_uring.fixed_buffers) {
        error = io_uring_register_buffers(uring, vecs, depth);

        if (error != 0) {
            error = -error;
            log_error("failed to init io_uring buffers: %s", strerror(error));

            goto cleanup;
        }
    }

    cleanup:

    *queue_ptr = queue;

    if (error != 0) {
        zstd_proxy_uring_destroy(queue);
    }

    return error;
}

static inline zstd_proxy_uring_buffer *zstd_proxy_uring_get(zstd_proxy_uring_queue *queue, zstd_proxy_uring_buffer_type type) {
    // Find the oldest waiting buffer
    zstd_proxy_uring_buffer *next = NULL;

    zstd_proxy_uring_foreach(queue, type) {
        if (!buffer->available) {
            if (next == NULL || next->id > buffer->id) {
                next = buffer;
            }
        }
    }

    debug_assert(next == NULL || next->type == type);

    return next;
}

/** Send a recv request */
int zstd_proxy_uring_submit_recv(zstd_proxy_uring_queue *queue) {
    zstd_proxy_uring_buffer *recv_buffer = NULL;

    zstd_proxy_uring_foreach(queue, zstd_proxy_uring_recv_buffer) {
        if (buffer->running) {
            return 0;
        } else if (recv_buffer == NULL && buffer->available) {
            recv_buffer = buffer;
        }
    }

    // We don't have any memory left to fill
    if (recv_buffer == NULL) {
        return 0;
    }

    struct io_uring *uring = &queue->uring;
    struct io_uring_sqe *sqe = io_uring_get_sqe(uring);

    if (sqe == NULL) {
        log_error("failed to get uring write sqe");

        return EIO;
    }

    debug_assert(!recv_buffer->running);

    recv_buffer->id = ++queue->id;
    recv_buffer->running = true;
    recv_buffer->available = false;
    recv_buffer->data = recv_buffer->queue_data;

    zstd_proxy_connection *connection = queue->connection;
    int fd = connection->listen->fd;

    // log_debug("scheduling recv on fd %d, buffer=%d", fd, recv_buffer->index);

    if (connection->options->io_uring.fixed_buffers) {
        io_uring_prep_read_fixed(sqe, fd, recv_buffer->data, queue->buffer_size, 0, recv_buffer->index);
    } else {
        io_uring_prep_read(sqe, fd, recv_buffer->data, queue->buffer_size, 0);
    }

    io_uring_sqe_set_data(sqe, recv_buffer);

    int error = io_uring_submit(uring);

    if (error < 0) {
        log_error("failed to submit read for fd %d: %s", fd, strerror(errno));

        return errno;
    }

    queue->running++;

    return 0;
}

int zstd_proxy_uring_submit_send(zstd_proxy_uring_queue *queue) {
    zstd_proxy_uring_buffer *buffer = zstd_proxy_uring_get(queue, zstd_proxy_uring_send_buffer);

    // Don't send anything if a send() is currently running
    if (buffer == NULL || buffer->running) {
        return 0;
    }

    // Enqueue a send()
    struct io_uring *uring = &queue->uring;
    struct io_uring_sqe *sqe = io_uring_get_sqe(uring);

    if (sqe == NULL) {
        log_error("failed to get uring write sqe");

        return EIO;
    }

    buffer->running = true;
    buffer->available = false;

    zstd_proxy_connection *connection = queue->connection;
    zstd_proxy_io_uring_options *options = &connection->options->io_uring;
    int fd = connection->connect->fd;

    // log_debug("scheduling send on fd %d, buffer=%d, size=%d", fd, buffer->index, buffer->size);

    if (options->zero_copy) {
        io_uring_prep_send_zc_fixed(sqe, fd, &buffer->data[buffer->offset], buffer->size, 0, 0, buffer->index);
    } else if (options->fixed_buffers) {
        io_uring_prep_write_fixed(sqe, fd, &buffer->data[buffer->offset], buffer->size, 0, buffer->index);
    } else {
        io_uring_prep_write(sqe, fd, &buffer->data[buffer->offset], buffer->size, 0);
    }

    io_uring_sqe_set_data(sqe, buffer);

    int error = io_uring_submit(uring);

    if (error < 0) {
        log_error("failed to submit write on fd %d: %s", fd, strerror(errno));

        return errno;
    }

    return 0;
}

static inline int zstd_proxy_uring_process(zstd_proxy_uring_buffer *recv_buffer) {
    int error = 0;
    zstd_proxy_uring_queue *queue = recv_buffer->queue;
    ZSTD_inBuffer input = {
        .src = recv_buffer->data,
        .pos = recv_buffer->offset,
        .size = recv_buffer->size,
    };

    // Loop in case the input doesn't fit in the output
    while (input.pos < input.size) {
        zstd_proxy_uring_buffer *send_buffer = NULL;

        zstd_proxy_uring_foreach(queue, zstd_proxy_uring_send_buffer) {
            if (buffer->available) {
                send_buffer = buffer;

                break;
            }
        }

        if (send_buffer == NULL) {
            // No send buffer available, save the offset and wait for next cqe
            recv_buffer->offset = input.pos;

            return 0;
        }

        ZSTD_outBuffer output = {
            .dst = send_buffer->data,
            .pos = 0,
            .size = queue->buffer_size,
        };

        // Pass the data to Zstd
        error = queue->process(queue->process_data, &input, &output);

        if (error != 0) {
            return error;
        }

        queue->running++;
        send_buffer->id = ++queue->id;
        send_buffer->size = output.pos;
        send_buffer->offset = 0;
        send_buffer->available = false;

        // Enqueue a send() if none are pending
        error = zstd_proxy_uring_submit_send(queue);

        if (error != 0) {
            return error;
        }
    }

    // This buffer can be filled by the kernel now
    queue->running--;
    recv_buffer->available = true;

    return 0;
}

static inline int zstd_proxy_uring_complete(zstd_proxy_uring_buffer *buffer) {
    int res = buffer->result;
    zstd_proxy_uring_queue *queue = buffer->queue;

    if (buffer->type == zstd_proxy_uring_recv_buffer) {
        int fd = queue->connection->listen->fd;

        log_debug("received data on fd %d, res=%d", fd, res);

        if (res < 0) {
            buffer->size = 0;
            buffer->offset = 0;

            log_error("failed read socket on fd %d: %s", fd, strerror(-res));

            return -res;
        }

        buffer->size = res;
        buffer->offset = 0;

        return 0;
    } else if (buffer->type == zstd_proxy_uring_send_buffer) {
        int fd = queue->connection->connect->fd;

        log_debug("sent data on fd %d, res=%d", fd, res);

        if (res < 0) {
            log_error("failed write to socket on fd %d: %s", fd, strerror(-res));

            return -res;
        }

        size_t size = res;

        if (size < buffer->size) {
            // Only a part of the buffer was sent, send more
            buffer->size -= size;
            buffer->offset += size;
        } else {
            // Everything got sent, mark the buffer as available
            queue->running--;
            buffer->available = true;
        }

        return 0;
    } else {
        log_error("invalid buffer type %d", buffer->type);

        return EINVAL;
    }
}

int zstd_proxy_uring_run(zstd_proxy_connection *connection) {
    int error = 0;
    zstd_proxy_uring_queue *queue;

    // Create the ring buffer
    error = zstd_proxy_uring_create(&queue, connection);

    if (error != 0) {
        goto cleanup;
    }

    queue->process = connection->process;
    queue->process_data = connection->process_data;

    // Send any buffered data if needed
    if (connection->listen->data_length > 0) {
        zstd_proxy_uring_buffer *buffer = &queue->buffers[0];

        queue->running++;
        buffer->available = false;
        buffer->data = connection->listen->data;
        buffer->size = connection->listen->data_length;
        buffer->offset = 0;

        // Process the recv buffer (pass to Zstd and enqueue send() calls)
        error = zstd_proxy_uring_process(buffer);

        if (error != 0) {
            goto cleanup;
        }
    }

    // Send a first recv()
    error = zstd_proxy_uring_submit_recv(queue);

    if (error != 0) {
        goto cleanup;
    }

    bool read = true;
    struct io_uring *uring = &queue->uring;
    struct io_uring_cqe *cqe = NULL;

    // Event loop
    while (!connection->options->stop && queue->running > 0) {
        // Wait for an event
        error = io_uring_wait_cqe(uring, &cqe);

        // Acknowledge it
        io_uring_cqe_seen(uring, cqe);

        if (error != 0) {
            log_error("failed to wait for cqe: %s", strerror(errno));

            goto cleanup;
        }

        // More events are coming (happens during zero-copy)
        if (cqe->flags & IORING_CQE_F_MORE) {
            continue;
        }

        // Get associated buffer with the event
        zstd_proxy_uring_buffer *buffer = io_uring_cqe_get_data(cqe);

        buffer->result = cqe->res;
        buffer->running = false;

        // Mark event as completed
        error = zstd_proxy_uring_complete(buffer);

        if (error != 0) {
            goto cleanup;
        }

        // Enqueue I/O requests before locking the CPU
        error = zstd_proxy_uring_submit_send(queue);

        if (error != 0) {
            goto cleanup;
        }

        if (read) {
            error = zstd_proxy_uring_submit_recv(queue);

            if (error != 0) {
                goto cleanup;
            }
        }

        // Get the oldest pending recv buffer
        zstd_proxy_uring_buffer *recv_buffer = zstd_proxy_uring_get(queue, zstd_proxy_uring_recv_buffer);

        // Nothing to process, wait for more
        if (recv_buffer == NULL || recv_buffer->running) {
            continue;
        }

        // Connection got closed, send remaining data and don't recv()
        if (recv_buffer->size == 0) {
            read = false;
        }

        debug_assert(!recv_buffer->running);

        // Process the recv buffer (pass to Zstd and enqueue send() calls)
        error = zstd_proxy_uring_process(recv_buffer);

        if (error != 0) {
            goto cleanup;
        }

        // Enqueue another recv() if possible
        if (read) {
            error = zstd_proxy_uring_submit_recv(queue);

            if (error != 0) {
                goto cleanup;
            }
        }
    }

    log_debug("stopping, running=%lu", queue->running);
    
    cleanup:

    zstd_proxy_uring_destroy(queue);

    return error;
}
