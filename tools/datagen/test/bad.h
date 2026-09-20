// Tables datagen must REFUSE, one per failure. Each of these compiles: C is
// happy with every one of them, which is the point - the tool's job is to catch
// what the language shrugs at, because a data table's mistakes are silent.
#ifndef DATAGEN_TEST_BAD_H
#define DATAGEN_TEST_BAD_H

typedef enum { BAD_A, BAD_B, BAD_C, BAD_N } BadIdx;

// C keeps the LAST initializer for a slot and says nothing. In a translation
// table that is a typo that deletes a string.
static const char *const BAD_TWICE[BAD_N] = { [BAD_A] = "first", [BAD_B] = "b", [BAD_A] = "second" };

// One name short: a row nobody can name cannot be emitted, and dropping it
// silently is how a table loses a language.
static const char *const BAD_SHORT_NAMES[BAD_N] = { [BAD_A] = "a", [BAD_C] = "c" };

// Two slots, one name. Whichever went out second would win in the emitted
// object, and the other's value would be gone.
static const char *const BAD_DUP_NAMES[BAD_N] = { [BAD_A] = "x", [BAD_B] = "x", [BAD_C] = "c" };

// Neither a string nor an integer constant: datagen reads tables of literals,
// and an address is not one.
static const char BAD_TARGET[] = "t";
static const char *const BAD_ADDR[BAD_N] = { [BAD_A] = BAD_TARGET, [BAD_B] = "b", [BAD_C] = "c" };

// Not an array at all.
static const char *const BAD_SCALAR = "not a table";

// A clean three-slot table, so the label failures below fail for the reason
// they are named after and not because the table itself is broken.
static const char *const BAD_OK[BAD_N] = { [BAD_A] = "a", [BAD_B] = "b", [BAD_C] = "c" };

// Two names for a three-slot table.
static const char *const BAD_TWO_NAMES[2] = { "one", "two" };

#endif
