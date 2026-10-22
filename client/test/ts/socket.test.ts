// socket.test.ts - the page's shim (ts/socket.ts) over the real module (build/ts/cbase_client.wasm),
// against a fake WebSocket and a fake fetch: what it opens, what it sends, what it hands the page.
// The server's frames are written out here byte by byte (stream_frame.h: [SF_V][kind] ...), as a
// server would put them on the wire; the shim itself reads none of them.
//
//   make -C c_base/client ts-test
import { test, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { CbClient, CbError } from '../../build/ts/socket.ts';

const WASM = readFileSync(new URL('../../build/ts/cbase_client.wasm', import.meta.url));
const wasm = () => WASM.buffer.slice(WASM.byteOffset, WASM.byteOffset + WASM.byteLength) as ArrayBuffer;

const SF_V = 1, SF_HELLO = 60, SF_READY = 61, SF_SUB = 62, SF_REPLY = 65;

class FakeSocket {
  static all: FakeSocket[] = [];
  url: string;
  binaryType = '';
  readyState = 0;
  sent: Uint8Array[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: ArrayBuffer }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(url: string) {
    this.url = url;
    FakeSocket.all.push(this);
  }
  send(b: ArrayBuffer) { this.sent.push(new Uint8Array(b)); }
  close() { this.readyState = 3; }
  // the server's side
  open() { this.readyState = 1; this.onopen?.(); }
  push(bytes: number[]) { this.onmessage?.({ data: new Uint8Array(bytes).buffer }); }
  drop() { this.readyState = 3; this.onclose?.(); }
}

const u32 = (b: Uint8Array, at: number) => b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (b[at + 3] << 24 >>> 0);
const reply = (id: number, status: number, body: number[] = []) =>
  [SF_V, SF_REPLY, id & 255, id >>> 8 & 255, id >>> 16 & 255, id >>> 24 & 255, status & 255, status >> 8, ...body];
// Every client a test opens is closed after it, passed or failed: a client's timer would keep the
// process alive.
const open: CbClient[] = [];
afterEach(() => { for (const c of open.splice(0)) c.close(); });
const start = async (o: Parameters<typeof CbClient.open>[0]) => { const c = await CbClient.open(o); open.push(c); return c; };

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const until = async (cond: () => boolean, ms = 2000) => {
  const end = Date.now() + ms;
  while (!cond()) { if (Date.now() > end) throw new Error('timed out'); await sleep(2); }
};

test('anonymous: /ws/public, an empty HELLO, the SUB after READY, then pushes', async () => {
  FakeSocket.all = [];
  const live: string[] = [];
  const pushes: [number, number[]][] = [];
  const states: string[] = [];
  const c = await start({
    wasm: wasm(), server: 'http://adv.test:8711', anonymous: true, seed: 7n,
    WebSocket: FakeSocket as unknown as typeof WebSocket,
    onSubLive: (_w, room) => live.push(room),
    onPush: (k, f) => pushes.push([k, [...new Uint8Array(f)]]),
    onState: (s) => states.push(s),
  });
  assert.equal(FakeSocket.all.length, 0);              // nothing held: no socket
  const release = c.hold(2, 'feed');
  assert.equal(FakeSocket.all.length, 1);
  const s = FakeSocket.all[0];
  assert.equal(s.url, 'ws://adv.test:8711/ws/public');
  s.open();
  assert.deepEqual([...s.sent[0]], [SF_V, SF_HELLO, 0, 0]);
  s.push([SF_V, SF_READY]);
  const sub = s.sent[1];
  assert.equal(sub[1], SF_SUB);
  assert.equal(new TextDecoder().decode(sub.subarray(8)), 'feed');
  s.push(reply(u32(sub, 2), 200));
  assert.deepEqual(live, ['feed']);
  s.push([SF_V, 1, 9, 9, 9]);
  assert.deepEqual(pushes, [[1, [SF_V, 1, 9, 9, 9]]]);
  assert.deepEqual(states, ['connecting', 'open']);
  release();
  assert.equal(s.readyState, 3);                       // the last hold gone: the socket closes
  c.close();
});

test('a dropped socket reopens after the jittered wait and holds its rooms again', async () => {
  FakeSocket.all = [];
  const c = await start({
    wasm: wasm(), server: 'https://adv.test', anonymous: true, seed: 1n, timing: { retryFirstMs: 20, retryMaxMs: 100 },
    WebSocket: FakeSocket as unknown as typeof WebSocket,
  });
  c.hold(2, 'feed');
  const a = FakeSocket.all[0];
  assert.equal(a.url, 'wss://adv.test/ws/public');     // the page's scheme
  a.open();
  a.push([SF_V, SF_READY]);
  const t0 = Date.now();
  a.drop();
  await until(() => FakeSocket.all.length === 2);
  const waited = Date.now() - t0;
  assert.ok(waited >= 18 && waited < 200, `waited ${waited} ms`);   // [20, 60] by the schedule, plus timer slack
  const b = FakeSocket.all[1];
  b.open();
  b.push([SF_V, SF_READY]);
  assert.equal(b.sent[1][1], SF_SUB);
  c.close();
});

test('an anonymous read goes over HTTP; a busy one goes again; a refusal is a CbError', async () => {
  const seen: string[] = [];
  let busy = 1;
  const fakeFetch = (async (url: string, init: RequestInit) => {
    seen.push(`${init.method} ${url}`);
    if (url.endsWith('/busy') && busy-- > 0)
      return new Response(new Uint8Array([1, 10, 0, 0, 0, 0, 0, 0, 0, 0]), { status: 503 });
    if (url.endsWith('/refused')) return new Response(new Uint8Array([1, 3, 17, 0, 0, 0, 0, 0, 0, 0]), { status: 409 });
    return new Response(new Uint8Array([5, 6, 7]), { status: 200 });
  }) as unknown as typeof fetch;
  const c = await start({ wasm: wasm(), server: 'http://adv.test', anonymous: true, fetch: fakeFetch });
  assert.deepEqual([...new Uint8Array(await c.get('/adv/users'))], [5, 6, 7]);
  assert.deepEqual([...new Uint8Array(await c.get('/busy'))], [5, 6, 7]);
  assert.deepEqual(seen.slice(0, 3), ['GET http://adv.test/adv/users', 'GET http://adv.test/busy', 'GET http://adv.test/busy']);
  // The refusal's own why and code (not what the status alone would say): the body was handed in.
  await assert.rejects(c.get('/refused'), (e: unknown) => e instanceof CbError && e.status === 409 && e.why === 3 && e.code === 17);
  c.close();
});
