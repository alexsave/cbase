// fake_apple.c - a stand-in Apple for trying Sign in with Apple end to end on one machine, with no
// Apple ID: a page in a browser, or an app in the simulator, against a real `cbase serve`.
//
//   build/test/fake_apple <env file>
//
// It starts lib_fake_idp.h's Apple (TLS on localhost: /auth/keys, /auth/token, /auth/revoke), writes
// the settings a server needs to trust it into <env file> (mode 0600: CBASE_APPLE_ISSUER,
// CBASE_APPLE_TEAM_ID, CBASE_APPLE_KEY_ID, CBASE_APPLE_PRIVATE_KEY, CBASE_CA_FILE, and FAKE_APPLE_MINT
// for the clients), and serves until it is killed. Beside it, on plain HTTP at FAKE_APPLE_MINT, the
// two things a client asks Apple's own UI for:
//   GET /mint?aud=<client id>&nonce=<hashed nonce>&state=<state>&person=<who>
//       an authorization as Apple's sign-in answers it: {"authorization":{"id_token","code","state"},
//       "user":{"name":{..},"email"}} (the user only the first time a person signs in to that client,
//       as Apple does). The token is signed by the fake's published key, for subject
//       "001234.<person>.fake", with a relay email.
//   GET /appleid.auth.js
//       Apple's JS SDK as far as a page uses it (AppleID.auth.init and signIn), which signIn answers
//       from /mint: the page's own code runs as it would against Apple, popup and all, without one.
// The client ids it mints for are any the request names; the server's CBASE_APPLE_CLIENT_IDS says
// which it accepts. Nothing here is linked into a server.
#include "lib_fake_idp.h"
#include "lib_tls.h"

#include "util/str.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static fake_idp g_f;
static tls_srv g_mint;
static char g_seen[64][160];   // person|client pairs that have signed in (Apple sends the name once)
static int g_n_seen;

static void qget(const char *req, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const char *q = strchr(req, '?');
    const char *end = strstr(req, " HTTP/");
    if (!q || !end) return;
    size_t kl = strlen(key);
    for (const char *p = q + 1; p < end;) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *e = amp ? amp : end;
        if ((size_t)(e - p) > kl && p[kl] == '=' && strncmp(p, key, kl) == 0) {
            size_t n = (size_t)(e - p) - kl - 1, o = 0;
            for (size_t i = 0; i < n && o + 1 < cap; i++) {
                char c = p[kl + 1 + i];
                if (c == '%' && i + 2 < n) {
                    char h[3] = { p[kl + 2 + i], p[kl + 3 + i], 0 };
                    out[o++] = (char)strtol(h, NULL, 16);
                    i += 2;
                } else {
                    out[o++] = c;
                }
            }
            out[o] = 0;
            return;
        }
        p = amp ? amp + 1 : end;
    }
}

static void send_body(cb_buf *resp, int status, const char *type, const char *body) {
    cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                        "Content-Length: %zu\r\n\r\n%s",
                  status, type, strlen(body), body);
}

static const char SDK[] =
    "// The fake Apple's appleid.auth.js (c_base/core/test/fake_apple.c): AppleID.auth.init and signIn, as\n"
    "// Apple's SDK answers them in popup mode, from the fake's /mint.\n"
    "(function () {\n"
    "  var here = document.currentScript ? new URL(document.currentScript.src).origin : '';\n"
    "  var opts = null;\n"
    "  window.AppleID = { auth: {\n"
    "    init: function (o) { opts = o; },\n"
    "    signIn: function () {\n"
    "      if (!opts) return Promise.reject({ error: 'not_initialized' });\n"
    "      var person = window.FAKE_APPLE_PERSON || 'ada';\n"
    "      var u = here + '/mint?aud=' + encodeURIComponent(opts.clientId) + '&nonce=' + encodeURIComponent(opts.nonce) +\n"
    "        '&state=' + encodeURIComponent(opts.state) + '&person=' + encodeURIComponent(person);\n"
    "      return fetch(u).then(function (r) { if (!r.ok) throw { error: 'mint_failed' }; return r.json(); });\n"
    "    }\n"
    "  } };\n"
    "})();\n";

