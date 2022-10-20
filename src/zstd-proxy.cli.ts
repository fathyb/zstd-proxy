import { openSync } from "fs"
import { createConnection, createServer, Socket } from "net"

import { zstdProxy } from "./zstd-proxy"

export function zstdProxyCli() {
    const args = new Map(process.argv.slice(2).map(string => {
        const arg = parseArgument(string)

        if(!arg) {
            throw new Error(`Invalid argument: ${string}`)
        }

        return [arg.key, arg.value]
    }))

    const listen = args.get('listen')
    const connect = args.get('connect')
    const compress = args.get('compress')

    if(!listen) {
        throw new Error('Missing --listen argument')
    }

    if(!connect) {
        throw new Error('Missing --connect argument')
    }

    if(!compress) {
        throw new Error('Missing --compress argument')
    }

    if(compress !== 'listen' && compress !== 'connect') {
        throw new Error(`Invalid --compress argument: "${compress}"`)
    }

    const getListenSocket = (cb: (server: number | Socket) => void) => {
        switch(listen) {
            case 'null':
                return cb(openSync('/dev/null', 'r+'))
        }

        createServer({pauseOnConnect: true})
            .listen(parseSocketOptions(listen))
            .on('connection', server => cb(server))
            .on('error', error => {
                console.error(error)

                process.exit(1)
            })
    }
    const connectSocket = (cb: (client: number | Socket) => void) => {
        switch(connect) {
            case 'null':
                return cb(openSync('/dev/null', 'r+'))
        }

        const client: Socket = createConnection(parseSocketOptions(connect))
            .on('connect', () => cb(client))
    }

    getListenSocket(
        server =>
            connectSocket((client) => {
                console.log('Connection opened')

                zstdProxy({
                    compress: compress === 'listen' ? server : client,
                    to: compress === 'listen' ? client : server,
                    onClose(error) {
                        if(error) {
                            console.error(error)
                        } else {
                            console.log('Connection closed')
                        }
                    }
                })
            })
    )
}

function parseArgument(argument: string) {
    if(!argument.startsWith('--')) {
        return null
    }

    const [key, value] = argument.slice(2).split('=')

    if(!value) {
        return null
    }

    if(key !== 'listen' && key !== 'connect' && key !== 'compress') {
        return null
    }
    
    return {key, value}
}

function parseSocketOptions(path: string) {
    if(path[0] === '.' || path[1] === '/') {
        return {path}
    } else {
        let [host, portString] = path.split(':')

        if(!portString) {
            [portString, host] = [host, portString]
        }

        const port = parseInt(portString, 10)

        if(Number.isNaN(port) || port < 0 || port > 65535) {
            throw new Error(`Invalid port number: ${portString}`)
        }

        return {host, port}
    }
}
