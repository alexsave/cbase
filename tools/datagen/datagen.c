// datagen - a C static table of literals -> the same table as TypeScript, as
// Swift and as Kotlin. The sibling of tools/structgen, on the same libclang.
//
// THE TWO TOOLS ASK CLANG DIFFERENT QUESTIONS, which is why they are two
// programs. structgen asks for SHAPE: every offset, size, bitfield position and
// array stride, and it never reads a value. datagen asks for CONTENTS: what the
// initializer of a file-scope `static const` array actually says, and it never
// reads a layout. Neither one can answer the other's question, and running them
// together would mean a layout hash over data that has no layout.
//
//   datagen --cwd DIR --header H... --table T [--labels T.DIM=L]...
//           [--require-complete] [--flags FLAGS] [--target TRIPLE] [--name IDENT]
//           [--ts OUT] [--swift OUT] [--kotlin OUT --kotlin-package PKG] [--json OUT]
//
// --table T       the table to read: a file-scope `static const` array of one or
//                 two dimensions, or a one-dimensional array of STRUCTS, whose
//                 cells are string literals or integer constant expressions. The
//                 values come from clang's own evaluator (clang_Cursor_Evaluate),
//                 so a cell may be any constant expression C accepts - a macro,
//                 an enum constant, arithmetic, two adjacent string literals -
//                 and not just a literal token.
//                 An array of structs uses the struct's FIELDS as its second
//                 dimension, named by the record, each with its own C type; the
//                 emitted row is an interface in TS and a struct in Swift.
// --labels T.D=L  table L (a one-dimensional table of strings, read the same
//                 way) names dimension D's indices, so the emitted table is
//                 keyed by those strings instead of by position. Without it the
//                 dimension is emitted as an array. A struct's columns are
//                 already named, so --labels T.1 is refused for one.
// --require-complete
//                 every slot of the index space must be filled, and a gap is a
//                 build failure naming each one. This is what stands in for a
//                 compile error once a table is SPLIT across files: c/i18n holds
//                 one file per language, and nothing in C makes twenty-five
//                 independent tables carry the same keys.
// --name IDENT    what the emitted modules export. Defaults to the table's name.
// --ts-const      emit the TS table as `as const` rather than with a declared
//                 type, so its keys and values stay literal types. A host can
//                 then derive a union of the real key names from the table
//                 itself instead of keeping a second list that goes stale.
// --ts / --swift / --kotlin
//                 where to write. --kotlin needs --kotlin-package, because a
//                 Kotlin file declares the package it is in and a wrong one is a
//                 wrong import at every call site. --json writes the same data as
//                 JSON, which is what a test reads to check the extraction
//                 without compiling any of the three.
// --flags FLAGS   extra clang flags (-D, -I, -std=...), one string, as structgen
//                 takes them from a build.
//
// ---- the designator is the key, and that is the whole safety argument -------
//
// A row is written `[LANG_RU] = { [K_GOOD] = "Бито", ... }`, and a struct's cell
// `.knobs = "..."`. Either way the designator is the NAME of the slot, and
// datagen resolves it to an index - an enumerator's value, or a field's
// position in its record - so a table is read by name in both dimensions. Positional entries are still
// accepted with C's own rule (the next index after the last one written), which
// is what makes the tool general - an ordered lookup table needs no designators
// at all - but a table that designates is a table that cannot be silently
// mis-keyed.
//
// That failure is not hypothetical. The first visitor written against this AST
// ignored designators and counted children instead; every row landed in row 0
// and the last one won. Nothing crashed, nothing warned, and the output was a
// complete, plausible, WRONG table - twenty-four languages quietly replaced by
// the twenty-fifth. tools/datagen/test/cli.sh pins that case: a table written
// out of designator order must come back in index order.
//
// TWO INITIALIZERS FOR ONE SLOT ARE REFUSED. C says the later one wins; in a
// data table it is a typo that deletes a translation, so datagen names the slot
// and exits rather than picking a winner.
//
// A SLOT NOBODY WROTE IS A HOLE, not an empty string: it is left out of the
// emitted object entirely, so a reader's own fallback (the web's and the app's
// both fall back to English) decides what to do about it. A table whose rows
// are not all the same length is the normal case here - the phone app renders
// strings the website has no screen for, and the other way round.
//
// ---- determinism -----------------------------------------------------------
//
// Output is a function of the tree and of nothing else: indices are emitted in
// ascending order, never in the order the initializer happens to list them, and
// no hash table or address ever reaches the output. Two runs over one tree write
// the same bytes on every platform (tools/structgen/gen.sh --check covers this
// tool's outputs too, since it writes them).
//
// die, Buf/bprintf, xstrdup, absolute and the probe translation unit itself are
// in tools/sgcommon, shared with structgen: the two tools ask libclang
// different questions, but they ask them the same way.
#include <clang-c/Index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sgc.h"
#include "sgc_probe.h"

#define MAXN 64
#define MAXDIM 2

static const char *headers[MAXN], *table_name, *out_ts, *out_swift, *out_kotlin, *out_json, *export_name;
static const char *kotlin_package;
static const char *cwd = ".", *target, *flags = "";
static int require_complete, ts_const;
static const char *complete_if;   // --require-complete-if TABLE.COLUMN
static int nheaders;
static const char *label_table[MAXDIM];   // --labels T.D=L, by dimension

