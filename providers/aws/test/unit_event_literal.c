// unit_event_literal.c - the two string-literal events (aws_json.h): the
// Scheduler's "<schedule>" and a job's "<job>:<key>", and the writer of the
// second, cb_event_job_payload, at the edges of what jobs.h allows.
#include "t.h"

#include "aws/event.h"
#include "aws_json.h"
#include "jobs.h"
#include "util/err.h"

#include <string.h>

static char g_lit[256];

// "\"<s>\"" for s of n copies of c, with a prefix.
static const char *lit(const char *prefix, char c, int n) {
    int k = snprintf(g_lit, sizeof g_lit, "\"%s", prefix);
    memset(g_lit + k, c, (size_t)n);
    snprintf(g_lit + k + n, sizeof g_lit - (size_t)(k + n), "\"");
    return g_lit;
}

static int decode(const char *s, cb_event *ev) {
    int rc = cb_event_decode(s, strlen(s), ev);
    return rc;
}

static void a_job_literal_at_the_edges(void) {
    cb_event ev;
    // A 63-byte key is taken, whole.
    T_EQ_INT(decode(lit("drive:", 'k', 63), &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_JOB);
    T_EQ_STR(ev.job, "drive");
    T_EQ_INT(strlen(ev.key), 63);
    T_CHECK(ev.key[0] == 'k' && ev.key[62] == 'k');
    cb_event_free(&ev);
    // A 64-byte one is refused.
    T_EQ_INT(decode(lit("drive:", 'k', 64), &ev), CB_E_INVALID);
    cb_event_free(&ev);
    // A 31-byte job name is taken, a 32-byte one refused.
    char b[128];
    memset(b, 'j', 31);
    snprintf(b + 31, sizeof b - 31, ":key-1");
    T_EQ_INT(decode(lit(b, 'x', 0), &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_JOB);
    T_EQ_INT(strlen(ev.job), 31);
    T_EQ_STR(ev.key, "key-1");
    cb_event_free(&ev);
    memset(b, 'j', 32);
    snprintf(b + 32, sizeof b - 32, ":key-1");
    T_EQ_INT(decode(lit(b, 'x', 0), &ev), CB_E_INVALID);
    cb_event_free(&ev);
    // One colon parts the two; a second one is in the key, which no key holds.
    T_EQ_INT(decode("\"a:b:c\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"a:b\"", &ev), CB_OK);
    T_EQ_STR(ev.job, "a");
    T_EQ_STR(ev.key, "b");
    cb_event_free(&ev);
    // Either half empty.
    T_EQ_INT(decode("\":b\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"a:\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
    // Every key byte class (A-Z a-z 0-9 - _), as the key.
    T_EQ_INT(decode("\"job_2:AZaz09-_\"", &ev), CB_OK);
    T_EQ_STR(ev.job, "job_2");
    T_EQ_STR(ev.key, "AZaz09-_");
    cb_event_free(&ev);
    // An upper-case job name is not a name.
    T_EQ_INT(decode("\"Job:k\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
}

static void a_schedule_literal_at_the_edges(void) {
    cb_event ev;
    // A 31-byte name is a schedule, a 32-byte one refused.
    T_EQ_INT(decode(lit("", 's', 31), &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_SCHEDULE);
    T_EQ_INT(strlen(ev.schedule), 31);
    cb_event_free(&ev);
    T_EQ_INT(decode(lit("", 's', 32), &ev), CB_E_INVALID);
    cb_event_free(&ev);
    // The name as given, not only "scan".
    T_EQ_INT(decode("\"nightly_2\"", &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_SCHEDULE);
    T_EQ_STR(ev.schedule, "nightly_2");
    cb_event_free(&ev);
    // Empty, unterminated, and a lone quote.
    T_EQ_INT(decode("\"\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"scan", &ev), CB_E_INVALID);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"", &ev), CB_E_INVALID);
    cb_event_free(&ev);
}

static void the_payload_is_what_the_decoder_reads(void) {
    char out[128], key[80], name[40];
    memset(key, 'K', 63);
    key[63] = 0;
    // The longest of each, written and read back.
    memset(name, 'n', 31);
    name[31] = 0;
    int n = cb_event_job_payload(name, key, out, sizeof out);
    T_EQ_INT(n, 31 + 1 + 63 + 2);
    T_EQ_INT((int)strlen(out), n);
    cb_event ev;
    T_EQ_INT(cb_event_decode(out, (size_t)n, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_JOB);
    T_EQ_STR(ev.job, name);
    T_EQ_STR(ev.key, key);
    cb_event_free(&ev);
    T_EQ_INT(cb_event_job_payload("drive", "a1", out, sizeof out), 10);
    T_EQ_STR(out, "\"drive:a1\"");
    // One past each is refused, and nothing a key may not hold is written.
    key[63] = 'K';
    key[64] = 0;
    T_EQ_INT(cb_event_job_payload("drive", key, out, sizeof out), CB_E_INVALID);
    name[31] = 'n';
    name[32] = 0;
    T_EQ_INT(cb_event_job_payload(name, "a1", out, sizeof out), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload("drive", "a:b", out, sizeof out), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload("drive", "a\"b", out, sizeof out), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload("drive", "", out, sizeof out), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload(NULL, "a1", out, sizeof out), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload("drive", NULL, out, sizeof out), CB_E_INVALID);
    // A cap one short of the literal and its NUL refused, the exact one taken.
    T_EQ_INT(cb_event_job_payload("drive", "a1", out, 10), CB_E_INVALID);
    T_EQ_INT(cb_event_job_payload("drive", "a1", out, 11), 10);
}

int main(void) {
    T_RUN(a_job_literal_at_the_edges);
    T_RUN(a_schedule_literal_at_the_edges);
    T_RUN(the_payload_is_what_the_decoder_reads);
    return T_DONE();
}
