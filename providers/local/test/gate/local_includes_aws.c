// local_includes_aws.c - one provider may not include another's header.
// c_base/Makefile's compiler-gate compiles this file in the flags a source of
// providers/local/src gets, and it must fail on this include; with every root on the path
// it must compile, so the header is real. docs/PLATFORM.md 8.4, the compiler gate.
#include "aws/sigv4.h"
