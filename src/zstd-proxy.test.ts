import { request } from 'http'
import { createServer, connect, Server, Socket, createConnection, SocketAddress } from 'net'
import { createServer as createHttpServer } from 'http'

import { zstdProxy } from './zstd-proxy'

const serverPort = 8540
const serverProxyPort = 8541
const clientProxyPort = 8542
const fail = (error: Error) => {
    console.error(error)

    process.exit(1)
}

runTest().catch(error => {
    console.error(error)
    process.exit(1)
}).then(() => {
    console.log('Test passed')
})

async function runTest() {
    let pass = false

    await testHarness({
        mode: 'http',
        server: {
            connect(socket) {
                // socket.write('yo')
            },
            head: Buffer.from('yo'),
            data(data, socket) {
                console.log('Message from client: %s, size=%s', data.toString('utf-8'), data.length)
            
                switch(data.toString('utf-8')) {
                    case "yo! what's up?":
                        return socket.write('just trying to test a proxy')
                    case 'how it is going?':
                        return socket.write('you tell me!')
                    case 'seems to be working fine':
                        return socket.write("yea I guess you're right")
                    case 'always am, ciao':
                        pass = true
                        return socket.end()
                    default:
                        return fail(new Error('Invalid client message'))
                }
            }
        },
        client: {
            data(data, socket) {
                console.log('Message from server: %s', data.toString('utf-8'))
            
                switch(data.toString('utf-8')) {
                    case 'yo':
                        return socket.write("yo! what's up?")
                    case 'just trying to test a proxy':
                        return socket.write('how it is going?')
                    case 'you tell me!':
                        return socket.write('seems to be working fine')
                    case "yea I guess you're right":
                        return socket.write('always am, ciao')
                    default:
                        return fail(new Error('Invalid server message'))
                }
            }
        }
    })

    if (!pass) {
        throw new Error('Connection closed')
    }
}


async function testHarness(
    options: {
        mode?: 'socket' | 'http'
        server: {
            head?: Buffer
            connect?(socket: Socket): void
            data?(data: Buffer, socket: Socket): void
        }
        client:{
            connect?(socket: Socket): void
            data?(data: Buffer, socket: Socket): void
        }
    }
) {
    const server = await listen(
        serverPort,
        (socket, head) => {
            if (head?.length) {
                options.server.data?.(head, socket)
            }

            socket.on('data', data =>
                setTimeout(() => options.server.data?.(data, socket), Math.random() * 500)
            )
            options.server.connect?.(socket)
        },
        { mode: options.mode, pauseOnConnect: false}
    )

    const serverProxy = await listen(serverProxyPort, client => {
        if(options.mode === 'http') {
            request(`http://localhost:${serverPort}`, {
                headers: {
                    Connection: 'upgrade', Upgrade: 'test-zstd-proxy'
                }
            })
                .on('error', fail)
                .on('upgrade', (_, socket, head) => {
                    console.log('server upgrade response')

                    setTimeout(() => zstdProxy({
                        compress: { socket, head: options.server.head },
                        to: { socket: client, head },
                    }), Math.random() * 500)
                })
                .end()
        } else {
            const socket = createConnection(serverPort)
        
            socket
                .on('error', fail)
                .on('connect', () =>
                    setTimeout(() =>
                        zstdProxy({ compress: {socket, head: options.server.head}, to: client }),
                        Math.random() * 500
                    )
                )
        }
    })

    const clientProxy = await listen(clientProxyPort, client => {
        const socket = createConnection(serverProxyPort)
    
        socket
            .on('error', fail)
            .on('connect', () => zstdProxy({ compress: client, to: socket }))
    })

    await new Promise<void>((resolve, reject) => {
        const socket = createConnection(clientProxyPort)

        socket
            .on('error', reject)
            .on('close', () => resolve())
            .on('connect', () => options.client.connect?.(socket))
            .on('data', data => setTimeout(() => options.client.data?.(data, socket), Math.random() * 500))
    })

    server.close()
    serverProxy.close()
    clientProxy.close()
}

async function listen(
    port: number,
    handle: (socket: Socket, head?: Buffer) => void,
    { mode, pauseOnConnect = true }: { mode?: 'socket' | 'http'; pauseOnConnect?: boolean } = {}
) {
    if (mode === 'http') {
        return await new Promise<Server>((resolve, reject) => {
            const server: Server = createHttpServer((_req, res) => res.writeHead(404).end())
                .listen(port)
                .on('error', (error) => reject(error))
                .on('listening', () => resolve(server))
                .on('upgrade', ({ socket }, _stream, head) => {
                    setTimeout(() => {
                        console.log('server upgrade request')
                        socket.write(
                            Buffer.from(
                                `HTTP/1.1 101 Handshake\r\n` +
                                    `Upgrade: test-zstd-proxy\r\n` +
                                    'Connection: Upgrade\r\n' +
                                    '\r\n',
                            ),
                        )
    
                        handle(socket, head)
                    }, 50)
                })
        })
    } else {
        return await new Promise<Server>((resolve, reject) => {
            const server: Server = createServer({ pauseOnConnect }, handle)
                .listen(port)
                .on('error', (error) => reject(error))
                .on('listening', () => resolve(server))
        })
    }
}
