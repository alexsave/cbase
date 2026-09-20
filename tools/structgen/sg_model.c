// The model the traversal builds, the lookups over it, and the checks that
// refuse a request it cannot honour.
#include <stdlib.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_model.h"
// scalar_access: a snapshot may only follow a pointer to a scalar the TS
// emitter has a reader for, and that table is the emitter's.
#include "sg_ts.h"

Spec specs[MAXN];
int nspecs;
Spec *spec_for(const char *type) {
    for (int i = 0; i < nspecs; i++) if (!strcmp(specs[i].type, type)) return &specs[i];
    return NULL;
}
int spec_has(Spec *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->names[i], name)) return s->seen[i] = 1;
    return 0;
}

Count counts[MAXN];
int ncounts;
Count *count_for(const char *rec, const char *field) {
    for (int i = 0; i < ncounts; i++)
        if (!strcmp(counts[i].type, rec) && !strcmp(counts[i].field, field)) return &counts[i];
    return NULL;
}
int is_count_field(const char *rec, const char *field) {
    for (int i = 0; i < ncounts; i++)
        if (!strcmp(counts[i].type, rec) && !strcmp(counts[i].count, field)) return 1;
    return 0;
}

Rec *recs;
int nrecs, caprecs;
int root_rec[MAXN];

Field *field_named(Rec *r, const char *name) {
    for (int j = 0; j < r->nf; j++) if (!strcmp(r->f[j].name, name)) return &r->f[j];
    return NULL;
}

// Marks `ri` and every record its fields reach as snapshotted.
void snap_mark(int ri) {
    Rec *r = &recs[ri];
    if (r->snap) return;
    if (r->is_union) die("--snapshot: %s is a union, which has no one value to copy", r->name);
    r->snap = 1;
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        if (t->kind == 'p') {
            if (f->nd) die("--snapshot: %s.%s is an array of pointers; a snapshot follows one pointer by its --count", r->name, f->name);
            if (!count_for(r->name, f->name))
                die("--snapshot: %s.%s is a pointer; give --count %s.%s=<count field> to copy what it points to", r->name, f->name, r->name, f->name);
            if (t->pointee < 0)
                die("--snapshot: %s.%s points to nothing a snapshot can copy (void, a function, an incomplete type or a pointer)", r->name, f->name);
            const char *vt;
            Rec *e = &recs[t->pointee];
            if (!e->record && !scalar_access(e->kind, e->size, 0, &vt))
                die("--snapshot: %s.%s points to a scalar of %ld bytes, which has no reader", r->name, f->name, e->size);
            if (e->record) snap_mark(t->pointee);
            continue;
        }
        if (f->nd > 1) die("--snapshot: %s.%s has %d array dimensions; a snapshot copies one", r->name, f->name, f->nd);
        if (t->record) snap_mark(f->type);
    }
}

// Marks `ri` and every record its fields reach as written.
void writer_mark(int ri) {
    Rec *r = &recs[ri];
    if (r->writer) return;
    r->writer = 1;
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width) continue;
        if (recs[f->type].kind == 'p')
            die("--writer: %s.%s is a pointer; a writer never writes an address C would follow", r->name, f->name);
        if (is_count_field(r->name, f->name)) continue;
        if (recs[f->type].record) writer_mark(f->type);
    }
}

// ---- constants ----------------------------------------------------------------
Const *consts;
int nconsts, capconsts;
static int prefix_of(const char *name) {
    for (int i = 0; i < nprefixes; i++) if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return i;
    return -1;
}
void add_const(const char *name, int macro, long long value) {
    int p = prefix_of(name);
    if (p < 0) return;
    for (int i = 0; i < nconsts; i++) if (!strcmp(consts[i].name, name)) return;
    GROW(consts, nconsts, capconsts);
    consts[nconsts++] = (Const){ xstrdup(name), p, value, macro };
}

void sg_check(void) {
    for (int i = 0; i < nspecs; i++) {
        Spec *s = &specs[i];
        int found = 0;
        for (int j = 0; j < nrecs; j++) found |= recs[j].record && !strcmp(recs[j].name, s->type);
        if (!found) die("--fields %s: no such record reached from the roots", s->type);
        spec_has(s, "SIZE");
        for (int j = 0; j < s->n; j++) if (!s->seen[j]) die("--fields %s: no field named %s", s->type, s->names[j]);
    }

    if (snapshot_only && !nsnaps) die("--snapshot-only without a --snapshot");
    for (int i = 0; i < ncounts; i++) {
        Count *c = &counts[i];
        Rec *r = NULL;
        for (int j = 0; j < nrecs; j++) if (recs[j].record && !strcmp(recs[j].name, c->type)) r = &recs[j];
        if (!r) die("--count %s.%s: no such record reached from the roots", c->type, c->field);
        Field *f = field_named(r, c->field), *cf = field_named(r, c->count);
        if (!f || f->width || (recs[f->type].kind == 'p' ? f->nd != 0 : f->nd != 1))
            die("--count %s.%s: not a one-dimensional array field or a pointer", c->type, c->field);
        if (!cf) die("--count %s.%s=%s: %s has no field named %s", c->type, c->field, c->count, c->type, c->count);
        if (cf->width || cf->nd || recs[cf->type].record || recs[cf->type].kind == 'f' || recs[cf->type].kind == 'p' || recs[cf->type].size > 4)
            die("--count %s.%s=%s: the count is not an integer field of %s", c->type, c->field, c->count, c->type);
    }
    for (int i = 0; i < nsnaps; i++) {
        int found = -1;
        for (int j = 0; j < nrecs; j++) if (recs[j].record && !strcmp(recs[j].name, snaps[i])) found = j;
        if (found < 0) die("--snapshot %s: no such record reached from the roots", snaps[i]);
        snap_mark(found);
    }
    for (int i = 0; i < nwriters; i++) {
        int found = -1;
        for (int j = 0; j < nrecs; j++) if (recs[j].record && recs[j].snap && !strcmp(recs[j].name, writers[i])) found = j;
        if (found < 0) die("--writer %s: not a --snapshot (a writer writes the snapshot type back)", writers[i]);
        writer_mark(found);
    }
    // TWO ARRAYS CANNOT SHARE ONE COUNT IN A WRITER. A reader is happy to read
    // both from the same number; a writer writes that number once per array, so
    // the LAST one silently decides it - and an empty second array (an awire
    // cover's attack list on a move that is not a cover) would write a count of
    // zero over a real one. Refused here rather than emitted, because the bug it
    // makes is a payload that is correctly formed and wrong.
    for (int i = 0; i < ncounts; i++)
        for (int j = i + 1; j < ncounts; j++) {
            if (strcmp(counts[i].type, counts[j].type) || strcmp(counts[i].count, counts[j].count)) continue;
            for (int r = 0; r < nrecs; r++)
                if (recs[r].writer && !strcmp(recs[r].name, counts[i].type))
                    die("--writer %s: %s.%s and %s.%s share the count %s, and a writer writes it once per array",
                        counts[i].type, counts[i].type, counts[i].field, counts[j].type, counts[j].field, counts[i].count);
        }
    for (int i = 0; i < ncounts; i++) {
        int used = 0;
        for (int j = 0; j < nrecs; j++) used |= recs[j].snap && !strcmp(recs[j].name, counts[i].type);
        if (!used) die("--count %s.%s: %s is not in any snapshot", counts[i].type, counts[i].field, counts[i].type);
    }
}
