// fz_dynamo_store.c - the DynamoDB backend (store/store_dynamo.c) end to end
// against an AWS that answers anything: each store call it makes is answered
// with a status and a body from the input, through the same seam the unit
// tests record against (store_priv.h cb_store_open_dynamo_with). This reaches
// the item decoders where they are used (a row with a missing or mistyped
// attribute, a list that pages forever, a transaction cancelled for a reason
// that is not one), the retry and pagination loops, and what each store
// function hands back to its caller.
//
// Input: a program of store calls (one byte each, with arguments), and the
// answers they consume in order: [status pick][u16 length][body].
#include "fz.h"

#include "store/rows_auth.h"
#include "store/store_priv.h"
#include "util/log.h"

static fz_in g_in;
static int g_sends;
static int64_t g_clock;

static int64_t fake_mono(void) { return g_clock; }
static void fake_sleep(int64_t ms) { g_clock += ms; }

static int fake_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    (void)ud; (void)target; (void)body; (void)len;
    // Bounded: a loop that never stops asking is a hang, and libFuzzer's
    // timeout reports it; the cap keeps each run short when it does stop.
    if (fz_left(&g_in) == 0 || ++g_sends > 200) return CB_E_IO;
    static const int ST[] = { 200, 200, 200, 200, 400, 400, 500, 503 };
    *status = ST[fz_u8(&g_in) % 8];
    size_t n;
    const uint8_t *b = fz_bytes(&g_in, fz_u16(&g_in) % 8192, &n);
    cb_buf_append(resp, b, n);
    return CB_OK;
}

static bool on_user(const cb_user_row *u, void *ud) { (void)strlen(u->email); return true; }
static bool on_name(const char *n, void *ud) { (void)strlen(n); return true; }
static bool on_row(int n, const char *const *names, const char *const *values, void *ud) { return true; }

#define U1 "11111111-1111-4111-8111-111111111111"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The first byte says where the program ends and the answers begin.
    if (size < 1) return 0;
    size_t split = 1 + (size_t)data[0] * (size - 1) / 256;
    fz_in prog = fz_in_of(data + 1, split - 1);
    g_in = fz_in_of(data + split, size - split);
    g_sends = 0;
    g_clock = 1;
    cb_store *s = NULL;
    if (cb_store_open_dynamo_with("cbase", fake_send, NULL, &s) != 0) abort();
    cb_store_dynamo_clock(s, fake_mono, fake_sleep);
    cb_store_set_deadline(0);
    for (int step = 0; step < 16 && fz_left(&prog); step++) {
        int op = fz_u8(&prog) % 24;
        // A product's own rows (ops 2..14, 19 and 20 of its target's program) are its own target's:
        // here those ops do nothing, so one corpus drives both.
        cb_user_row u; cb_refresh_row rr;
        switch (op) {
        case 0: (void)CB_STORE_CALL(s, user_get, U1, &u); break;
        case 1: (void)CB_STORE_CALL(s, user_by_email, "a@b.io", &u); break;
        case 15: (void)CB_STORE_CALL(s, refresh_take, "00", 1000, 60000, &rr); break;
        case 16: (void)CB_STORE_CALL(s, users_list, "", 0, 10, on_user, NULL); break;
        case 17: (void)CB_STORE_CALL(s, admin_collections, on_name, NULL); break;
        case 18: { char next[256]; (void)CB_STORE_CALL(s, admin_browse, "users", "", "", 10, on_row, NULL, next, sizeof next); break; }
        case 21: (void)CB_STORE_CALL(s, admin_query, "SELECT * FROM cbase", false, on_row, NULL); break;
        // The readers the security fixes added (H1's sign-in failures, M1's sessions).
        case 22: { cb_session_row se; (void)CB_STORE_CALL(s, session_get, "s1", 1000, &se); break; }
        case 23: { cb_signin_fails sf; (void)CB_STORE_CALL(s, signin_fails_get, "a@b.io", 1000, &sf); break; }
        }
    }
    cb_store_close(s);
    return 0;
}
