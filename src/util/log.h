// log.h - one log for the process: stderr, plus a ring buffer the admin
// console tails.
//
// Lines go to stderr (journald keeps them on the VM) as
//   2026-10-13T12:34:56.789000+00:00 INFO  app: game 1a2b3c committed v12
// and into a fixed ring of the last N entries, each with a sequence number, so
// the console can poll "everything after seq S at level >= L" without missing
// or repeating a line. Thread-safe; never allocates after cb_log_init.
#ifndef CB_LOG_H
#define CB_LOG_H

#include <stddef.h>
#include <stdint.h>

enum { CB_LOG_DEBUG = 0, CB_LOG_INFO = 1, CB_LOG_WARN = 2, CB_LOG_ERROR = 3 };

#define CB_LOG_MODULE_MAX 16
#define CB_LOG_MSG_MAX    240

typedef struct {
    uint64_t seq;                       // 1, 2, 3, ... in the order logged
    int64_t  ms;                        // cb_now_ms() when logged
    int      level;                     // CB_LOG_*
    char     module[CB_LOG_MODULE_MAX];
    char     msg[CB_LOG_MSG_MAX];       // cut, never split across entries
} cb_log_entry;

// `min_level`: lines below it are dropped entirely (not printed, not kept).
// `ring`: entries kept for cb_log_read (0 picks 2048). Safe to call once,
// before any thread logs; logging before it prints to stderr only.
void cb_log_init(int min_level, size_t ring);
void cb_log_set_level(int min_level);
// "debug" | "info" | "warn" | "error" -> CB_LOG_*, or -1.
int  cb_log_level_parse(const char *s);
const char *cb_log_level_name(int level);

void cb_log(int level, const char *module, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define CB_DEBUG(mod, ...) cb_log(CB_LOG_DEBUG, (mod), __VA_ARGS__)
#define CB_INFO(mod, ...)  cb_log(CB_LOG_INFO, (mod), __VA_ARGS__)
#define CB_WARN(mod, ...)  cb_log(CB_LOG_WARN, (mod), __VA_ARGS__)
#define CB_ERROR(mod, ...) cb_log(CB_LOG_ERROR, (mod), __VA_ARGS__)

// Copies the retained entries with seq > after_seq and level >= min_level,
// oldest first, at most `cap` of them. Returns how many were copied. An entry
// that has already been overwritten is simply gone (the caller sees a gap in seq).
size_t cb_log_read(uint64_t after_seq, int min_level, cb_log_entry *out, size_t cap);
// The seq of the newest entry (0 before the first).
uint64_t cb_log_last_seq(void);

#endif
