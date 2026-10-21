// affinity.h - THE SATURATION KNOB (OPTIMIZED_DESIGN.md step 5, as section 10 left it): serve may
// put its loop on one CPU and every other thread on another (CBASE_LOOP_CPU, CBASE_BG_CPU, both
// unset by default). Measured on a t4g.nano it changes nothing at human pace and buys about 6%
// more moves a second at saturation, so production leaves it off; it is here for a box that runs
// saturated. The pools are sized from the configuration, never from the CPUs a thread may use.
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

#endif