// ---- the extracted table ---------------------------------------------------
//
// One flat array of `dim[0] * dim[1]` cells (dim[1] is 1 for a 1-D table), so a
// slot is addressed by its indices and the emitter walks them in order.
typedef struct { char kind; char *s; long long i; } Cell;   // kind: 0 hole, 's', 'i'
#define MAXCOL 64
typedef struct {
    const char *name;
    int ndim;              // array dimensions; a struct's fields become dim 1
    long dim[MAXDIM];
    int is_struct;         // dim 1 is a record's fields, named by the record
    const char *rec_name;  // that record's name, for the emitted row type
    char *field[MAXCOL];   // its field names, in declaration order
    char colptr[MAXCOL];   // per column: is this column's C type a pointer?
    int nfield;
    Cell *cells;
} Table;

// One field of a struct element: its name and whether it is a pointer (so a 0
// in that column is NULL rather than the number zero).
static enum CXVisitorResult collect_field(CXCursor c, CXClientData d);

static long cols_of(Table *t) { return t->ndim == 2 ? t->dim[1] : 1; }
static Cell *cell_at(Table *t, long a, long b) { return &t->cells[a * cols_of(t) + b]; }

// ---- reading one initializer ------------------------------------------------
//
// The AST of `static const char *const T[R][C] = { [A] = { [B] = "x" } }`:
//
//   VarDecl T
//     DeclRefExpr R           <- the array bounds, if they are named constants
//     DeclRefExpr C
//     InitListExpr            <- the table
//       UnexposedExpr         <- one designated row
//         DeclRefExpr A       <- the designator
//         InitListExpr        <- the row
//           UnexposedExpr     <- one designated cell
//             DeclRefExpr B
//             UnexposedExpr   <- the value (an implicit cast over the literal)
//
// An undesignated entry is the value (or the row's InitListExpr) directly, with
// no UnexposedExpr wrapper carrying a DeclRefExpr.

typedef struct {
    Table *t;
    int dim;          // which dimension this list is indexing
    long row;         // for dim 1: the row already chosen
    long next;        // C's rule: an entry with no designator takes the next slot
    int *seen;        // one flag per slot of this list, to refuse a second write
} ListCtx;

// A designator names a slot two ways, and datagen reads both: `[ENUM] =` in an
// array, and `.field =` in a struct. Either one is the slot's NAME, which is the
// whole point - a named slot cannot be silently mis-placed.
typedef struct { long idx; CXCursor value; int found_designator, nkids; Table *t; int dim; } Split;

static long field_index(Table *t, const char *name) {
    if (!t->is_struct) return -1;
    for (long i = 0; i < t->dim[1]; i++) if (!strcmp(t->field[i], name)) return i;
    return -1;
}

static long designator_index(Split *s, CXCursor c) {
    enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_MemberRef) {           // .field = value, inside a struct row
        if (s->dim != 1 || !s->t->is_struct) return -1;
        char *n = str(clang_getCursorSpelling(c));
        long i = field_index(s->t, n);
        if (i < 0) die("%s has no field named %s", s->t->rec_name, n);
        free(n);
        return i;
    }
    if (k != CXCursor_DeclRefExpr) return -1;
    CXCursor d = clang_getCursorReferenced(c);
    if (clang_getCursorKind(d) != CXCursor_EnumConstantDecl) return -1;
    // An enum constant is also a legal VALUE for an int cell (STRAT_CORDITE in
    // the bot roster). Only dimension-0/1 array slots take one as a designator,
    // and clang puts a designator first among the entry's children - which is
    // exactly the position tested here.
    if (s->t->is_struct && s->dim == 1) return -1;
    return (long)clang_getEnumConstantDeclValue(d);
}

static enum CXChildVisitResult split_visit(CXCursor c, CXCursor p, CXClientData d) {
    (void)p; Split *s = d;
    if (s->nkids == 0) {
        long i = designator_index(s, c);
        if (i >= 0) { s->idx = i; s->found_designator = 1; s->nkids++; return CXChildVisit_Continue; }
    }
    if (s->found_designator && s->nkids == 1) { s->value = c; s->nkids++; return CXChildVisit_Continue; }
    s->nkids++;
    return CXChildVisit_Continue;
}

static void read_list(CXCursor list, ListCtx *ctx);

// A pointer cell written as an explicit NULL. clang's evaluator declines a null
// pointer constant outright - it hands back no result at all rather than the
// integer 0 - so the one way to tell "NULL" from "clang could not read this" is
// to go and look for the literal zero underneath the implicit cast.
static enum CXChildVisitResult null_probe(CXCursor c, CXCursor p, CXClientData d) {
    (void)p; int *is_null = d;
    if (clang_getCursorKind(c) != CXCursor_IntegerLiteral) return CXChildVisit_Recurse;
    CXEvalResult r = clang_Cursor_Evaluate(c);
    if (r) {
        if (clang_EvalResult_getKind(r) == CXEval_Int && clang_EvalResult_getAsLongLong(r) == 0) *is_null = 1;
        clang_EvalResult_dispose(r);
    }
    return CXChildVisit_Break;
}

