// oom.h - a failing allocator for the tests, with no seam in the code under test.
//
// Included by exactly ONE file of a test program (unit_oom.c), it DEFINES
// malloc, calloc, realloc and free for that program. The static linker
// resolves every c_base and kernel object's call to them here (the
// definitions are in the program being linked); on macOS the system's
// libraries keep their own (two-level namespaces), on glibc they reach these
// too, which forward to __libc_malloc and friends. Every block comes from the
// system allocator, so either side may free it.
//
//   oom_fail_after(n)   the calling thread's (n+1)-th allocation from now
//                       fails, and only that one: n = 0 fails the next.
//   oom_off()           stop failing (also: the one failure has happened).
//   oom_count()         allocations by the armed thread since oom_fail_after.
//   oom_failed()        true once the armed failure has been delivered.
//   oom_live()          blocks the armed thread allocated since oom_fail_after
//                       and has not freed: a leak, once the operation is over
//                       and whatever it may keep (a cache) is let go.
//
// Only the thread that armed it fails, so a pool worker's allocation never
// fails behind a test's back. And only an allocation the program's own code
// makes (c_base and the kernel): on glibc the definitions also catch every
// shared library's calls (sqlite's caches, OpenSSL's lazily made tables), whose
// failures and long-lived blocks are the libraries' business, so a caller
// outside the executable's text is passed through, as macOS's two-level
// namespaces pass it. oom_everywhere(true) lifts that, where it can (glibc),
// for a sweep that means to reach into a library (a JWT's HMAC). pthread_self allocates nothing, which
// _Thread_local storage on macOS can (the first touch of a TLV mallocs).
//
// The usual way to use it is a sweep: run the operation with the k-th
// allocation failing for k = 0, 1, 2, ... until a run makes no failure
// (oom_failed() false), checking each run returns its error and leaks nothing
// (the ASan lane) instead of crashing.
//
// Not under AddressSanitizer: its runtime owns malloc, and replacing it would
// blind ASan. OOM_AVAILABLE is 0 there and a sweep runs once, unfailed.
#ifndef CB_TEST_OOM_H
#define CB_TEST_OOM_H

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define OOM_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define OOM_ASAN 1
#endif

#ifdef OOM_ASAN
#  define OOM_AVAILABLE 0
static inline void oom_fail_after(long n) { (void)n; }
static inline void oom_off(void) {}
static inline long oom_count(void) { return 0; }
static inline bool oom_failed(void) { return false; }
static inline long oom_live(void) { return 0; }
static inline void oom_dump_live(FILE *f) { (void)f; }
static inline void oom_trace(bool on) { (void)on; }
static inline void oom_everywhere(bool on) { (void)on; }
#else
#  define OOM_AVAILABLE 1

#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define OOM_REAL_MALLOC(n) malloc_zone_malloc(malloc_default_zone(), (n))
#  define OOM_REAL_CALLOC(c, n) malloc_zone_calloc(malloc_default_zone(), (c), (n))
// realloc and free go to the zone that owns the block: a block the system
// allocated (strdup, a library) may live in another zone than the default.
static inline void *oom_real_realloc_(void *p, size_t n) {
    if (!p) return OOM_REAL_MALLOC(n);
    malloc_zone_t *z = malloc_zone_from_ptr(p);
    return malloc_zone_realloc(z ? z : malloc_default_zone(), p, n);
}
static inline void oom_real_free_(void *p) {
    malloc_zone_t *z = p ? malloc_zone_from_ptr(p) : NULL;
    if (z) malloc_zone_free(z, p);
}
#  define OOM_REAL_REALLOC(p, n) oom_real_realloc_((p), (n))
#  define OOM_REAL_FREE(p) oom_real_free_(p)
#else
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
#  define OOM_REAL_MALLOC(n) __libc_malloc(n)
#  define OOM_REAL_CALLOC(c, n) __libc_calloc((c), (n))
#  define OOM_REAL_REALLOC(p, n) __libc_realloc((p), (n))
#  define OOM_REAL_FREE(p) __libc_free(p)
#endif

// THE ARMED WINDOW: oom_fail_after opens it, oom_off closes it. Inside it the
// armed thread's allocations are counted and remembered (a fixed open-address
// set: no allocation of its own), and the chosen one fails; the window stays
// open after the failure, so what the failure path frees is still seen.
static atomic_bool oom_armed_;
static pthread_t oom_thread_;
static long oom_left_, oom_seen_, oom_live_;
static bool oom_hit_;
#define OOM_SET (1 << 16)
static void *oom_set_[OOM_SET];
static void *oom_site_[OOM_SET];   // who allocated each remembered block (oom_dump_live)
// oom_trace(true): also each block's call stack (backtrace(3)), for a leak whose direct caller is an
// allocator wrapper (SQLite's pm_malloc). Off by default: it costs a stack walk per allocation.
#define OOM_FRAMES 8
static bool oom_trace_, oom_in_trace_;
static void *oom_bt_[OOM_SET][OOM_FRAMES];

