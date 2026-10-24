// socket.ts - the page's host of the c_base client (c_base/client/include/cbase/client.h, compiled to
// cbase_client.wasm). Everything about the protocol is the module's: the frames, which call goes
// on the socket and which over HTTP, retries, the reconnect's jittered wait, ping, the token and its
// refresh. This file owns the browser's three resources and nothing else: one WebSocket, fetch, and
// one timer, plus localStorage for a signed-in client's session. It spells no byte offset: every
// struct is read and written through structgen's accessors (client_layout.ts), and the module's
// exports are typed by structgen from its C (client_exports.ts).
//
// THE SCHEME. The socket's URL is the server's with http mapped to ws and https to wss; the server
// defaults to the page's own origin, so a page on https talks wss (CLIENT_DESIGN.md 6.5).
//
// Copied by `make -C c_base/client ts` into an app's generated directory, beside the generated
// files it imports; edit it here.
import type { CbcWasmOidc } from './client_exports_oidc.ts';
import { LAYOUT_HASH } from './client_layout_hash.ts';
import {
  CBC_EV_DONE,
  CBC_EV_PUSH,
  CBC_EV_READY,
  CBC_EV_SESSION,
  CBC_EV_SUB_LIVE,
  CBC_EV_SUB_REFUSED,
  CBC_C_READ,
  CBC_GET,
  CBC_POST,
  CBC_RQ_BODY,
  CBC_OIDC_ENTROPY,
  CBW_OIDC_LINK,
  CBW_SIGNIN,
  CBW_SIGNUP,
  cbc_req_hdr_SIZE,
  memOf,
  readcbc_cfg,
  readcbc_event,
  readcbc_req_hdr,
  readcbc_user,
  writecbc_cfg,
  type cbc_cfg_Snap,
  type cbc_event_Snap,
} from './client_layout.ts';

export { CBC_C_AUTH, CBC_C_HTTP, CBC_C_READ, CBC_ROOM } from './client_layout.ts';
export {
  CBW_ACCOUNT_E_BAD_IDENTITY,
  CBW_ACCOUNT_E_EXISTS,
  CBW_ACCOUNT_E_LINKED,
  CBW_ACCOUNT_E_NEEDS_NAME,
  CBW_ACCOUNT_E_NO_PROVIDER,
  CBW_IDP_APPLE,
  CBW_WHY_ACCOUNT,
} from './client_layout.ts';

// What a provider answered, handed back for its sign-in (cbase/client.h cbc_oidc_sign_in).
export interface CbIdentityAnswer {
  readonly state: string;     // the state it echoed
  readonly idToken: string;
  readonly code?: string;     // its authorization code, when it gave one
  readonly name?: string;     // a new account's name (upper-cased here, as a sign-up's)
  readonly link?: boolean;    // add the identity to the signed-in account instead
}

export type CbState = 'connecting' | 'open' | 'closed';

export interface CbUser {
  readonly userId: string;
  readonly name: string;
  readonly expiresAt: number;
}

// The configuration's numbers a page may set (the rest are the module's defaults).
export type CbTiming = Partial<Pick<cbc_cfg_Snap, 'pingMs' | 'pongMs' | 'ackWaitMs' | 'retryFirstMs' | 'retryMaxMs' |
  'refreshEarlyMs' | 'forceRefreshMs' | 'callWaitMs'>>;

export interface CbOptions {
  // The module: its URL (fetched) or its bytes.
  wasm: string | URL | ArrayBuffer;
  // The app's server, http(s)://host[:port]; the page's own origin when absent.
  server?: string;
  // Listen without signing in (GET /ws/public); every call then goes over HTTP.
  anonymous?: boolean;
  // Where a signed-in client keeps its session blob (localStorage). None: it is not kept.
  sessionKey?: string;
  timing?: CbTiming;
  onPush?: (kind: number, frame: ArrayBuffer) => void;
  onState?: (s: CbState) => void;
  onSubLive?: (what: number, room: string) => void;
  onSubRefused?: (what: number, room: string, status: number) => void;
  onSession?: (user: CbUser | null) => void;
  // Test seams: the browser's own by default.
  WebSocket?: typeof WebSocket;
  fetch?: typeof fetch;
  now?: () => number;
  seed?: bigint;
}

