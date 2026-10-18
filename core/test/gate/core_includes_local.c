// core_includes_local.c - core may not include a provider's header (local's).
// c_base/Makefile's compiler-gate compiles this file in the flags a source of
// core/src gets, and it must fail on this include; with every root on the path
// it must compile, so the header is real. docs/PLATFORM.md 8.4, the compiler gate.
#include "realtime/rt_local.h"
