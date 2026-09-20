// The model: what the headers say, as this tool holds it. One Rec per type the
// roots reach and one Field per member it emits, with the caller's requests
// over them - which fields are wanted (--fields), which arrays are counted
// (--count), which records are copied (--snapshot) and written (--writer).
//
// The traversal (sg_clang.c) is the only thing that fills it; every emitter
// only reads it. An offset, a size or a stride in here came from clang and from
// nothing else.
#ifndef SG_MODEL_H
#define SG_MODEL_H
#include <clang-c/Index.h>
#include "structgen.h"

// ---- --fields requests -------------------------------------------------------
typedef struct { char *type; char *names[256]; char seen[256]; int n, used; } Spec;
extern Spec specs[MAXN];
extern int nspecs;
Spec *spec_for(const char *type);
int spec_has(Spec *s, const char *name);

// ---- --count requests --------------------------------------------------------
typedef struct { char *type, *field, *count; int used; } Count;
extern Count counts[MAXN];
extern int ncounts;
Count *count_for(const char *rec, const char *field);
int is_count_field(const char *rec, const char *field);

// ---- the model -------------------------------------------------------------
typedef struct {
    char *name;
    long off;            // bytes
    int lo, width;       // bitfield: first bit within byte `off`, bit count; width 0 = not a bitfield
    char kind;           // bitfield only
    int nd; long dims[8];
    int type;            // index into recs (not a bitfield)
} Field;
typedef struct {
    char *key, *name, *expr;
    long size;
    char kind;           // scalars: i u b f, p = a pointer (records: 0)
    int pointee;         // a pointer: the record or scalar it points to, -1 if nothing to follow (void, a function, ...)
    int pointee_done;    // a pointer: pointee resolved
    CXType ptr_decl;     // a pointer: the declared type of the first field that reached it,
    int ptr_nd;          //   its array levels,
    char *ptr_rec, *ptr_field, *ptr_expr;   //   and that field's record, name and C expression (NULL: no field did)
    int record, charlike, is_union;
    int snap;            // --snapshot: 1 once reached from a snapshot root
    int writer;          // --writer: 1 once reached from a writer root
    CXType t;
    Field *f; int nf, capf;
} Rec;
extern Rec *recs;
extern int nrecs, caprecs;
extern int root_rec[MAXN];   // --root i -> its record

Field *field_named(Rec *r, const char *name);
// Marks `ri` and every record its fields reach as snapshotted / as written.
void snap_mark(int ri);
void writer_mark(int ri);

// ---- constants ----------------------------------------------------------------
typedef struct { char *name; int prefix; long long value; int macro; } Const;
extern Const *consts;
extern int nconsts, capconsts;
void add_const(const char *name, int macro, long long value);

// The model against the request: every --fields, --count, --snapshot and
// --writer must name something the roots reach, and every refusal a request
// earns is made here rather than inside an emitter.
void sg_check(void);

#endif
