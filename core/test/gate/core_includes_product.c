// core_includes_product.c - core may not include the product's header.
// c_base/Makefile's compiler-gate compiles this file in the flags a source of
// core/src gets, and it must fail on this include; with every root on the path
// it must compile, so the header is real. docs/PLATFORM.md 8.4, the compiler gate.
#include "store/rows_foolish.h"