static inline size_t oom_slot_(void *p) { return ((uintptr_t)p >> 4) * 2654435761u % OOM_SET; }
static void oom_remember_(void *p, void *site) {
    if (!p) return;
    for (size_t i = oom_slot_(p), k = 0; k < OOM_SET; k++, i = (i + 1) % OOM_SET)
        if (!oom_set_[i] || oom_set_[i] == (void *)1) {
            oom_set_[i] = p;
            oom_site_[i] = site;
            memset(oom_bt_[i], 0, sizeof oom_bt_[i]);
            if (oom_trace_ && !oom_in_trace_) {
                oom_in_trace_ = true;
                backtrace(oom_bt_[i], OOM_FRAMES);
                oom_in_trace_ = false;
            }
            oom_live_++;
            return;
        }
}
static void oom_forget_(void *p) {
    if (!p) return;
    for (size_t i = oom_slot_(p), k = 0; k < OOM_SET && oom_set_[i]; k++, i = (i + 1) % OOM_SET)
        if (oom_set_[i] == p) { oom_set_[i] = (void *)1; oom_live_--; return; }   // a tombstone
}

static inline void oom_fail_after(long n) {
    oom_thread_ = pthread_self();
    oom_left_ = n;
    oom_seen_ = oom_live_ = 0;
    oom_hit_ = false;
    memset(oom_set_, 0, sizeof oom_set_);
    atomic_store(&oom_armed_, true);
}
static inline void oom_off(void) { atomic_store(&oom_armed_, false); }
static inline long oom_count(void) { return oom_seen_; }
static inline bool oom_failed(void) { return oom_hit_; }
static inline long oom_live(void) { return oom_live_; }
// Names the code that allocated each block still live (for a leak report): the caller's symbol.
static inline void oom_trace(bool on) { oom_trace_ = on; }
static void oom_name_(FILE *f, const char *pre, void *a) {
    Dl_info d;
    if (dladdr(a, &d) && d.dli_sname)
        fprintf(f, "%s%s+0x%lx\n", pre, d.dli_sname, (unsigned long)((char *)a - (char *)d.dli_saddr));
    else
        fprintf(f, "%s%p\n", pre, a);
}
static void oom_dump_live(FILE *f) {
    for (size_t i = 0; i < OOM_SET; i++) {
        if (!oom_set_[i] || oom_set_[i] == (void *)1) continue;
        oom_name_(f, "       live block from ", oom_site_[i]);
        for (int k = 1; k < OOM_FRAMES && oom_bt_[i][k]; k++) oom_name_(f, "         <- ", oom_bt_[i][k]);
    }
}

static bool oom_everywhere_;
static inline void oom_everywhere(bool on) { oom_everywhere_ = on; }

#if defined(__APPLE__)
#  define OOM_OURS_(caller) ((void)(caller), true)   // the linker already bound only our objects here
#else
// The executable's text, as the default linker script names it.
extern char __executable_start[], etext[];
#  define OOM_OURS_(caller) (oom_everywhere_ || ((char *)(caller) >= __executable_start && (char *)(caller) < etext))
#endif

static bool oom_armed_here_(void) { return atomic_load(&oom_armed_) && pthread_equal(pthread_self(), oom_thread_); }
#define oom_mine_() (oom_armed_here_() && OOM_OURS_(__builtin_return_address(0)))

// true when this allocation is the one to fail.
static bool oom_take_(void) {
    oom_seen_++;
    if (oom_hit_ || oom_left_-- > 0) return false;
    oom_hit_ = true;
    return true;
}

void *malloc(size_t n) {
    if (!oom_mine_()) return OOM_REAL_MALLOC(n);
    if (oom_take_()) return NULL;
    void *p = OOM_REAL_MALLOC(n);
    oom_remember_(p, __builtin_return_address(0));
    return p;
}
void *calloc(size_t c, size_t n) {
    if (!oom_mine_()) return OOM_REAL_CALLOC(c, n);
    if (oom_take_()) return NULL;
    void *p = OOM_REAL_CALLOC(c, n);
    oom_remember_(p, __builtin_return_address(0));
    return p;
}
void *realloc(void *p, size_t n) {
    if (!oom_mine_()) return OOM_REAL_REALLOC(p, n);
    if (oom_take_()) return NULL;
    void *q = OOM_REAL_REALLOC(p, n);
    if (q) { oom_forget_(p); oom_remember_(q, __builtin_return_address(0)); }
    return q;
}
void free(void *p) {
    if (p && oom_armed_here_()) oom_forget_(p);   // whoever frees it, it is no longer live
    OOM_REAL_FREE(p);
}
#endif

#endif