static void store(Table *t, long a, long b, CXCursor value, const char *where) {
    Cell *cell = cell_at(t, a, b);
    int leaf_ptr = t->colptr[t->ndim == 2 ? b : 0];
    CXEvalResult r = clang_Cursor_Evaluate(value);
    if (!r) {
        int is_null = 0;
        if (leaf_ptr) clang_visitChildren(value, null_probe, &is_null);
        if (!is_null)
            die("%s: this initializer is neither a string literal nor an integer constant "
                "- datagen reads tables of literals, and clang's evaluator declined this one", where);
        cell->kind = 0;   // a written NULL is a hole, exactly like an unwritten slot
        return;
    }
    switch (clang_EvalResult_getKind(r)) {
    case CXEval_StrLiteral: {
        const char *s = clang_EvalResult_getAsStr(r);
        if (!s) die("%s: clang read a string literal it will not hand back", where);
        cell->kind = 's'; cell->s = xstrdup(s);
        break;
    }
    case CXEval_Int:
        // In a table of pointers the only integer C accepts is 0, and a written
        // NULL says "no value here" as loudly as leaving the slot out does.
        if (leaf_ptr) {
            if (clang_EvalResult_getAsLongLong(r) != 0)
                die("%s: a pointer cell initialized with a nonzero integer", where);
            cell->kind = 0;
        } else {
            cell->kind = 'i'; cell->i = clang_EvalResult_getAsLongLong(r);
        }
        break;
    default:
        die("%s: this initializer is neither a string literal nor an integer constant "
            "- datagen reads tables of literals", where);
    }
    clang_EvalResult_dispose(r);
}

static enum CXChildVisitResult entry_visit(CXCursor c, CXCursor p, CXClientData d) {
    (void)p; ListCtx *ctx = d;
    Table *t = ctx->t;
    long n = t->dim[ctx->dim];

    Split s = { -1, clang_getNullCursor(), 0, 0, t, ctx->dim };
    CXCursor value = c;
    if (clang_getCursorKind(c) == CXCursor_UnexposedExpr) {
        clang_visitChildren(c, split_visit, &s);
        if (s.found_designator) value = s.value;
    }
    long idx = s.found_designator ? s.idx : ctx->next;
    ctx->next = idx + 1;
    if (idx < 0 || idx >= n)
        die("%s: initializer for index %ld is outside the array's %ld slot(s)", t->name, idx, n);
    if (ctx->seen[idx])
        die("%s: index %ld is initialized twice - C would silently keep the last one, "
            "which in a data table is a typo that deletes a value", t->name, idx);
    ctx->seen[idx] = 1;

    if (ctx->dim == 0 && t->ndim == 2) {
        if (clang_getCursorKind(value) != CXCursor_InitListExpr)
            die("%s: row %ld is not a braced list", t->name, idx);
        int *seen = calloc((size_t)t->dim[1], sizeof *seen);
        if (!seen) die("out of memory");
        ListCtx inner = { t, 1, idx, 0, seen };
        clang_visitChildren(value, entry_visit, &inner);
        free(seen);
    } else {
        char where[256];
        snprintf(where, sizeof where, "%s[%ld]%s", t->name, ctx->dim == 1 ? ctx->row : idx,
                 ctx->dim == 1 ? "[..]" : "");
        store(t, ctx->dim == 1 ? ctx->row : idx, ctx->dim == 1 ? idx : 0, value, where);
    }
    return CXChildVisit_Continue;
}

static void read_list(CXCursor list, ListCtx *ctx) { clang_visitChildren(list, entry_visit, ctx); }

static enum CXVisitorResult collect_field(CXCursor c, CXClientData d) {
    Table *t = d;
    if (t->nfield == MAXCOL) die("%s has more than %d fields", t->rec_name, MAXCOL);
    if (clang_Cursor_isBitField(c)) die("%s: a bitfield is a layout question, and datagen reads contents", t->rec_name);
    t->field[t->nfield] = str(clang_getCursorSpelling(c));
    CXType ft = clang_getCanonicalType(clang_getCursorType(c));
    if (ft.kind != CXType_Pointer && ft.kind != CXType_Enum && !(ft.kind >= CXType_Bool && ft.kind <= CXType_LongDouble))
        die("%s.%s is a %s; datagen reads tables of literals", t->rec_name, t->field[t->nfield],
            clang_getCString(clang_getTypeSpelling(ft)));
    t->colptr[t->nfield] = ft.kind == CXType_Pointer;
    t->nfield++;
    return CXVisit_Continue;
}

// ---- finding the table ------------------------------------------------------
typedef struct { const char *want; Table *out; int found; } Find;

static enum CXChildVisitResult find_init(CXCursor c, CXCursor p, CXClientData d) {
    (void)p; Find *f = d;
    if (clang_getCursorKind(c) != CXCursor_InitListExpr) return CXChildVisit_Continue;
    int *seen = calloc((size_t)f->out->dim[0], sizeof *seen);
    if (!seen) die("out of memory");
    ListCtx ctx = { f->out, 0, 0, 0, seen };
    read_list(c, &ctx);
    free(seen);
    return CXChildVisit_Break;
}

static enum CXChildVisitResult find_var(CXCursor c, CXCursor p, CXClientData d) {
    (void)p; Find *f = d;
    if (clang_getCursorKind(c) != CXCursor_VarDecl) return CXChildVisit_Recurse;
    char *name = str(clang_getCursorSpelling(c));
    int hit = !strcmp(name, f->want);
    free(name);
    if (!hit) return CXChildVisit_Recurse;
    if (f->found) die("%s is declared more than once", f->want);
    f->found = 1;

