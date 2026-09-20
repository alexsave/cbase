// ---- the layout hash -------------------------------------------------------------
// FNV-1a over the LAYOUT facts the emitted TS relies on, and nothing else:
//   every emitted field, walked from each --root along its field path
//   ("Game.players.hand.suit"): byte offset, bit range and bit kind, or array
//   dims, element size, element kind (i u b f, r = record) and char-ness;
//   each record SIZE the module emits (requested, or its raw_get/raw_set);
//   each --const constant's name and value, in emission order.
// Paths are made of --root names (the caller's) and field names (the header's);
// record and typedef names are NOT in it. Renaming a typedef renames the
// generated accessors (a TS compile error at every user) but leaves the hash
// alone, and libclang's rendering of a type, which differs between LLVM
// versions, can never reach it. tools/structgen/test/hash.sh pins both halves.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_hash.h"
#include "sg_model.h"

static unsigned layout_hash = 2166136261u;
static int hash_path[256], hash_depth;   // the records on the path being hashed, for a pointer cycle
void hput(const char *fmt, ...) {
    char line[1024];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof line) die("hash line too long");
    for (int i = 0; i <= k; i++) layout_hash = (layout_hash ^ (unsigned char)(i < k ? line[i] : '\n')) * 16777619u;
}
static void hash_record(int ri, const char *path) {
    Rec *r = &recs[ri];
    Spec *s = spec_for(r->name);
    if (hash_depth == 256) die("%s: records nested too deep to hash", path);
    hash_path[hash_depth++] = ri;
    if (!s || spec_has(s, "SIZE") || r->size == 1 || r->size == 2 || r->size == 4) hput("%s size %ld", path, r->size);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width) { hput("%s.%s off %ld bits %d+%d %c", path, f->name, f->off, f->lo, f->width, f->kind); continue; }
        Rec *t = &recs[f->type];
        char dims[128] = "";
        for (int k = 0; k < f->nd; k++) snprintf(dims + strlen(dims), sizeof dims - strlen(dims), "[%ld]", f->dims[k]);
        if (t->kind == 'p') {   // pointer-ness, then the pointee by size and kind, and a record pointee's own layout
            if (t->pointee < 0) { hput("%s.%s off %ld%s elem %ld p -> none", path, f->name, f->off, dims, t->size); continue; }
            Rec *e = &recs[t->pointee];
            hput("%s.%s off %ld%s elem %ld p -> %ld %c%s", path, f->name, f->off, dims, t->size, e->size, e->record ? 'r' : e->kind, e->charlike ? " char" : "");
            if (!e->record) continue;
            int back = 0;
            for (int k = hash_depth - 1; k >= 0 && !back; k--) if (hash_path[k] == t->pointee) back = hash_depth - k;
            Buf child = {0};
            bprintf(&child, "%s.%s*", path, f->name);
            if (back) hput("%s back %d", child.s, back); else hash_record(t->pointee, child.s);
            free(child.s);
            continue;
        }
        hput("%s.%s off %ld%s elem %ld %c%s", path, f->name, f->off, dims, t->size, t->record ? 'r' : t->kind, t->charlike ? " char" : "");
        if (t->record) {
            Buf child = {0};
            bprintf(&child, "%s.%s", path, f->name);
            hash_record(f->type, child.s);
            free(child.s);
        }
    }
    hash_depth--;
}

unsigned sg_layout_hash(void) {
    for (int i = 0; i < nroots; i++) hash_record(root_rec[i], roots[i]);
    return layout_hash;
}

void sg_hash_write_ts(const char *path, unsigned hash) {
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s", path);
    emit_banner(fp);
    fprintf(fp, "export const LAYOUT_HASH = 0x%08x;\n", hash);
    if (fclose(fp)) die("cannot write %s", path);
}