static tls_action mint(const cb_buf *req, cb_buf *resp, void *ud) {
    (void)ud;
    const char *r = (const char *)req->p;
    if (strncmp(r, "GET /appleid.auth.js", 20) == 0) {
        send_body(resp, 200, "text/javascript", SDK);
        return TLS_CLOSE;
    }
    if (strncmp(r, "GET /mint?", 10) != 0) {
        send_body(resp, 404, "text/plain", "not found\n");
        return TLS_CLOSE;
    }
    char aud[256], nonce[128], state[64], person[64], sub[160], email[200], code[200];
    qget(r, "aud", aud, sizeof aud);
    qget(r, "nonce", nonce, sizeof nonce);
    qget(r, "state", state, sizeof state);
    qget(r, "person", person, sizeof person);
    for (char *p = person; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) *p = 'x';
    if (!aud[0] || !nonce[0] || !person[0]) {
        send_body(resp, 400, "text/plain", "aud, nonce and person, please\n");
        return TLS_CLOSE;
    }
    snprintf(sub, sizeof sub, "001234.%s.fake", person);
    snprintf(email, sizeof email, "%s.relay@privaterelay.appleid.com", person);
    snprintf(code, sizeof code, "code-%s", sub);
    int64_t now = (int64_t)time(NULL);
    fi_claims c = { .iss = g_f.issuer, .sub = sub, .aud = aud, .nonce_hashed = nonce, .email = email,
                    .email_verified = "\"true\"", .is_private_email = "\"true\"", .iat = now, .exp = now + 600 };
    char token[16384];
    const char *t = fi_mint(&g_f, 0, NULL, NULL, &c);
    cb_strlcpy(token, t ? t : "", sizeof token);
    char key[160];
    snprintf(key, sizeof key, "%s|%s", person, aud);
    bool first = true;
    for (int i = 0; i < g_n_seen; i++)
        if (strcmp(g_seen[i], key) == 0) first = false;
    if (first && g_n_seen < 64) cb_strlcpy(g_seen[g_n_seen++], key, sizeof g_seen[0]);
    char first_name[64];
    snprintf(first_name, sizeof first_name, "%s", person);
    if (first_name[0] >= 'a' && first_name[0] <= 'z') first_name[0] = (char)(first_name[0] - 'a' + 'A');
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_printf(&b, "{\"authorization\":{\"id_token\":\"%s\",\"code\":\"%s\",\"state\":\"%s\"}", token, code, state);
    if (first) cb_buf_printf(&b, ",\"user\":{\"name\":{\"firstName\":\"%s\",\"lastName\":\"Fake\"},\"email\":\"%s\"}", first_name, email);
    cb_buf_puts(&b, "}");
    send_body(resp, 200, "application/json", cb_buf_cstr(&b));
    cb_buf_free(&b);
    printf("fake apple: minted for %s at %s%s\n", sub, aud, first ? " (first: with the name)" : "");
    fflush(stdout);
    return TLS_CLOSE;
}

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fake_apple <env file>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (fi_start(&g_f) != 0 || tls_srv_start_plain(&g_mint, mint, NULL) != 0) {
        fprintf(stderr, "fake apple: did not start\n");
        return 1;
    }
    umask(077);
    FILE *f = fopen(argv[1], "w");
    if (!f) { perror(argv[1]); return 1; }
    fprintf(f, "CBASE_APPLE_ISSUER=%s\nCBASE_APPLE_TEAM_ID=%s\nCBASE_APPLE_KEY_ID=%s\nCBASE_APPLE_PRIVATE_KEY='%s'\n"
               "CBASE_CA_FILE=%s\nFAKE_APPLE_MINT=http://127.0.0.1:%d\n",
            g_f.issuer, g_f.team_id, g_f.key_id, fi_p8_pem(&g_f, true), g_f.srv.ca_file, g_mint.port);
    fclose(f);
    printf("fake apple: issuer %s, mint http://127.0.0.1:%d, settings in %s\n", g_f.issuer, g_mint.port, argv[1]);
    fflush(stdout);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!g_stop) pause();
    tls_srv_stop(&g_mint);
    fi_stop(&g_f);
    return 0;
}
