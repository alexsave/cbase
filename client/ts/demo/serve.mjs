// serve.mjs - the Sign in with Apple demo on one machine: the fake Apple (build/test/fake_apple),
// a `cbase serve` that trusts it, and this page, each on a port of its own. Ctrl-C stops all three.
//
//   node serve.mjs <c_base dir> <demo dir> [page port]
//
// The page is http://127.0.0.1:<page port>/ (?auto=1 signs in by itself). Nothing here holds or
// asks for an Apple ID: every token is the fake's, minted for the page's own nonce.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, existsSync } from 'node:fs';
import { join, extname } from 'node:path';
import { tmpdir } from 'node:os';

const [cbase, demo, pagePortArg] = process.argv.slice(2);
const pagePort = Number(pagePortArg || 8941);
const dir = mkdtempSync(join(process.env.TMPDIR || tmpdir(), 'cbase-apple-demo-'));
const kids = [];
const stop = () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } process.exit(0); };
process.on('SIGINT', stop);
process.on('SIGTERM', stop);

const lineOf = (child, needle) => new Promise((resolve, reject) => {
  let buf = '';
  const on = (d) => {
    buf += d;
    const l = buf.split('\n').find((x) => x.includes(needle));
    if (l) resolve(l);
  };
  child.stdout?.on('data', on);
  child.stderr?.on('data', (d) => { process.stderr.write(d); on(d); });
  child.on('exit', (c) => reject(new Error(`exited ${c} before "${needle}"`)));
});

// 1. The fake Apple, and the settings it wrote.
const envFile = join(dir, 'apple.env');
const apple = spawn(join(cbase, 'build/test/fake_apple'), [envFile], { stdio: ['ignore', 'pipe', 'pipe'] });
kids.push(apple);
await lineOf(apple, 'settings in');
const env = {};
for (const l of readFileSync(envFile, 'utf8').split('\n')) {
  const m = l.match(/^([A-Z_]+)=(.*)$/);
  if (m) env[m[1]] = m[2].replace(/^'(.*)'$/, '$1');
}

// 2. The server, trusting the fake as it would Apple.
const page = `http://127.0.0.1:${pagePort}`;
const server = spawn(join(cbase, 'build/cbase'), ['serve'], {
  env: { ...process.env, ...env, CBASE_DEV: '1', CBASE_LISTEN: '127.0.0.1:0', CBASE_DB: join(dir, 'cbase.sqlite'),
         CBASE_JWT_SECRET: 'demo-secret-demo-secret-demo-secret-0123', CBASE_ALLOWED_ORIGINS: page,
         CBASE_APPLE_CLIENT_IDS: 'com.example.web,com.example.app',
         CBASE_APPLE_REDIRECT_URI: 'https://demo.example/auth/apple' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
kids.push(server);
const listening = await lineOf(server, 'listening on');
const serverUrl = 'http://127.0.0.1:' + listening.match(/listening on [^ ]*:(\d+)/)[1];

// 3. The page.
const config = `window.CBASE_DEMO = ${JSON.stringify({ server: serverUrl, wasm: 'cbase_client_oidc.wasm', clientId: 'com.example.web',
  redirectURI: 'https://demo.example/auth/apple', appleJs: `${env.FAKE_APPLE_MINT}/appleid.auth.js`, fake: true })};\n`;
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css' };
createServer((req, res) => {
  const path = (req.url || '/').split('?')[0];
  if (path === '/config.js') { res.writeHead(200, { 'content-type': 'text/javascript', 'cache-control': 'no-store' }); res.end(config); return; }
  const file = join(demo, path === '/' ? 'index.html' : path.slice(1));
  if (!file.startsWith(demo) || !existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
  res.writeHead(200, { 'content-type': types[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
  res.end(readFileSync(file));
}).listen(pagePort, '127.0.0.1', () => console.log(`demo: ${page}/ (server ${serverUrl}, fake Apple ${env.CBASE_APPLE_ISSUER})`));