    CXType ty = clang_getCanonicalType(clang_getCursorType(c));
    Table *t = f->out;
    t->name = f->want;
    t->ndim = 0;
    while (ty.kind == CXType_ConstantArray) {
        if (t->ndim == MAXDIM) die("%s has more than %d dimensions", f->want, MAXDIM);
        t->dim[t->ndim++] = (long)clang_getArraySize(ty);
        ty = clang_getCanonicalType(clang_getArrayElementType(ty));
    }
    if (!t->ndim) die("%s is not an array - datagen reads tables", f->want);
    if (ty.kind == CXType_Record) {
        // An array of structs: the struct's FIELDS are the second dimension,
        // and the record names them, so no --labels table is needed or allowed.
        if (t->ndim != 1) die("%s is an array of arrays of structs; datagen reads one dimension of structs", f->want);
        CXCursor rd = clang_getTypeDeclaration(ty);
        t->rec_name = str(clang_getCursorSpelling(rd));
        clang_Type_visitFields(ty, collect_field, t);
        if (!t->nfield) die("%s's element type %s has no fields", f->want, t->rec_name);
        t->is_struct = 1;
        t->ndim = 2;
        t->dim[1] = t->nfield;
    } else {
        for (int i = 0; i < (t->ndim == 2 ? (int)t->dim[1] : 1); i++) t->colptr[i] = ty.kind == CXType_Pointer;
        if (t->ndim == 1) t->colptr[0] = ty.kind == CXType_Pointer;
    }
    for (int i = 0; i < t->ndim; i++)
        if (t->dim[i] <= 0) die("%s has a dimension of size %ld: give the array an explicit bound", f->want, t->dim[i]);
    size_t n = (size_t)t->dim[0] * (size_t)(t->ndim == 2 ? t->dim[1] : 1);
    if (!(t->cells = calloc(n, sizeof *t->cells))) die("out of memory");
    clang_visitChildren(c, find_init, f);
    return CXChildVisit_Continue;
}

static void read_table(CXTranslationUnit tu, const char *name, Table *out) {
    Find f = { name, out, 0 };
    clang_visitChildren(clang_getTranslationUnitCursor(tu), find_var, &f);
    if (!f.found) die("no table named %s in the given headers", name);
}

// ---- labels ------------------------------------------------------------------
//
// A dimension's labels are themselves a one-dimensional table of strings, read
// by the same code. A label table with a hole is refused: a row nobody can name
// cannot be emitted, and silently dropping it is how a table loses a language.
typedef struct { char **name; long n; } Labels;

// A spec is either a whole table (`NAMES`) or one column of a table of structs
// (`KEYS.name`). One reader for both, so a companion table can answer with any
// of its columns and neither the caller nor this file cares which shape it is.
static void read_column(CXTranslationUnit tu, const char *spec, Table *t, long *col) {
    char *name = xstrdup(spec), *dot = strrchr(name, '.');
    *col = 0;
    if (dot) *dot = 0;
    read_table(tu, name, t);
    if (!dot) {
        if (t->ndim != 1) die("%s is not one-dimensional: name one of its columns as %s.FIELD", name, name);
        return;
    }
    if (!t->is_struct) die("%s is not a table of structs, so it has no column %s", name, dot + 1);
    for (long i = 0; i < t->nfield; i++) if (!strcmp(t->field[i], dot + 1)) { *col = i; return; }
    die("%s has no column named %s", name, dot + 1);
}

static void read_labels(CXTranslationUnit tu, const char *tbl, long want, Labels *out) {
    Table t = {0};
    long col;
    read_column(tu, tbl, &t, &col);
    if (t.is_struct) {
        // One column of a struct table, lifted out as the one-dimensional table
        // of names the rest of this function already knows how to check.
        Table flat = { t.name, 1, { t.dim[0], 0 }, 0, NULL, {0}, {0}, 0, NULL };
        if (!(flat.cells = calloc((size_t)t.dim[0], sizeof *flat.cells))) die("out of memory");
        for (long i = 0; i < t.dim[0]; i++) flat.cells[i] = *cell_at(&t, i, col);
        t = flat;
    }
    if (t.ndim != 1) die("--labels table %s must be one-dimensional", tbl);
    if (t.dim[0] != want)
        die("--labels table %s holds %ld name(s) for a dimension of %ld", tbl, t.dim[0], want);
    out->n = t.dim[0];
    if (!(out->name = calloc((size_t)out->n, sizeof *out->name))) die("out of memory");
    for (long i = 0; i < out->n; i++) {
        Cell *c = cell_at(&t, i, 0);
        if (c->kind != 's') die("--labels table %s has no name for index %ld", tbl, i);
        out->name[i] = c->s;
    }
    for (long i = 0; i < out->n; i++)
        for (long j = i + 1; j < out->n; j++)
            if (!strcmp(out->name[i], out->name[j]))
                die("--labels table %s names both index %ld and index %ld \"%s\"", tbl, i, j, out->name[i]);
}

