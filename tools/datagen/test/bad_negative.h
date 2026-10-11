// A negative number in the brackets. Refused by the compiler, like
// bad_past_end.h, and kept apart from bad.h for the same reason.
static const char *const BAD_NEGATIVE[2] = { [-1] = "a" };
