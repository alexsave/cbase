#include "log.h"

#include "clock.h"
#include "str.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cb_log_entry *g_ring;
static size_t g_cap;
static uint64_t g_seq;
static atomic_int g_min = CB_LOG_INFO;   // written under g_lock, read without it

static const char *NAMES[] = { "DEBUG", "INFO", "WARN", "ERROR" };

void cb_log_init(int min_level, size_t ring) {
    pthread_mutex_lock(&g_lock);
    atomic_store_explicit(&g_min, min_level, memory_order_relaxed);
    if (!g_ring) {
        g_cap = ring ? ring : 2048;
        g_ring = calloc(g_cap, sizeof *g_ring);
        if (!g_ring) g_cap = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

int cb_log_level_parse(const char *s) {
    for (int i = 0; i < 4; i++) if (strcasecmp(s, NAMES[i]) == 0) return i;
    return -1;
}

const char *cb_log_level_name(int level) { return level >= 0 && level < 4 ? NAMES[level] : "?"; }

int cb_log_enabled(int level) {
    if (level > CB_LOG_ERROR) level = CB_LOG_ERROR;
    return level >= atomic_load_explicit(&g_min, memory_order_relaxed);
}

void cb_log(int level, const char *module, const char *fmt, ...) {
    if (level < CB_LOG_DEBUG) level = CB_LOG_DEBUG;
    if (level > CB_LOG_ERROR) level = CB_LOG_ERROR;
    // The level first: a dropped line is not formatted, timestamped or locked for.
    if (!cb_log_enabled(level)) return;
    cb_log_entry e;
    e.ms = cb_now_ms();
    e.level = level;
    cb_strlcpy(e.module, module ? module : "-", sizeof e.module);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof e.msg, fmt, ap);
    va_end(ap);
    char ts[40];
    cb_iso8601(e.ms, ts, sizeof ts);

    pthread_mutex_lock(&g_lock);
    e.seq = ++g_seq;
    if (g_cap) g_ring[(e.seq - 1) % g_cap] = e;
    // Under the lock, so two threads' lines never interleave mid-line.
    fprintf(stderr, "%s %-5s %s: %s\n", ts, NAMES[level], e.module, e.msg);
    pthread_mutex_unlock(&g_lock);
}

size_t cb_log_read(uint64_t after_seq, int min_level, cb_log_entry *out, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&g_lock);
    if (g_cap) {
        uint64_t oldest = g_seq > g_cap ? g_seq - g_cap + 1 : 1;
        uint64_t s = after_seq + 1 > oldest ? after_seq + 1 : oldest;
        for (; s <= g_seq && n < cap; s++) {
            const cb_log_entry *e = &g_ring[(s - 1) % g_cap];
            if (e->level >= min_level) out[n++] = *e;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t cb_log_last_seq(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t s = g_seq;
    pthread_mutex_unlock(&g_lock);
    return s;
}