// A call that finished without its answer: the HTTP status (0: it never arrived), and the
// refusal's why, code and detail (auth_wire.h CBW_WHY_*, or CBC_WHY_LOST).
export class CbError extends Error {
  readonly status: number;
  readonly why: number;
  readonly code: number;
  readonly detail: number;
  constructor(status: number, why: number, code: number, detail: number) {
    super(`c_base call failed: status ${status}, why ${why}${code ? `, code ${code}` : ''}`);
    this.name = 'CbError';
    this.status = status;
    this.why = why;
    this.code = code;
    this.detail = detail;
  }
}

// The module is not the one this page's readers were generated for.
export class CbLayoutMismatch extends Error {
  constructor(got: number) {
    super(`cbase_client.wasm layout ${got.toString(16)} is not this page's ${LAYOUT_HASH.toString(16)}: rebuild (make -C c_base/client ts)`);
    this.name = 'CbLayoutMismatch';
  }
}

interface Waiter {
  resolve: (b: ArrayBuffer) => void;
  reject: (e: Error) => void;
}

const utf8 = new TextEncoder();

function randomSeed(): bigint {
  const b = new BigUint64Array(1);
  globalThis.crypto.getRandomValues(b);
  return b[0];
}

function b64(bytes: Uint8Array): string {
  let s = '';
  for (const x of bytes) s += String.fromCharCode(x);
  return btoa(s);
}
function unb64(s: string): Uint8Array {
  const t = atob(s);
  const b = new Uint8Array(t.length);
  for (let i = 0; i < t.length; i++) b[i] = t.charCodeAt(i);
  return b;
}

export class CbClient {
  // Typed as the oidc variant's exports, a superset; a base module has none of cbcw_oidc_*.
  private readonly x: CbcWasmOidc;
  private readonly io: number;
  private readonly ioCap: number;
  private readonly o: CbOptions;
  private readonly http: string;
  private readonly wsUrl: string;
  private readonly WS: typeof WebSocket;
  private readonly fetchFn: typeof fetch;
  private readonly now: () => number;
  private readonly waiting = new Map<number, Waiter>();
  private ws: WebSocket | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private reopenAt = 0;
  private stopped = false;

  private constructor(x: CbcWasmOidc, o: CbOptions) {
    this.x = x;
    this.o = o;
    this.io = x.cbcw_io();
    this.ioCap = x.cbcw_io_cap();
    const origin = (o.server ?? globalThis.location?.origin ?? '').replace(/\/+$/, '');
    this.http = origin;
    this.WS = o.WebSocket ?? globalThis.WebSocket;
    this.fetchFn = o.fetch ?? globalThis.fetch.bind(globalThis);
    this.now = o.now ?? Date.now;
    const n = x.cbcw_stream_path();
    this.wsUrl = origin.replace(/^http/, 'ws') + new TextDecoder().decode(this.bytes(0, n));
  }

  // Loads the module, checks its layout, configures one client and starts it.
  static async open(o: CbOptions): Promise<CbClient> {
    const src = o.wasm instanceof ArrayBuffer ? o.wasm : await (await (o.fetch ?? fetch)(o.wasm)).arrayBuffer();
    const { instance } = await WebAssembly.instantiate(src, {});
    const x = instance.exports as unknown as CbcWasmOidc;
    const hash = x.cbcw_layout_hash() >>> 0;
    if (hash !== LAYOUT_HASH) throw new CbLayoutMismatch(hash);
    const p = x.cbcw_cfg();
    const m = memOf(x.memory.buffer);
    writecbc_cfg(m, p, { ...readcbc_cfg(m, p), ...o.timing, seed: o.seed ?? randomSeed(), anonymous: o.anonymous ? 1 : 0 });
    x.cbcw_init();
    const c = new CbClient(x, o);
    c.loadSession();
    c.pump();
    return c;
  }

