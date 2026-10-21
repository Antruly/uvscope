#!/usr/bin/env node
// Local gzip / deflate / brotli server for the uvscope smoke test.
//
// Usage: node compression-test-server.mjs <port>
// Prints "COMPRESSION LISTENING <port>". /gzip, /deflate and /br return the
// same text payload with the matching Content-Encoding.
import http from 'node:http'
import zlib from 'node:zlib'

const port = Number(process.argv[2])
if (!port) {
  console.error('usage: compression-test-server.mjs <port>')
  process.exit(2)
}

const payload = Buffer.from('compressed-ok '.repeat(200))
const encoders = {
  '/gzip': zlib.gzipSync,
  '/deflate': zlib.deflateSync,
  '/br': zlib.brotliCompressSync,
}

const server = http.createServer((request, response) => {
  const encode = encoders[request.url]
  if (!encode) {
    response.writeHead(404, { 'content-type': 'text/plain' })
    response.end('not found')
    return
  }
  const body = encode(payload)
  response.writeHead(200, {
    'content-type': 'text/plain; charset=utf-8',
    'content-encoding': request.url.slice(1),
    'content-length': body.length,
    'cache-control': 'no-store',
  })
  response.end(body)
})

server.on('error', (error) => {
  console.error(`compression server error: ${error.message}`)
  process.exit(1)
})

server.listen(port, '127.0.0.1', () => {
  console.log(`COMPRESSION LISTENING ${server.address().port}`)
})
