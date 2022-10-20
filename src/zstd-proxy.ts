import { Socket } from 'net'

import { proxy } from '../native/build/Release/zstd_proxy.node'

export type SocketWithHead = { socket: Socket; head?: Buffer }
export type MaybeSocketWithHead = Socket | SocketWithHead

export interface ZstdProxyOptions {
    compress: number | MaybeSocketWithHead
    to: number | MaybeSocketWithHead

    onClose?(error?: Error): void

    zstd?: {
        /** Set to `false` to disable compression */
        enabled?: boolean

        /** Zstd compression level. Defaults to `1`. */
        level?: number
    }

    /**
     * Linux io_uring specific options.
     * More efficient, this implementation will use around `depth * 2 * bufferSize` bytes of memory per connection.
     */
    io_uring?: {
        /** Set to `false` to disable io_uring. */
        enabled?: boolean

        /** Configure how many pending items can be queued. Defaults to `8`. */
        depth?: number
        /** Set to `false` to disable zero-copy networking. */
        zeroCopy?: number
        /** Configure the ring buffer size in bytes. Defaults to 1 MB (`1024 * 1024`). */
        bufferSize?: number
        /** Set to `false` to disable fixed buffers. */
        fixedBuffers?: boolean
    }
}

export async function zstdProxy(options: ZstdProxyOptions) {
    const to = socketWithHead(options.to)
    const compress = socketWithHead(options.compress)

    proxy(compress.fd, to.fd, compress.head, to.head, {
        zstd: options.zstd?.enabled,
        zstd_level: options.zstd?.level,
        io_uring: options.io_uring?.enabled,
        io_uring_depth: options.io_uring?.depth,
        io_uring_zero_copy: options.io_uring?.zeroCopy,
        io_uring_buffer_size: options.io_uring?.bufferSize,
        io_uring_fixed_buffers: options.io_uring?.fixedBuffers,
    }, (code?: number) => {
        to.socket?.destroy()
        compress.socket?.destroy()

        options.onClose?.(
            typeof code === 'number' ? new Error(`Error ${code}`) : undefined,
        )
    })
}

// Prevent Node.js from sending any system calls on the socket file descriptor.
function disown(socket: Socket) {
    const anySocket = socket as any
    const { _handle } = anySocket
    const fd = anySocket._handle?.fd

    if (typeof fd !== 'number' || fd < 0) {
        throw new Error('Invalid socket file descriptor')
    }

    // Prevent Node.js from calling recv() on the socket
    socket.setNoDelay().pause()
    _handle.readStop()
    _handle.readStop = () => {}
    _handle.readStart = () => {}
    anySocket._handle = null

    return fd
}

function socketWithHead(socket: number | MaybeSocketWithHead) {
    return typeof socket === 'number'
        ? { fd: socket }
        : socket instanceof Socket
        ? { fd: disown(socket), socket }
        : {
              fd: disown(socket.socket),
              socket: socket.socket,
              head: socket.head,
          }
}
