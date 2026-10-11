// A number past the end of the array. The compiler refuses this one itself, so
// it lives in a file of its own: bad.h has to compile.
static const char *const BAD_PAST_END[2] = { [0] = "a", [2] = "c" };