  // ---- what a page calls ----------------------------------------------------------------------
  // Hear a room until the returned function is called (holds are counted by the client).
  hold(what: number, room: string): () => void {
    const n = this.put(utf8.encode(room));
    this.x.cbcw_sub(what, n);
    this.pump();
    let held = true;
    return () => {
      if (!held || this.stopped) return;
      held = false;
      this.x.cbcw_unsub(what, this.put(utf8.encode(room)));
      this.pump();
    };
  }

  get(path: string, flags = CBC_C_READ): Promise<ArrayBuffer> {
    return this.call(CBC_GET, flags, path);
  }

  post(path: string, body?: Uint8Array, flags = 0): Promise<ArrayBuffer> {
    return this.call(CBC_POST, flags, path, body);
  }

  // A call: its answer's body, or a CbError. It rides the socket when signed in, HTTP otherwise.
  call(method: number, flags: number, path: string, body?: Uint8Array): Promise<ArrayBuffer> {
    const p = utf8.encode(path);
    const b = body ?? new Uint8Array(0);
    if (p.length + b.length > this.ioCap) return Promise.reject(new RangeError('c_base call: path and body too large'));
    this.mem().u8.set(p, this.io);
    this.mem().u8.set(b, this.io + p.length);
    const t = this.x.cbcw_call(method, flags, p.length, b.length, this.now()) >>> 0;
    return this.ticket(t);
  }

  signIn(name: string, password: string, signUp = false): Promise<ArrayBuffer> {
    const nb = utf8.encode(name.toUpperCase());
    const pb = utf8.encode(password);
    this.mem().u8.set(nb, this.io);
    this.mem().u8.set(pb, this.io + nb.length);
    const t = this.x.cbcw_sign_in(signUp ? CBW_SIGNUP : CBW_SIGNIN, nb.length, pb.length, this.now()) >>> 0;
    return this.ticket(t);
  }

  // A provider's sign-in begins: the module makes the nonce and the state from the page's entropy and
  // keeps the raw nonce; the page hands these two to the provider (Apple's nonce and state).
  oidcBegin(): { nonce: string; state: string } {
    this.needOidc();
    const e = new Uint8Array(CBC_OIDC_ENTROPY);
    globalThis.crypto.getRandomValues(e);
    this.put(e);
    e.fill(0);
    if (this.x.cbcw_oidc_begin() !== 0) throw new Error('c_base: the provider sign-in did not begin');
    const dec = new TextDecoder();
    const nonce = dec.decode(this.bytes(0, this.x.cbcw_oidc_nonce()));
    const state = dec.decode(this.bytes(0, this.x.cbcw_oidc_state()));
    return { nonce, state };
  }

  // What the provider answered, to the server. Resolves when a session came (or, with link, when the
  // identity was added); rejects with the CbError the server refused with (CBW_ACCOUNT_E_NEEDS_NAME:
  // call again with a name, the same answer otherwise).
  oidcSignIn(idp: number, a: CbIdentityAnswer): Promise<ArrayBuffer> {
    this.needOidc();
    const parts = [a.state, a.idToken, a.code ?? '', (a.name ?? '').toUpperCase()].map((x) => utf8.encode(x));
    const total = parts.reduce((n, p) => n + p.length, 0);
    if (total > this.ioCap) return Promise.reject(new RangeError('c_base sign-in: the answer is too large'));
    let at = this.io;
    for (const p of parts) {
      this.mem().u8.set(p, at);
      at += p.length;
    }
    const t = this.x.cbcw_oidc_sign_in(idp, a.link ? CBW_OIDC_LINK : 0, parts[0].length, parts[1].length, parts[2].length,
      parts[3].length, this.now()) >>> 0;
    return this.ticket(t);
  }

  private needOidc(): void {
    if (typeof this.x.cbcw_oidc_begin !== 'function')
      throw new Error('c_base: this module has no provider sign-in: load cbase_client_oidc.wasm');
  }

  signOut(global = false): Promise<ArrayBuffer> {
    return this.ticket(this.x.cbcw_sign_out(global ? 1 : 0, this.now()) >>> 0);
  }