// ---- emission ----------------------------------------------------------------
//
// One escaper for all three languages. TypeScript, Swift and JSON agree on
// \\, \" and the \uXXXX form for a control character, and all three read UTF-8
// source, so a non-ASCII byte goes out as itself - Cyrillic, Korean, Hebrew and
// Arabic stay readable in the generated file instead of becoming escape soup.
// Swift's one extra rule is \( , which opens an interpolation: escaping the
// backslash already covers it, since a lone ( is not special.
static void quoted(Buf *b, const char *s) {
    bprintf(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') bprintf(b, "\\%c", *p);
        else if (*p == '\n') bprintf(b, "\\n");
        else if (*p == '\t') bprintf(b, "\\t");
        else if (*p == '\r') bprintf(b, "\\r");
        else if (*p < 0x20 || *p == 0x7f) bprintf(b, "\\u%04x", *p);
        else bprintf(b, "%c", *p);
    }
    bprintf(b, "\"");
}

// Swift spells a control escape \u{XXXX}; everything else is the same text.
static void quoted_swift(Buf *b, const char *s) {
    Buf tmp = {0};
    quoted(&tmp, s);
    for (size_t i = 0; i < tmp.n; i++) {
        if (tmp.s[i] == '\\' && i + 1 < tmp.n && tmp.s[i + 1] == 'u') {
            bprintf(b, "\\u{%.4s}", tmp.s + i + 2);
            i += 5;
        } else bprintf(b, "%c", tmp.s[i]);
    }
    free(tmp.s);
}

// Kotlin's one extra rule is $, which opens a string template: a lone one in a
// translated price ("$1.99") would be read as a reference to an identifier that
// does not exist, which is a compile error in twenty-five generated files at
// once. Everything else is the same text, \uXXXX included.
static void quoted_kotlin(Buf *b, const char *s) {
    Buf tmp = {0};
    quoted(&tmp, s);
    for (size_t i = 0; i < tmp.n; i++) {
        if (tmp.s[i] == '$') bprintf(b, "\\$");
        else bprintf(b, "%c", tmp.s[i]);
    }
    free(tmp.s);
}

typedef void (*Quote)(Buf *, const char *);

static void cell_literal(Buf *b, Cell *c, Quote q) {
    if (c->kind == 'i') bprintf(b, "%lld", c->i);
    else q(b, c->s);
}

// The body shared by all three emitters: an object keyed by labels, or an array.
// `open`/`close` are the language's brackets, `colon` its key separator.
typedef struct { const char *obj_open, *obj_close, *arr_open, *arr_close, *colon, *empty_obj; Quote q; } Syntax;

