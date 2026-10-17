// lib_faultstore.h - a store that fails one chosen call, for the error paths.
//
// fs_wrap(inner) is a cb_store whose every operation forwards to `inner`
// (which it closes with itself), except the one fs_fail_at picked: the
// k-th store call from now (on any thread) answers `rc` instead and is not
// forwarded. A sweep fails k = 0, 1, 2, ... until a run makes no failure
// (fs_failed() false), the way unit_oom.c sweeps allocations.
//
// Its ops->name is "fault", so code that asks which backend it has
// (the console's SQL, the import's counts) sees neither.
#ifndef CB_LIB_FAULTSTORE_H
#define CB_LIB_FAULTSTORE_H

#include "store/store.h"

#include <stdbool.h>

cb_store *fs_wrap(cb_store *inner);
void fs_fail_at(long k, int rc);
void fs_off(void);
long fs_calls(void);     // store calls since fs_fail_at
bool fs_failed(void);    // the chosen call has failed

#endif
