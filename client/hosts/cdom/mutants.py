# mutants.py - the cdom adapter's mutants (test/mutate.sh runs every hosts/<name>/mutants.py): each
# breaks one behaviour of cbc_cdom.c, or of the shared host's stop (which only this suite drives),
# often back to what the adapter before the shared layer did, and its suite (build/test/unit_host_cdom:
# the host suite and the adapter's own cases, test/unit_host_cdom.c) must go red, with the text in the
# row's last field. (file, name, old, new, want): old occurs once. OBJ: every object a mutant touches.
BUILD = 'build/test/unit_host_cdom'
RUN = './build/test/unit_host_cdom'
OBJ = 'build/obj/host-cdom/_root/client/hosts/cdom/cbc_cdom.o build/obj/host-cdom/_root/client/hosts/common/cbc_host.o'
H = 'hosts/cdom/cbc_cdom.c'
HC = 'hosts/common/cbc_host.c'   # the shared host's stop, which only this suite drives
MUTANTS = [
  # (a) a stop and a new start
  (H, 'an older start\'s answers handed on', '    return o->h->live && o->gen == o->h->gen;', '    return 1;',
   'FAIL  cdom: a request\'s answer after a stop'),
  (H, 'a stop does not move the generation on', '    h->gen++;   // every answer', '    h->gen += 0;   // every answer',
   'FAIL  cdom: a request\'s answer after a stop'),
  (HC, 'a stop leaves the tick armed', '    if (h->tick_timer) h->env->timer_stop(h->ud, h->tick_timer);\n    if (h->reopen', '    if (h->reopen',
   'FAIL  cdom: a stop cancels every timer'),
  (HC, 'a stop leaves the socket open', '    if (h->has_sock) h->env->ws_close(h->ud, h->sock);   // its close', '    (void)0;   // its close',
   'FAIL  cdom: a stop cancels every timer'),
  (H, 'the adapter\'s stop leaves the shared host running', '    cbc_host_stop(&h->host);   // its', '    (void)0;   // its',
   'FAIL  cdom: a stop inside DONE, SOCKET_FAILED or ready'),
  (HC, 'a stopped host still pumps', '    if (h->held || h->stopped) return;', '    if (h->held) return;',
   'FAIL  cdom: a stop inside DONE, SOCKET_FAILED or ready'),
  (HC, 'a stop in the pump\'s own SOCKET_FAILED goes on', '    if (!h->has_sock && !h->reopen_timer && cbc_stream_wanted(&h->c)) ws_open(h);\n    if (h->stopped) return;',
   '    if (!h->has_sock && !h->reopen_timer && cbc_stream_wanted(&h->c)) ws_open(h);', 'FAIL  cdom: a stop inside DONE, SOCKET_FAILED or ready'),
  (HC, 'a stop mid-batch tells the rest', 'i < n && !h->stopped; i++', 'i < n; i++',
   'FAIL  cdom: a stop inside DONE, SOCKET_FAILED or ready'),
  (HC, 'the app\'s callback not counted', '    h->depth++;\n    h->fn(h->ctx, &h->c, e, bytes);\n    h->depth--;', '    h->fn(h->ctx, &h->c, e, bytes);',
   'FAIL  cdom: a start from inside DONE or ready traps'),
  (H, 'ready not counted', '    if (h->in_ready || cbc_host_in_app(&h->host)) __builtin_trap();', '    if (cbc_host_in_app(&h->host)) __builtin_trap();',
   'FAIL  cdom: a start from inside DONE or ready traps'),
  (H, 'a start on a live handle goes on', '    if (h->live) __builtin_trap();', '    (void)0;',
   'FAIL  cdom: a start on a live handle traps'),
  (H, 'past the slot table, never answered', '    if (!h->late_timer) h->late_timer = cd_timer_cb(0, on_late, h);', '    (void)on_late;',
   'FAIL  cdom: past the slot table'),
  # (b) the key
  (H, 'a key past CBC_CDOM_KEY_MAX cut short', '    if (kl > CBC_CDOM_KEY_MAX) return -1;', '    if (kl > CBC_CDOM_KEY_MAX) kl = CBC_CDOM_KEY_MAX;',
   'FAIL  cdom: a key past CBC_CDOM_KEY_MAX'),
  # (c) a page that is not http: or https:
  (H, 'a file: page opens a socket', '    if (!sl) return CBC_SOCK_UNSUPPORTED;   // file:', '    if (!sl && 0) return CBC_SOCK_UNSUPPORTED;   // file:',
   'FAIL  cdom: a file: page opens no socket'),
  # (d) the test gaps: wss, the timeout, ids from 0, the deadline
  (H, 'a host past 256 bytes taken', '    if (on - sl > 256) return CBC_SOCK_UNSUPPORTED;', '    if (on - sl > 259) return CBC_SOCK_UNSUPPORTED;',
   'FAIL  cdom: a host:port of 256 bytes'),
  (H, 'an https: page opens ws://', '    memcpy(url + 2, origin + 4, on - 4);', '    memcpy(url + 2, origin + 5, on - 5);',
   'a socket URL not on the page\'s scheme and host'),
  (H, 'the origin\'s scheme kept', '    memcpy(url, "ws", 2);', '    memcpy(url, "ht", 2);',
   'a socket URL not on the page\'s scheme and host'),
  # (e) a socket the browser refuses to make
  (H, 'a refused socket is a plain failure', '    else if (ev == CD_WS_REFUSED) cbc_host_ws_failed(&h->host, id, CBC_SOCK_REFUSED);', '    else if (ev == CD_WS_REFUSED) cbc_host_ws_failed(&h->host, id, CBC_SOCK_FAILED);',
   'FAIL  cdom: a refused socket'),
  (H, 'a refused socket unheard', '    else if (ev == CD_WS_REFUSED) cbc_host_ws_failed(&h->host, id, CBC_SOCK_REFUSED);\n', '',
   'FAIL  cdom: a refused socket'),
  (H, 'CD_HTTP_TIMEOUT taken for a status', 'if (status == CD_HTTP_FAILED || status == CD_HTTP_TIMEOUT) status = 0;', 'if (status == CD_HTTP_FAILED) status = 0;',
   '[a_slow_borrowed_upload_is_not_aborted]'),
  (H, 'cdom\'s socket id + 1', '    *id = cd_ws_open_cb(on_ws, h, url, n + k);', '    *id = cd_ws_open_cb(on_ws, h, url, n + k) + 1;',
   '[signs_up_then_says_hello]'),
  (H, 'the deadline the call wait', '               (uint32_t)r->body_len, r->wait_ms);', '               (uint32_t)r->body_len, cbc_cdom_client(h)->cfg.call_wait_ms);',
   'a request whose deadline is not http_wait_ms + body_len'),
  # the borrowed body: cbc_req_body's bytes over HTTP, never the head buffer
  (H, 'a borrowed body sent from the head buffer', 'h->hdr, hl, r->body,', 'h->hdr, hl, (const uint8_t *)r->path + r->path_len + r->auth_len,',
   '[a_64k_body_arrives_byte_for_byte_and_done_comes_once_after]'),
  (H, 'a borrowed body copied', '    cbc_cdom_owed *o = owe(h, tag);\n', '    static uint8_t copy[70000];\n    cbc_host_req rc = *r;\n    if (r->body_len <= sizeof copy) { memcpy(copy, r->body, r->body_len); rc.body = copy; r = &rc; }\n    cbc_cdom_owed *o = owe(h, tag);\n',
   'r->body_at == g_big'),
  # the request's headers, the session in the store, the load
  (H, 'no Authorization header', '    if (r->auth) {   // "Bearer <access>"', '    if (0) {   // "Bearer <access>"',
   '[an_http_call_carries_the_bearer_and_is_retried_by_the_timer]'),
  (H, 'no Content-Type', '    if (r->body_len) { memcpy(h->hdr + hl, CT', '    if (0) { memcpy(h->hdr + hl, CT',
   'a body without its octet-stream type'),
  (H, 'the session stored as raw bytes', 'cd_store_put_cb(on_stored, NULL, h->key, h->klen, h->text, (uint32_t)t);',
   'cd_store_put_cb(on_stored, NULL, h->key, h->klen, p, (uint32_t)len);', 'a put that is not the session\'s base64'),
  (H, 'signed out keeps the session', '    else cd_store_del_cb(on_stored, NULL, h->key, h->klen);', '    else (void)0;',
   '[a_sign_out_closes_and_forgets]'),
  (H, 'the kept session not decoded', 'int n = len > 0 ? b64_decode((const char *)p, (size_t)len, h->host.sess, sizeof h->host.sess) : -1;',
   'int n = len > 0 && len <= CBC_HOST_SESS_CAP ? len : -1;\n    if (n > 0) memcpy(h->host.sess, p, (size_t)n);', '[the_kept_session_is_used_at_start]'),
  (H, 'an anonymous client asks the store', '    size_t kl = session_key && !k.anonymous ? strlen(session_key) : 0;', '    size_t kl = session_key ? strlen(session_key) : 0;',
   'an anonymous client asked the store'),
  (H, 'no hold while the load is out', '        cbc_host_hold(&h->host, 1);   // until the store answers', '        (void)0;   // until the store answers',
   'FAIL  cdom: a sign-up made before'),
  (H, 'no ready after the load', '    tell_ready(h);\n    cbc_host_pump(&h->host);\n}\n\nstatic void on_loaded',
   '    cbc_host_pump(&h->host);\n}\n\nstatic void on_loaded', 'ready was not called once'),
  (H, 'no seed check', '    if (cd_random_bytes(&k.seed, sizeof k.seed) != CD_RANDOM_OK) return -1;', '    cd_random_bytes(&k.seed, sizeof k.seed);',
   'FAIL  cdom: a page without crypto.getRandomValues'),
]