static void emit_dim(Buf *b, Table *t, Labels *lab, int dim, long row, const Syntax *sx, const char *indent) {
    Labels *l = &lab[dim];
    int keyed = l->name != NULL;
    long n = t->dim[dim];
    bprintf(b, "%s\n", keyed ? sx->obj_open : sx->arr_open);
    int wrote = 0;
    for (long i = 0; i < n; i++) {
        if (dim == 0 && t->ndim == 2) {
            // A row is emitted unless every cell of it is a hole.
            int any = 0;
            for (long j = 0; j < t->dim[1]; j++) if (cell_at(t, i, j)->kind) { any = 1; break; }
            if (!any) continue;
            bprintf(b, "%s    ", indent);
            if (keyed) { sx->q(b, l->name[i]); bprintf(b, "%s ", sx->colon); }
            char deeper[64];
            snprintf(deeper, sizeof deeper, "%s    ", indent);
            emit_dim(b, t, lab, 1, i, sx, deeper);
            bprintf(b, ",\n");
            wrote = 1;
        } else {
            Cell *c = cell_at(t, t->ndim == 2 ? row : i, t->ndim == 2 ? i : 0);
            if (!c->kind) continue;
            bprintf(b, "%s    ", indent);
            if (keyed) { sx->q(b, l->name[i]); bprintf(b, "%s ", sx->colon); }
            cell_literal(b, c, sx->q);
            bprintf(b, ",\n");
            wrote = 1;
        }
    }
    // Swift cannot read an empty dictionary as [:] spelled [], and TS/JSON
    // cannot read a trailing comma in an empty object either.
    if (!wrote && keyed) { bprintf(b, "%s%s", indent, sx->empty_obj); return; }
    bprintf(b, "%s%s", indent, keyed ? sx->obj_close : sx->arr_close);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void banner(Buf *b, const char *comment) {
    bprintf(b, "%s GENERATED by tools/datagen - do not edit.\n", comment);
    bprintf(b, "%s source: %s in", comment, table_name);
    for (int i = 0; i < nheaders; i++) bprintf(b, " %s", base_name(headers[i]));
    bprintf(b, "\n");
}

static void write_out(const char *path, Buf *b) {
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s", path);
    if (b->n && fwrite(b->s, 1, b->n, fp) != b->n) die("short write to %s", path);
    fclose(fp);
}

static void emit_ts(Table *t, Labels *lab) {
    Buf b = {0};
    banner(&b, "//");
    if (t->is_struct) {   // one named row type, each field with its own C type
        bprintf(&b, "\nexport interface %sRow {\n", export_name);
        for (int i = 0; i < t->nfield; i++)
            bprintf(&b, "    readonly %s: %s;\n", t->field[i], t->colptr[i] ? "string" : "number");
        bprintf(&b, "}\n\nexport const %s: readonly %sRow[] = ", export_name, export_name);
        static const Syntax sxr = { "{", "}", "[", "]", ":", "{}", quoted };
        emit_dim(&b, t, lab, 0, 0, &sxr, "");
        bprintf(&b, ";\n");
        write_out(out_ts, &b);
        free(b.s);
        return;
    }
    // The type, spelled outward from the leaf.
    const char *leaf = "string";
    for (long i = 0; i < t->dim[0] * (t->ndim == 2 ? t->dim[1] : 1); i++)
        if (t->cells[i].kind == 'i') { leaf = "number"; break; }
    char ty[256];
    if (t->ndim == 1) snprintf(ty, sizeof ty, lab[0].name ? "Readonly<Record<string, %s>>" : "readonly %s[]", leaf);
    else {
        char inner[128];
        snprintf(inner, sizeof inner, lab[1].name ? "Readonly<Record<string, %s>>" : "readonly %s[]", leaf);
        snprintf(ty, sizeof ty, lab[0].name ? "Readonly<Record<string, %s>>" : "readonly %s[]", inner);
    }
    static const Syntax sx = { "{", "}", "[", "]", ":", "{}", quoted };
    // --ts-const: `as const` instead of a declared type, so TypeScript keeps
    // every key and value as a LITERAL. That is what lets a host derive a union
    // of the real key names from the emitted table (the website's StringId)
    // rather than restating them in a second place that can fall behind.
    if (ts_const) {
        bprintf(&b, "\nexport const %s = ", export_name);
        emit_dim(&b, t, lab, 0, 0, &sx, "");
        bprintf(&b, " as const;\n");
    } else {
        bprintf(&b, "\nexport type %sTable = %s;\n\nexport const %s: %sTable = ", export_name, ty, export_name, export_name);
        emit_dim(&b, t, lab, 0, 0, &sx, "");
        bprintf(&b, ";\n");
    }
    write_out(out_ts, &b);
    free(b.s);
}

static void emit_swift(Table *t, Labels *lab) {
    Buf b = {0};
    banner(&b, "//");
    if (t->is_struct) {
        bprintf(&b, "\npublic struct %sRow: Sendable, Equatable {\n", export_name);
        for (int i = 0; i < t->nfield; i++)
            bprintf(&b, "    public let %s: %s\n", t->field[i], t->colptr[i] ? "String" : "Int");
        bprintf(&b, "}\n\npublic let %s: [%sRow] = ", export_name, export_name);
        // A Swift struct literal is `Row(a: 1, b: "x")`, not a braced list, so
        // the row is written here rather than through the shared emitter.
        bprintf(&b, "[\n");
        for (long r = 0; r < t->dim[0]; r++) {
            bprintf(&b, "    %sRow(", export_name);
            for (int i = 0; i < t->nfield; i++) {
                Cell *c = cell_at(t, r, i);
                bprintf(&b, "%s%s: ", i ? ", " : "", t->field[i]);
                if (!c->kind) bprintf(&b, t->colptr[i] ? "\"\"" : "0");
                else cell_literal(&b, c, quoted_swift);
            }
            bprintf(&b, "),\n");
        }
        bprintf(&b, "]\n");
        write_out(out_swift, &b);
        free(b.s);
        return;
    }
    const char *leaf = "String";
    for (long i = 0; i < t->dim[0] * (t->ndim == 2 ? t->dim[1] : 1); i++)
        if (t->cells[i].kind == 'i') { leaf = "Int"; break; }
    char ty[256];
    if (t->ndim == 1) snprintf(ty, sizeof ty, lab[0].name ? "[String: %s]" : "[%s]", leaf);
    else {
        char inner[128];
        snprintf(inner, sizeof inner, lab[1].name ? "[String: %s]" : "[%s]", leaf);
        snprintf(ty, sizeof ty, lab[0].name ? "[String: %s]" : "[%s]", inner);
    }
    static const Syntax sx = { "[", "]", "[", "]", ":", "[:]", quoted_swift };
    bprintf(&b, "\npublic let %s: %s = ", export_name, ty);
    emit_dim(&b, t, lab, 0, 0, &sx, "");
    bprintf(&b, "\n");
    write_out(out_swift, &b);
    free(b.s);
}

// Kotlin has no braced collection literal: a list is listOf(...) and a map is
// mapOf(k to v, ...). That is a different SPELLING of the same two brackets and
// one separator the shared emitter already takes, so it goes through emit_dim
// like the other two rather than growing a second walker. Trailing commas inside
// a call are legal Kotlin (1.4).
static void emit_kotlin(Table *t, Labels *lab) {
    Buf b = {0};
    banner(&b, "//");
    bprintf(&b, "\npackage %s\n", kotlin_package);
    if (t->is_struct) {
        bprintf(&b, "\ndata class %sRow(\n", export_name);
        for (int i = 0; i < t->nfield; i++)
            bprintf(&b, "    val %s: %s,\n", t->field[i], t->colptr[i] ? "String" : "Int");
        bprintf(&b, ")\n\nval %s: List<%sRow> = listOf(\n", export_name, export_name);
        // A Kotlin row is a constructor call with named arguments, not a braced
        // list, so it is written here rather than through the shared emitter.
        for (long r = 0; r < t->dim[0]; r++) {
            bprintf(&b, "    %sRow(", export_name);
            for (int i = 0; i < t->nfield; i++) {
                Cell *c = cell_at(t, r, i);
                bprintf(&b, "%s%s = ", i ? ", " : "", t->field[i]);
                if (!c->kind) bprintf(&b, t->colptr[i] ? "\"\"" : "0");
                else cell_literal(&b, c, quoted_kotlin);
            }
            bprintf(&b, "),\n");
        }
        bprintf(&b, ")\n");
        write_out(out_kotlin, &b);
        free(b.s);
        return;
    }
    // Kotlin's Int is 32 bits, where TypeScript's number and Swift's Int are not,
    // so the leaf type is a question about the VALUES here and not only about
    // whether they are strings.
    const char *leaf = "String";
    for (long i = 0; i < t->dim[0] * (t->ndim == 2 ? t->dim[1] : 1); i++)
        if (t->cells[i].kind == 'i') {
            leaf = "Int";
            for (long j = 0; j < t->dim[0] * (t->ndim == 2 ? t->dim[1] : 1); j++)
                if (t->cells[j].kind == 'i' && (t->cells[j].i > 2147483647LL || t->cells[j].i < -2147483648LL)) leaf = "Long";
            break;
        }
    char ty[256];
    if (t->ndim == 1) snprintf(ty, sizeof ty, lab[0].name ? "Map<String, %s>" : "List<%s>", leaf);
    else {
        char inner[128];
        snprintf(inner, sizeof inner, lab[1].name ? "Map<String, %s>" : "List<%s>", leaf);
        snprintf(ty, sizeof ty, lab[0].name ? "Map<String, %s>" : "List<%s>", inner);
    }
    static const Syntax sx = { "mapOf(", ")", "listOf(", ")", " to", "mapOf()", quoted_kotlin };
    bprintf(&b, "\nval %s: %s = ", export_name, ty);
    emit_dim(&b, t, lab, 0, 0, &sx, "");
    bprintf(&b, "\n");
    write_out(out_kotlin, &b);
    free(b.s);
}

static void emit_json(Table *t, Labels *lab) {
    Buf b = {0};
    static const Syntax sx = { "{", "}", "[", "]", ":", "{}", quoted };
    emit_dim(&b, t, lab, 0, 0, &sx, "");
    bprintf(&b, "\n");
    // JSON has no trailing commas: take them back out. Cheap, and it keeps one
    // emitter rather than threading a "last element" flag through it.
    Buf j = {0};
    for (size_t i = 0; i < b.n; i++) {
        if (b.s[i] == ',') {
            size_t k = i + 1;
            while (k < b.n && (b.s[k] == ' ' || b.s[k] == '\n')) k++;
            if (k < b.n && (b.s[k] == '}' || b.s[k] == ']')) continue;
        }
        bprintf(&j, "%c", b.s[i]);
    }
    write_out(out_json, &j);
    free(b.s); free(j.s);
}

// ---- TU ----------------------------------------------------------------------
//
// The probe itself is tools/sgcommon; what is datagen's own is the one warning
// it turns off and the flags the caller passed with --flags, taken as they come:
// this tool reads values, and a flag it does not understand can only change
// what the header says a value IS, which is the caller's business.
static void build_args(void) {
    sgc_args_base(target);
    // A table of literals is not compiled by anything, so it is read with the
    // warnings off: an unused static in a header is exactly what it is.
    sgc_arg("-Wno-unused-const-variable");
    char *fl = xstrdup(flags);
    for (char *tok = strtok(fl, " \t\n"); tok; tok = strtok(NULL, " \t\n")) sgc_arg(tok);
}

static void usage(void) {
    fputs("usage: datagen --cwd DIR --header H... --table T [--labels T.DIM=L]...\n"
          "               [--require-complete] [--require-complete-if T.COL]\n"
          "               [--ts-const] [--flags FLAGS] [--target TRIPLE] [--name IDENT]\n"
          "               [--ts OUT.ts] [--swift OUT.swift] [--json OUT.json]\n"
          "               [--kotlin OUT.kt --kotlin-package PKG]\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--require-complete")) { require_complete = 1; continue; }
        if (!strcmp(a, "--ts-const")) { ts_const = 1; continue; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) usage();
        if (strncmp(a, "--", 2)) die("unexpected argument %s", a);
        if (i + 1 >= argc) die("%s needs a value", a);
        const char *v = argv[++i];
        if (!strcmp(a, "--header")) { if (nheaders == MAXN) die("too many --header"); headers[nheaders++] = v; }
        else if (!strcmp(a, "--table")) { if (table_name) die("one --table per run"); table_name = v; }
        else if (!strcmp(a, "--labels")) {
            // T.DIM=L
            char *s = xstrdup(v), *dot = strchr(s, '.'), *eq = dot ? strchr(dot, '=') : NULL;
            if (!dot || !eq || !eq[1]) die("--labels must be TABLE.DIM=LABELTABLE");
            *dot = 0; *eq = 0;
            if (!table_name || strcmp(s, table_name)) die("--labels names %s, but --table is %s (give --table first)", s, table_name ? table_name : "(none)");
            char *end;
            long d = strtol(dot + 1, &end, 10);
            if (*end || d < 0 || d >= MAXDIM) die("--labels dimension must be 0 or 1");
            if (label_table[d]) die("--labels given twice for dimension %ld", d);
            label_table[d] = eq + 1;
        }
        else if (!strcmp(a, "--name")) export_name = v;
        else if (!strcmp(a, "--ts")) out_ts = v;
        else if (!strcmp(a, "--swift")) out_swift = v;
        else if (!strcmp(a, "--kotlin")) out_kotlin = v;
        else if (!strcmp(a, "--kotlin-package")) kotlin_package = v;
        else if (!strcmp(a, "--json")) out_json = v;
        else if (!strcmp(a, "--target")) target = v;
        else if (!strcmp(a, "--flags")) flags = v;
        else if (!strcmp(a, "--require-complete-if")) { complete_if = v; require_complete = 1; }
        else if (!strcmp(a, "--cwd")) cwd = v;
        else die("unknown argument %s", a);
    }
    if (!nheaders || !table_name) usage();
    if (!out_ts && !out_swift && !out_kotlin && !out_json) die("nothing to do: give --ts, --swift, --kotlin and/or --json");
    if (out_kotlin && !kotlin_package) die("--kotlin needs --kotlin-package: a Kotlin file declares the package it is in");
    if (kotlin_package && !out_kotlin) die("--kotlin-package without --kotlin: nothing is being written in Kotlin");
    if (!export_name) export_name = table_name;
    out_ts = absolute(out_ts); out_swift = absolute(out_swift); out_json = absolute(out_json);
    out_kotlin = absolute(out_kotlin);
    if (chdir(cwd)) die("cannot cd to %s", cwd);
    build_args();

    Buf src = {0};
    for (int i = 0; i < nheaders; i++) bprintf(&src, "#include \"%s\"\n", headers[i]);
    CXIndex idx = clang_createIndex(0, 0);
    CXTranslationUnit tu = sgc_parse(idx, "__datagen_probe.c", src.s, 0, " reading the table's headers");

    Table t = {0};
    read_table(tu, table_name, &t);
    Labels lab[MAXDIM] = {{0}};
    if (t.is_struct) {
        if (label_table[1]) die("%s is an array of %s: its columns are already named by that struct's fields", table_name, t.rec_name);
        lab[1].n = t.nfield; lab[1].name = t.field;
    }
    for (int d = 0; d < MAXDIM; d++) {
        if (!label_table[d]) continue;
        if (d >= t.ndim) die("--labels for dimension %d, but %s has %d", d, table_name, t.ndim);
        read_labels(tu, label_table[d], t.dim[d], &lab[d]);
    }
    // --require-complete-if: which slots are REQUIRED, read out of a companion
    // table's column. This is the two-tier rule, as data.
    //
    // A key nobody has translated yet and a key that does not exist are not the
    // same mistake. The second is a bug - the enum and the table disagree, and
    // some host will ask for a name nothing answers to. The first is just work
    // not done, and making it a build error would mean nobody could add a string
    // without doing twenty-five translations in the same commit, which is how a
    // key ends up quietly added to English only.
    //
    // So c/i18n/keys.h says, per key, whether every language must carry it, and
    // the ones that must are checked here. The rest are holes, left out of the
    // emitted module, and the host falls back to English - visibly, and counted
    // by e2e/validation/i18n_source_of_truth.test.ts.
    int *required = NULL;
    if (complete_if) {
        Table c = {0}; long col;
        read_column(tu, complete_if, &c, &col);
        if (c.dim[0] != t.dim[0])
            die("--require-complete-if %s has %ld rows for a dimension of %ld", complete_if, c.dim[0], t.dim[0]);
        if (!(required = calloc((size_t)c.dim[0], sizeof *required))) die("out of memory");
        for (long i = 0; i < c.dim[0]; i++) {
            Cell *cell = cell_at(&c, i, col);
            if (cell->kind != 'i') die("--require-complete-if %s has no integer at index %ld", complete_if, i);
            required[i] = cell->i != 0;
        }
    }
    if (require_complete) {
        Buf missing = {0}; int nmissing = 0;
        for (long a = 0; a < t.dim[0]; a++)
            for (long b = 0; b < cols_of(&t); b++) {
                if (cell_at(&t, a, b)->kind) continue;
                // A one-dimensional table is indexed by `a`; a two-dimensional
                // one asks about its column, since that is the key axis.
                if (required && !required[t.ndim == 2 ? b : a]) continue;
                if (nmissing < 20) {
                    bprintf(&missing, "\n  ");
                    if (lab[0].name) bprintf(&missing, "%s", lab[0].name[a]); else bprintf(&missing, "[%ld]", a);
                    if (t.ndim == 2) { bprintf(&missing, " . "); if (lab[1].name) bprintf(&missing, "%s", lab[1].name[b]); else bprintf(&missing, "[%ld]", b); }
                }
                nmissing++;
            }
        if (nmissing)
            die("%s is missing %d value(s) that every language must carry%s:%s%s",
                table_name, nmissing, complete_if ? " (c/i18n/keys.h marks these `1`)" : "",
                missing.s ? missing.s : "", nmissing > 20 ? "\n  ... and more" : "");
    }

    if (out_ts) emit_ts(&t, lab);
    if (out_swift) emit_swift(&t, lab);
    if (out_kotlin) emit_kotlin(&t, lab);
    if (out_json) emit_json(&t, lab);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
    return 0;
}
