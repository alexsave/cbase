// affinity.h - THE SATURATION KNOB (OPTIMIZED_DESIGN.md step 5, as section 10 left it): serve puts
// its loop on one CPU and every other thread on another (CBASE_LOOP_CPU, CBASE_BG_CPU; config.h).
// It is on by default: with both unset serve picks the two lowest CPUs it may use when it starts
// (cb_affinity_pick), and pins nothing where it has fewer than two or cannot pin. `off` turns it
// off, a number pins by hand. Measured on a t4g.nano (the Graviton pass, 6 of 6 paired runs): +12
// to 18% moves a second and 19 to 30% lower move p99 at sat2, about +9% and 7 to 23% lower at sat8,
// no CPU migrations per move; at human pace it changes nothing (an earlier nano measurement). The
// pools are sized from the configuration, never from the CPUs a thread may use.
//
// Linux only (sched_setaffinity's per-thread form); elsewhere every call answers CB_E_NOTIMPL.
#ifndef CB_UTIL_AFFINITY_H
#define CB_UTIL_AFFINITY_H

#include <stdbool.h>
#include <stddef.h>

// Whether this platform can pin a thread.
bool cb_affinity_supported(void);

// Pins the calling thread to `cpu`; a thread it creates afterwards starts with the same CPU (Linux
// threads inherit their creator's mask). cpu < 0 gives the thread back the mask it had at its
// first call here. CB_OK, CB_E_INVALID for a CPU the process may not use (why into err), or
// CB_E_NOTIMPL.
int cb_affinity_pin(int cpu, char *err, size_t cap);

// The CPUs the calling thread may run on (a count), or -1 where that cannot be asked.
int cb_affinity_count(void);

// The knob's automatic choice from the CPUs the calling thread may run on now: the lowest for the
// loop, the next for every other thread. CB_OK; CB_E_INVALID with fewer than two, or CB_E_NOTIMPL
// (why into err). Both outputs are -1 unless CB_OK.
int cb_affinity_pick(int *loop_cpu, int *bg_cpu, char *err, size_t cap);

#endif
