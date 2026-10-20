#!/usr/bin/env node
// Minimal HTTP/2 test server for the uvscope smoke test.
//
// Usage: node h2-test-server.mjs <key.pem> <cert.pem> <port>
// Prints "H2 LISTENING <port>" once the TLS socket is up. Each request to
// /h2 returns 200 text/plain "h2-ok"; every other path returns 404. ALPN
// negotiates h2 because createSecureServer advertises it by default.
import fs from 'node:fs'
import http2 from 'node:http2'

const [keyPath, certPath, portText] = process.argv.slice(2)
if (!keyPath || !certPath || !portText) {
  console.error('usage: h2-test-server.mjs <key.pem> <cert.pem> <port>')
  process.exit(2)
}

const server = http2.createSecureServer({
  key: fs.readFileSync(keyPath),
  cert: fs.readFileSync(certPath),
  allowHTTP1: false,
})

server.on('stream', (stream, headers) => {
  if (headers[':path'] === '/h2') {
    stream.respond({ ':status': 200, 'content-type': 'text/plain' })
    stream.end('h2-ok')
  } else {
    stream.respond({ ':status': 404, 'content-type': 'text/plain' })
    stream.end('not found')
  }
})

server.on('error', (error) => {
  console.error(`h2 server error: ${error.message}`)
  process.exit(1)
})

server.listen(Number(portText), '127.0.0.1', () => {
  console.log(`H2 LISTENING ${server.address().port}`)
})