  user(): CbUser | null {
    const u = readcbc_user(this.mem(), this.x.cbcw_user());
    return u.signedIn ? { userId: u.userId, name: u.name, expiresAt: Number(u.expiresAt) } : null;
  }

  close(): void {
    this.stopped = true;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    const ws = this.ws;
    this.ws = null;
    if (ws) ws.close();
    for (const w of this.waiting.values()) w.reject(new Error('c_base client closed'));
    this.waiting.clear();
  }

  // ---- the module's memory --------------------------------------------------------------------
  private mem() {
    return memOf(this.x.memory.buffer);
  }
  private bytes(off: number, n: number): Uint8Array {
    return new Uint8Array(this.x.memory.buffer, this.io + off, n);
  }
  private put(b: Uint8Array): number {
    this.mem().u8.set(b, this.io);
    return b.length;
  }
  private copyIo(off: number, n: number): ArrayBuffer {
    return this.x.memory.buffer.slice(this.io + off, this.io + off + n);
  }

  private ticket(t: number): Promise<ArrayBuffer> {
    if (!t) return Promise.reject(new CbError(0, 0, this.x.cbcw_error(), 0));
    const pr = new Promise<ArrayBuffer>((resolve, reject) => this.waiting.set(t, { resolve, reject }));
    this.pump();
    return pr;
  }

  // ---- the session ----------------------------------------------------------------------------
  private loadSession(): void {
    const key = this.o.sessionKey;
    if (!key || this.o.anonymous) return;
    try {
      const s = globalThis.localStorage?.getItem(key);
      if (s) this.x.cbcw_session_load(this.put(unb64(s)));
    } catch {
      // storage unavailable: the client starts signed out
    }
  }
  private saveSession(): void {
    const key = this.o.sessionKey;
    if (!key || this.o.anonymous) return;
    const n = this.x.cbcw_session_save();
    try {
      if (n > 0) globalThis.localStorage?.setItem(key, b64(this.bytes(0, n)));
      else globalThis.localStorage?.removeItem(key);
    } catch {
      // storage unavailable: the session lasts this page
    }
  }

  // ---- events ---------------------------------------------------------------------------------
  // The events the last call raised, read whole before any is acted on (an action may call in again,
  // which replaces them). `body` is what the span of a DONE from HTTP is into; a socket's spans are
  // into io, copied out here.
  private events(body: ArrayBuffer | null): void {
    const n = this.x.cbcw_n_events();
    if (n <= 0) return;
    const evs: { e: cbc_event_Snap; bytes: ArrayBuffer | null }[] = [];
    for (let i = 0; i < n; i++) {
      const e = readcbc_event(this.mem(), this.x.cbcw_event(i));
      let bytes: ArrayBuffer | null = null;
      if (e.kind === CBC_EV_PUSH) bytes = this.copyIo(e.off, e.len);
      else if (e.kind === CBC_EV_DONE) bytes = body ? body.slice(e.off, e.off + e.len) : this.copyIo(e.off, e.len);
      evs.push({ e, bytes });
    }
    for (const { e, bytes } of evs) {
      switch (e.kind) {
        case CBC_EV_READY:
          this.o.onState?.('open');
          break;
        case CBC_EV_PUSH:
          this.o.onPush?.(e.push, bytes!);
          break;
        case CBC_EV_SUB_LIVE:
          this.o.onSubLive?.(e.what, e.room);
          break;
        case CBC_EV_SUB_REFUSED:
          this.o.onSubRefused?.(e.what, e.room, e.status);
          break;
        case CBC_EV_DONE: {
          const w = this.waiting.get(e.ticket >>> 0);
          this.waiting.delete(e.ticket >>> 0);
          if (!w) break;
          if (e.why === 0) w.resolve(bytes!);
          else w.reject(new CbError(e.status, e.why, e.code, e.detail));
          break;
        }
        case CBC_EV_SESSION:
          this.saveSession();
          this.o.onSession?.(this.user());
          break;
        default:
          break;   // a frame that is not the protocol's: the module counted it, nothing to do
      }
    }
  }

