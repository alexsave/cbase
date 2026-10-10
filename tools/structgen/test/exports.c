// The fixture of test/exports.sh: one function per thing an exports run has to
// get right about a C definition, each named for it. EX_DRIFT builds the same
// file with one signature changed, which is what a module stale against its
// own source looks like.
#include <stdbool.h>
#include <stdint.h>

typedef enum { EX_KIND_A, EX_KIND_B } ExKind;

static unsigned char g_buf[64];

// Internal linkage: nothing a linker could export, whatever a link line says.
static int ex_helper(int x) { return x + 1; }
// External, and no link names it: not in any interface.
int ex_unlisted(int x) { return ex_helper(x); }

// A pointer result is an address, which is an i32.
unsigned char *ex_buf_ptr(void) { return g_buf; }

// Parameter names are the definition's, in camelCase.
#ifdef EX_DRIFT
int ex_add(double left_side, unsigned right_side) { return (int)left_side + (int)right_side; }
#else
int ex_add(int left_side, unsigned right_side) { return left_side + (int)right_side; }
#endif

// f64 and f32 are both a `number`, and the line's comment tells them apart.
double ex_scale(double value, float by) { return value * (double)by; }

// A 64-bit integer is an i64, which JS passes as a bigint.
int64_t ex_wide(int64_t big, uint64_t bigger, int small) { return big + (int64_t)bigger + small; }

void ex_reset(void) { g_buf[0] = 0; }

// bool, an enum, a pointer and an array parameter are each one i32.
bool ex_flag(bool on, ExKind kind, const char *name, uint8_t bytes[4]) { return on && kind == EX_KIND_B && name && bytes; }

// `in` and `new` are names C allows and TypeScript reserves.
int ex_words(int in, int new) { return in + new; }

// The module calls this one ex_renamed, and exports it listed or not.
__attribute__((export_name("ex_renamed")))
int ex_internal_name(int a) { return a; }
