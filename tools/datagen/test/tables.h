// datagen's genericity fixtures. Nothing here is the string table: these exist
// to hold the tool to the general case - "a C static table of literals" - so
// that the day a second caller wants an integer lookup table or a plain ordered
// list, the tool already does it and this file already proves it.
//
// Each table below is a case the tool must get right:
//
//   DG_GRID     two dimensions, both designated, ROWS WRITTEN OUT OF ORDER and
//               cells written out of order inside them. This is the case that
//               catches a positional reader: a visitor that counts children
//               instead of reading designators produces a complete, plausible,
//               wrong table here and nowhere else.
//   DG_HOLES    a row that skips slots, and a row nobody wrote at all. Neither
//               may become an empty string; both are left out.
//   DG_ORDINAL  no designators anywhere - C's own "the next slot" rule. An
//               ordered lookup table is a table too.
//   DG_SCORE    integers, not strings, and one written as a constant expression
//               rather than a literal token, because the values come from
//               clang's evaluator and not from the source text.
//   DG_MIXED    UTF-8 and right-to-left, plus every character the emitters have
//               to escape. If a byte survives C -> TS -> Swift -> Kotlin it
//               survives here. The dollar is not decoration: it opens a string
//               template in Kotlin and in no other target, so a price in a
//               translated string is a compile error in twenty-five generated
//               files at once unless the Kotlin emitter escapes it.
#ifndef DATAGEN_TEST_TABLES_H
#define DATAGEN_TEST_TABLES_H

typedef enum { DG_R0, DG_R1, DG_R2, DG_NROW } DgRow;
typedef enum { DG_C0, DG_C1, DG_C2, DG_NCOL } DgCol;

static const char *const DG_ROW_NAME[DG_NROW] = { [DG_R2] = "third", [DG_R0] = "first", [DG_R1] = "second" };
static const char *const DG_COL_NAME[DG_NCOL] = { [DG_C0] = "a", [DG_C1] = "b", [DG_C2] = "c" };

static const char *const DG_GRID[DG_NROW][DG_NCOL] = {
    [DG_R2] = { [DG_C1] = "r2c1", [DG_C0] = "r2c0", [DG_C2] = "r2c2" },
    [DG_R0] = { [DG_C2] = "r0c2", [DG_C0] = "r0c0", [DG_C1] = "r0c1" },
    [DG_R1] = { [DG_C0] = "r1c0", [DG_C2] = "r1c2", [DG_C1] = "r1c1" },
};

// R1 is never mentioned; R0 skips C1; R2 writes an explicit NULL.
static const char *const DG_HOLES[DG_NROW][DG_NCOL] = {
    [DG_R0] = { [DG_C0] = "kept", [DG_C2] = "also kept" },
    [DG_R2] = { [DG_C0] = 0, [DG_C1] = "survivor" },
};

static const char *const DG_ORDINAL[DG_NCOL] = { "zero", "one", "two" };

#define DG_BONUS 7
static const int DG_SCORE[DG_NCOL] = { [DG_C1] = DG_BONUS * 2, [DG_C0] = -1, [DG_C2] = 0 };

static const char *const DG_MIXED[DG_NCOL] = {
    [DG_C0] = "Бито / 좋아 / \"quoted\" / back\\slash / $1.99",
    [DG_C1] = "קח לעצמך",          // Hebrew, right to left
    [DG_C2] = "التقاط\tand a tab", // Arabic, plus an escape the emitters rewrite
};

// A companion table that says, per slot, whether a value is REQUIRED there. Two
// tiers: a slot marked 1 must be filled and a slot marked 0 may be a hole. This
// is the shape c/i18n/keys.h uses to tell "nobody has translated this yet" from
// "this key does not exist", which are not the same mistake.
typedef struct {
    const char *name;
    int         everywhere;
} DgSlotInfo;

static const DgSlotInfo DG_SLOTS[DG_NCOL] = {
    [DG_C0] = { "a", 1 },
    [DG_C1] = { "b", 1 },
    [DG_C2] = { "c", 0 },   // optional: a hole here is allowed
};

// Fills the two required slots and leaves the optional one empty.
static const char *const DG_PARTIAL[DG_NCOL] = { [DG_C0] = "have a", [DG_C1] = "have b" };

// Two rows where the table it is asked about has three.
typedef struct { const char *name; int flag; } DgPair;
static const DgPair DG_PAIR[2] = { { "x", 1 }, { "y", 1 } };

// Leaves a REQUIRED slot empty, which must fail.
static const char *const DG_SHORT[DG_NCOL] = { [DG_C0] = "have a", [DG_C2] = "have c" };

#endif
