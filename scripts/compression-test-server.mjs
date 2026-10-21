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
const big = Buffer.alloc(6 * 1024 * 1024)
for (let i = 0; i < big.length; i += 1) big[i] = i & 0xff
const encoders = {
  '/gzip': zlib.gzipSync,
  '/deflate': zlib.deflateSync,
  '/br': zlib.brotliCompressSync,
}

const server = http.createServer((request, response) => {
  if (request.url === '/echo-request') {
    const chunks = []
    request.on('data', (chunk) => chunks.push(chunk))
    request.on('end', () => {
      const body = Buffer.concat(chunks)
      const payload = JSON.stringify({
        method: request.method,
        headers: request.headers,
        body: body.toString('utf8'),
      })
      response.writeHead(200, {
        'content-type': 'application/json',
        'content-length': Buffer.byteLength(payload),
        'cache-control': 'no-store',
      })
      response.end(payload)
    })
    return
  }
  if (request.url === '/echo-body') {
    const chunks = []
    request.on('data', (chunk) => chunks.push(chunk))
    request.on('end', () => {
      const body = Buffer.concat(chunks)
      response.writeHead(200, {
        'content-type': request.headers['content-type'] || 'application/octet-stream',
        'content-length': body.length,
        'cache-control': 'no-store',
      })
      response.end(body)
    })
    return
  }
  if (request.url === '/big.bin') {
    response.writeHead(200, {
      'content-type': 'application/octet-stream',
      'content-length': big.length,
      'cache-control': 'no-store',
    })
    response.end(big)
    return
  }
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
