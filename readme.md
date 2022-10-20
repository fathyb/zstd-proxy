# zstd-proxy

High-performance TCP/UNIX proxy with Zstd compression, with Node.js bindings.

This is built for the block storage infrastructre in Refloat CI, optimized for a low count (< 100) of medium-lived connections (~ 3 minutes) with high-throughput (~ 500Mbps) and low-latency requirements (< 1ms). It uses io_uring on Linux with fixed buffers to transmit without any syscall.

An optional zero-copy send mode can be enabled, but it requires Linux 6 and seems to crash on ARM.

## Usage

### CLI

- Create a server on port `9001`, compress `9001` to `9002` and decompress `9002` to `9001`
```console
$ zstd-proxy --listen=9001 --connect=9002 --compress=listen
```
- Create a server on port `9002`, compress `9003` to `9002` and decompress `9002` to `9003`
```console
$ zstd-proxy --listen=9002 --connect=9003 --compress=connect
```

### Library

- Create a server on port `9001`, compress `9001` to `9002` and decompress `9002` to `9001`
```ts
import { zstdProxy } from 'zstd-proxy'

createServer({ pauseOnConnect: true })
    .on('connection', server => {
        const client: Socket = connect(9002)
            .on('connect', () => zstdProxy({ compress: server, to: client }))
    })
    .listen(9001)
```
- Create a server on port `9002`, compress `9003` to `9002` and decompress `9002` to `9003`
```ts
import { zstdProxy } from 'zstd-proxy'

createServer({ pauseOnConnect: true })
    .on('connection', server => {
        const client: Socket = connect(9003)
            .on('connect', () => zstdProxy({ compress: client, to: server }))
    })
    .listen(9002)
```
