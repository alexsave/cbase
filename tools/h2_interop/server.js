// server.js - a gRPC unary service on Node's own node:http2, for h2_interop.c to call.
//
// Two listeners on 127.0.0.1, free ports both: cleartext with prior knowledge, and TLS
// (ALPN h2) with the certificate and key named on the command line. It answers the way
// amgi's fake Gemini (plusaudio/test/helpers/fake-gemini.js) does, so the client meets
// Node's HPACK as Gemini's test double sends it: Huffman strings, and response fields
// Node indexes into its dynamic table and names by index on the calls after.
//
//   /interop.Echo/Echo  the request's message back; headers a few fields long, trailers
//                       grpc-status 0 and grpc-message "ok" (percent-encoded)
//   /interop.Echo/Meta  a message "<x-goog-api-key>|<grpc-timeout>|<never or indexed>"
//   /interop.Echo/Fail  trailers-only RESOURCE_EXHAUSTED, a percent-encoded message and
//                       grpc-status-details-bin (base64 of fixed bytes), as Gemini refuses
//   anything else       trailers-only UNIMPLEMENTED (12)
//
// usage: node server.js <cert.pem> <key.pem> <ports file>
// It writes "<cleartext port> <tls port>\n" to the ports file once both listen, and runs
// until it is killed.
'use strict';

const fs = require('node:fs');
const http2 = require('node:http2');

const [certFile, keyFile, portsFile] = process.argv.slice(2);
if (!portsFile) {
  console.error('usage: node server.js <cert.pem> <key.pem> <ports file>');
  process.exit(2);
}

// The bytes the C side expects back as the decoded details (h2_interop.c FAIL_DETAILS).
const FAIL_DETAILS = Buffer.from([0x08, 0x08, 0x12, 0x05, 0x71, 0x75, 0x6f, 0x74, 0x61, 0x1a, 0x00]);

function frame(message) {
  const prefix = Buffer.alloc(5);
  prefix.writeUInt32BE(message.length, 1);
  return Buffer.concat([prefix, message]);
}

function unframe(body) {
  if (body.length < 5 || body[0] !== 0 || body.readUInt32BE(1) !== body.length - 5) return null;
  return body.subarray(5);
}

function trailersOnly(stream, code, message, details) {
  stream.respond({
    ':status': 200,
    'content-type': 'application/grpc',
    'grpc-status': String(code),
    ...(message ? { 'grpc-message': encodeURIComponent(message) } : {}),
    ...(details ? { 'grpc-status-details-bin': details.toString('base64') } : {}),
  }, { endStream: true });
}

function onStream(stream, headers) {
  const chunks = [];
  stream.on('data', (chunk) => chunks.push(chunk));
  stream.on('end', () => {
    const path = headers[':path'];
    const message = unframe(Buffer.concat(chunks));
    if (message === null) return trailersOnly(stream, 13, 'the request is not one framed message');
    let reply;
    if (path === '/interop.Echo/Echo') {
      reply = message;
    } else if (path === '/interop.Echo/Meta') {
      const never = (headers[http2.sensitiveHeaders] || []).includes('x-goog-api-key');
      reply = Buffer.from(`${headers['x-goog-api-key'] || ''}|${headers['grpc-timeout'] || ''}|${never ? 'never' : 'indexed'}`);
    } else if (path === '/interop.Echo/Fail') {
      return trailersOnly(stream, 8, 'You exceeded your current quota: 100% used.', FAIL_DETAILS);
    } else {
      return trailersOnly(stream, 12, `unknown method ${path}`);
    }
    stream.respond({
      ':status': 200,
      'content-type': 'application/grpc',
      'grpc-accept-encoding': 'identity',
      'x-interop-server': 'node ' + process.version,
      'x-interop-long': 'a value long enough that Huffman coding it is worth the bits'.repeat(4),
    }, { waitForTrailers: true });
    stream.on('wantTrailers', () => stream.sendTrailers({ 'grpc-status': '0', 'grpc-message': encodeURIComponent('ok') }));
    stream.end(frame(reply));
  });
}

const clear = http2.createServer();
const tls = http2.createSecureServer({ cert: fs.readFileSync(certFile), key: fs.readFileSync(keyFile), allowHTTP1: false });
clear.on('stream', onStream);
tls.on('stream', onStream);
let listening = 0;
function ready() {
  if (++listening < 2) return;
  fs.writeFileSync(portsFile + '.new', `${clear.address().port} ${tls.address().port}\n`);
  fs.renameSync(portsFile + '.new', portsFile);
}
clear.listen(0, '127.0.0.1', ready);
tls.listen(0, '127.0.0.1', ready);