  // ---- the loop -------------------------------------------------------------------------------
  // Opens the socket when the client wants one, sends every frame and request it has, and sets the
  // one timer. Called after anything happens.
  private pump(): void {
    if (this.stopped) return;
    const now = this.now();
    if (!this.ws && now >= this.reopenAt && this.x.cbcw_stream_wanted()) this.openSocket();
    for (;;) {
      const n = this.x.cbcw_next_out(this.now());
      if (n === 0) break;
      if (n < 0) {
        const ws = this.ws;
        this.ws = null;
        if (ws) ws.close();
        this.socketClosed();
        break;
      }
      if (this.ws && this.ws.readyState === 1) this.ws.send(this.copyIo(0, n));
    }
    for (;;) {
      const n = this.x.cbcw_next_request(this.now());
      if (n <= 0) break;
      this.send(n);
    }
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    let due = this.x.cbcw_due_ms(this.now());
    if (!this.ws && this.reopenAt > now) due = due < 0 ? this.reopenAt - now : Math.min(due, this.reopenAt - now);
    if (due >= 0) {
      this.timer = setTimeout(() => {
        this.timer = null;
        if (this.stopped) return;
        this.x.cbcw_tick(this.now());
        this.events(null);
        this.pump();
      }, due);
    }
  }

  private openSocket(): void {
    this.o.onState?.('connecting');
    const ws = new this.WS(this.wsUrl);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      if (ws !== this.ws) return;
      this.x.cbcw_opened(this.now());
      this.pump();
    };
    ws.onmessage = (ev: MessageEvent) => {
      if (ws !== this.ws || !(ev.data instanceof ArrayBuffer)) return;
      const b = new Uint8Array(ev.data);
      if (b.length > this.ioCap) return;   // larger than any frame this protocol sends a page
      this.mem().u8.set(b, this.io);
      this.x.cbcw_in(b.length, this.now());
      this.events(null);
      this.pump();
    };
    ws.onclose = () => {
      if (ws !== this.ws) return;
      this.ws = null;
      this.socketClosed();
      this.pump();
    };
    ws.onerror = () => {
      // onclose follows
    };
  }

  private socketClosed(): void {
    const wait = this.x.cbcw_closed(this.now()) >>> 0;
    this.reopenAt = this.now() + wait;
    this.events(null);
    this.o.onState?.('closed');
  }

  private send(n: number): void {
    const m = this.mem();
    const h = readcbc_req_hdr(m, this.io);
    let at = cbc_req_hdr_SIZE;
    const dec = new TextDecoder();
    const path = dec.decode(this.bytes(at, h.pathLen));
    at += h.pathLen;
    const auth = h.authLen ? dec.decode(this.bytes(at, h.authLen)) : '';
    at += h.authLen;
    const body = h.bodyLen ? this.copyIo(at, h.bodyLen) : undefined;
    if (at + h.bodyLen !== n) throw new Error('c_base request: lengths disagree');
    const id = h.id >>> 0;
    const wantBody = (h.flags & CBC_RQ_BODY) !== 0;
    const headers: Record<string, string> = {};
    if (auth) headers.authorization = auth;
    if (body) headers['content-type'] = 'application/octet-stream';
    this.fetchFn(this.http + path, { method: h.method === CBC_POST ? 'POST' : 'GET', headers, body, cache: 'no-store' })
      .then(async (res) => ({ status: res.status, buf: await res.arrayBuffer() }))
      .then(
        ({ status, buf }) => this.answered(id, status, buf, wantBody),
        () => this.answered(id, 0, new ArrayBuffer(0), wantBody),
      );
  }

  private answered(id: number, status: number, buf: ArrayBuffer, wantBody: boolean): void {
    if (this.stopped) return;
    const ok = status >= 200 && status < 300;
    let given = 0;
    if ((wantBody || !ok) && buf.byteLength <= this.ioCap) {
      this.mem().u8.set(new Uint8Array(buf), this.io);
      given = buf.byteLength;
    }
    this.x.cbcw_on_response(id, status, given, buf.byteLength, this.now());
    this.events(buf);
    this.pump();
  }
}
