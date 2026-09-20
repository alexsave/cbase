// The TypeScript emitter: one module of accessors over wasm32 linear memory,
// the snapshot readers that copy a record out of it, and the writers that put
// one back. A pointer is an offset into that memory, read and followed but
// never written (structgen.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_hash.h"
#include "sg_model.h"
#include "sg_ts.h"

const char *scalar_access(char kind, long size, int write, const char **vt) {
    static const struct { char k; long sz; const char *rd, *wr, *vt; } T[] = {
        { 'i', 1, "m.i8[%s]", "m.i8[%s] = v", "number" }, { 'u', 1, "m.u8[%s]", "m.u8[%s] = v", "number" },
        { 'b', 1, "m.u8[%s] !== 0", "m.u8[%s] = v ? 1 : 0", "boolean" },
        { 'i', 2, "m.dv.getInt16(%s, true)", "m.dv.setInt16(%s, v, true)", "number" },
        { 'u', 2, "m.dv.getUint16(%s, true)", "m.dv.setUint16(%s, v, true)", "number" },
        { 'i', 4, "m.dv.getInt32(%s, true)", "m.dv.setInt32(%s, v, true)", "number" },
        { 'u', 4, "m.dv.getUint32(%s, true)", "m.dv.setUint32(%s, v, true)", "number" },
        { 'f', 4, "m.dv.getFloat32(%s, true)", "m.dv.setFloat32(%s, v, true)", "number" },
        { 'f', 8, "m.dv.getFloat64(%s, true)", "m.dv.setFloat64(%s, v, true)", "number" },
        { 'i', 8, "m.dv.getBigInt64(%s, true)", "m.dv.setBigInt64(%s, v, true)", "bigint" },
        { 'u', 8, "m.dv.getBigUint64(%s, true)", "m.dv.setBigUint64(%s, v, true)", "bigint" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].k == kind && T[i].sz == size) { *vt = T[i].vt; return write ? T[i].wr : T[i].rd; }
    return NULL;
}
// "p" or "p + off": the address of a member at byte offset `off`.
static const char *at(long off) {
    static char buf[8][32];
    static int k;
    char *b = buf[k++ % 8];
    if (off) snprintf(b, sizeof buf[0], "p + %ld", off); else snprintf(b, sizeof buf[0], "p");
    return b;
}
// A window of `bytes` little-endian bytes at p + off, as an unsigned integer.
static void window(char *out, size_t cap, long off, int bytes) {
    if (bytes == 1) snprintf(out, cap, "m.u8[%s]", at(off));
    else snprintf(out, cap, "m.dv.getUint%d(%s, true)", bytes == 2 ? 16 : 32, at(off));
}

static void emit_record(Buf *ts, Rec *r, int *strings) {
    Spec *s = spec_for(r->name);
    bprintf(ts, "// %s\n", r->name);
    if (!s || spec_has(s, "SIZE")) bprintf(ts, "export const %s_SIZE = %ld;\n", r->name, r->size);
    int all_bits = r->nf > 0 && !s;
    for (int j = 0; j < r->nf; j++) all_bits &= r->f[j].width > 0;
    if (r->size == 1 || r->size == 2 || r->size == 4) {   // the whole record as one unsigned integer
        const char *vt, *rd = scalar_access('u', r->size, 0, &vt), *wr = scalar_access('u', r->size, 1, &vt);
        bprintf(ts, "export const %s_raw_get = (m: Mem, p: number) => ", r->name); bprintf(ts, rd, "p");
        bprintf(ts, ";\nexport const %s_raw_set = (m: Mem, p: number, v: number) => { ", r->name); bprintf(ts, wr, "p");
        bprintf(ts, "; };\n");
    }
    if (all_bits && r->size <= 4) {                         // pack/unpack the raw integer by field
        bprintf(ts, "export const %s_pack = (", r->name);
        for (int j = 0; j < r->nf; j++) bprintf(ts, "%s_%s: %s", j ? ", " : "", r->f[j].name, r->f[j].kind == 'b' ? "boolean" : "number");
        bprintf(ts, ") => (");
        for (int j = 0; j < r->nf; j++) {
            Field *f = &r->f[j];
            int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "%s((%s_%s << %d) & %u)", j ? " | " : "", f->kind == 'b' ? "+" : "", f->name, pos,
                (unsigned)((((1ull << f->width) - 1) << pos) & 0xffffffffull));
        }
        bprintf(ts, ")%s;\n", r->size == 4 ? " >>> 0" : "");
        for (int j = 0; j < r->nf; j++) {
            Field *f = &r->f[j];
            int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "export const %s_unpack_%s = (r: number) => %s(r << %d) %s %d%s;\n", r->name, f->name,
                f->kind == 'b' ? "(" : "", 32 - pos - f->width, f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
        }
    }
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width) {
            int bytes = (f->lo + f->width + 7) / 8;
            if (bytes == 3) bytes = 4;
            char W[64];
            window(W, sizeof W, f->off, bytes);
            unsigned mask = (unsigned)((((1ull << f->width) - 1) << f->lo) & 0xffffffffull);
            bprintf(ts, "export const %s_get_%s = (m: Mem, p: number) => %s(%s << %d) %s %d%s;\n", r->name, f->name,
                f->kind == 'b' ? "(" : "", W, 32 - f->lo - f->width, f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            const char *vt = f->kind == 'b' ? "boolean" : "number", *v = f->kind == 'b' ? "+v" : "v";
            if (bytes == 1)
                bprintf(ts, "export const %s_set_%s = (m: Mem, p: number, v: %s) => { m.u8[%s] = (%s & %d) | ((%s << %d) & %u); };\n",
                    r->name, f->name, vt, at(f->off), W, (int)~mask, v, f->lo, mask);
            else
                bprintf(ts, "export const %s_set_%s = (m: Mem, p: number, v: %s) => { m.dv.setUint%d(%s, ((%s & %d) | ((%s << %d) & %u)) >>> 0, true); };\n",
                    r->name, f->name, vt, bytes * 8, at(f->off), W, (int)~mask, v, f->lo, mask);
            continue;
        }
        Rec *t = &recs[f->type];
        Buf addr = {0}, idxs = {0};
        bprintf(&addr, "%s", at(f->off));
        long stride = t->size;
        for (int k = f->nd - 1; k >= 0; k--) {
            if (stride == 1) bprintf(&addr, " + i%d", k); else bprintf(&addr, " + i%d * %ld", k, stride);
            stride *= f->dims[k];
        }
        bprintf(&idxs, "%s", "");
        for (int k = 0; k < f->nd; k++) bprintf(&idxs, ", i%d: number", k);
        bprintf(ts, "export const %s_%s_at = (p: number%s) => %s;\n", r->name, f->name, idxs.s, addr.s);
        for (int k = 0; k < f->nd; k++) {
            if (k) bprintf(ts, "export const %s_%s_LEN%d = %ld;\n", r->name, f->name, k, f->dims[k]);
            else bprintf(ts, "export const %s_%s_LEN = %ld;\n", r->name, f->name, f->dims[k]);
        }
        const char *vt, *rd = t->record ? NULL : scalar_access(t->kind, t->size, 0, &vt);
        if (rd) {
            bprintf(ts, "export const %s_get_%s = (m: Mem, p: number%s) => ", r->name, f->name, idxs.s); bprintf(ts, rd, addr.s);
            bprintf(ts, ";\nexport const %s_set_%s = (m: Mem, p: number%s, v: %s) => { ", r->name, f->name, idxs.s, vt);
            bprintf(ts, scalar_access(t->kind, t->size, 1, &vt), addr.s);
            bprintf(ts, "; };\n");
        }
        if (t->kind == 'p') {   // an address to read and follow, never to write
            bprintf(ts, "export const %s_%s_ptr = (m: Mem, p: number%s) => m.dv.getUint32(%s, true);\n", r->name, f->name, idxs.s, addr.s);
            if (t->pointee >= 0) {
                long es = recs[t->pointee].size;
                char end[64], el[64];
                if (es == 1) { snprintf(end, sizeof end, "a + i + 1"); snprintf(el, sizeof el, "a + i"); }
                else { snprintf(end, sizeof end, "a + (i + 1) * %ld", es); snprintf(el, sizeof el, "a + i * %ld", es); }
                bprintf(ts, "export const %s_%s_deref_at = (m: Mem, p: number%s, i: number) => {\n"
                            "    const a = m.dv.getUint32(%s, true);\n"
                            "    if (a === 0) throw new RangeError('%s.%s: NULL');\n"
                            "    if (!(i >= 0) || %s > m.u8.byteLength) throw new RangeError(`%s.%s: element ${i} at ${a} is outside wasm memory (${m.u8.byteLength} bytes)`);\n"
                            "    return %s;\n"
                            "};\n",
                    r->name, f->name, idxs.s, addr.s, r->name, f->name, end, r->name, f->name, el);
            }
        }
        if (t->charlike && f->nd == 1) {   // char[N]: a NUL-terminated UTF-8 string of at most N-1 bytes
            *strings |= STR_ACCESSORS;
            bprintf(ts, "export const %s_get_%s_str = (m: Mem, p: number) => cstrGet(m, %s, %ld);\n", r->name, f->name, at(f->off), f->dims[0]);
            bprintf(ts, "export const %s_set_%s_str = (m: Mem, p: number, v: string) => { cstrSet(m, %s, %ld, v, '%s.%s'); };\n",
                r->name, f->name, at(f->off), f->dims[0], r->name, f->name);
        }
        free(addr.s); free(idxs.s);
    }
}

// ---- snapshots ---------------------------------------------------------------------

// The TS type of a field's element in a snapshot.
static const char *snap_elem_type(Rec *t) {
    static char buf[4][128];
    static int k;
    char *b = buf[k++ % 4];
    const char *vt = "number";
    if (t->record) snprintf(b, sizeof buf[0], "%s_Snap", t->name);
    else { scalar_access(t->kind, t->size, 0, &vt); snprintf(b, sizeof buf[0], "%s", vt); }
    return b;
}

static void emit_snapshot(Buf *ts, Rec *r, int *strings) {
    bprintf(ts, "// %s snapshot\nexport interface %s_Snap {", r->name, r->name);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        const char *type;
        if (f->width) type = f->kind == 'b' ? "boolean" : "number";
        else {
            Rec *t = &recs[f->type];
            int ptr = t->kind == 'p';
            if (ptr) t = &recs[t->pointee];   // a counted pointer copies like a counted array of its pointee
            if (t->charlike && (f->nd == 1 || ptr)) type = "string";
            else if (f->nd == 1 || ptr) { static char arr[160]; snprintf(arr, sizeof arr, "readonly %s[]", snap_elem_type(t)); type = arr; }
            else type = snap_elem_type(t);
        }
        bprintf(ts, " readonly %s: %s;", camel(f->name), type);
    }
    bprintf(ts, " }\nexport const read%s = (m: Mem, p: number): %s_Snap => {\n", r->name, r->name);
    // Counts first: every one is checked before anything is read with it.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        Count *c = f->width ? NULL : count_for(r->name, f->name);
        if (!c) continue;
        Field *cf = field_named(r, c->count);
        const char *vt, *rd = scalar_access(recs[cf->type].kind, recs[cf->type].size, 0, &vt);
        bprintf(ts, "    const n_%s = ", f->name);
        bprintf(ts, rd, at(cf->off));
        Rec *t = &recs[f->type];
        if (t->kind == 'p') {   // the pointer, checked against its count before anything is read through it
            long es = recs[t->pointee].size;
            char bytes[160];
            if (es == 1) snprintf(bytes, sizeof bytes, "n_%s", f->name); else snprintf(bytes, sizeof bytes, "n_%s * %ld", f->name, es);
            bprintf(ts, ";\n    const a_%s = m.dv.getUint32(%s, true);\n    if (n_%s !== 0) {\n", f->name, at(f->off), f->name);
            bprintf(ts, "        if (a_%s === 0) throw new RangeError(`%s.%s: NULL with a count of ${n_%s}`);\n", f->name, r->name, f->name, f->name);
            bprintf(ts, "        if (!(n_%s > 0) || a_%s + %s > m.u8.byteLength) throw new RangeError(`%s.%s: ${n_%s} elements at ${a_%s} are outside wasm memory (${m.u8.byteLength} bytes)`);\n    }\n",
                    f->name, f->name, bytes, r->name, f->name, f->name, f->name);
            continue;
        }
        bprintf(ts, ";\n    if (!(n_%s >= 0 && n_%s <= %ld)) throw new RangeError(`%s.%s: count ${n_%s} is outside 0..%ld`);\n",
                f->name, f->name, f->dims[0], r->name, f->name, f->name, f->dims[0]);
    }
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        int ptr = t->kind == 'p';
        if (!ptr && f->nd != 1) continue;
        char base[160];
        if (ptr) { snprintf(base, sizeof base, "a_%s", f->name); t = &recs[t->pointee]; }
        else snprintf(base, sizeof base, "%s", at(f->off));
        if (t->charlike) continue;
        char n[160];
        if (count_for(r->name, f->name)) snprintf(n, sizeof n, "n_%s", f->name); else snprintf(n, sizeof n, "%ld", f->dims[0]);
        bprintf(ts, "    const %s: %s[] = new Array(%s);\n", camel(f->name), snap_elem_type(t), n);
        bprintf(ts, "    for (let i = 0; i < %s; i++) %s[i] = ", n, camel(f->name));
        char a[256];
        if (t->size == 1) snprintf(a, sizeof a, "%s + i", base); else snprintf(a, sizeof a, "%s + i * %ld", base, t->size);
        if (t->record) bprintf(ts, "read%s(m, %s);\n", t->name, a);
        else { const char *vt; bprintf(ts, scalar_access(t->kind, t->size, 0, &vt), a); bprintf(ts, ";\n"); }
    }
    // A record of bitfields that fits one integer is read once and unpacked.
    int packed = r->nf > 0 && (r->size == 1 || r->size == 2 || r->size == 4);
    for (int j = 0; j < r->nf; j++) packed &= r->f[j].width > 0;
    if (packed) {
        const char *vt;
        bprintf(ts, "    const raw = ");
        bprintf(ts, scalar_access('u', r->size, 0, &vt), "p");
        bprintf(ts, ";\n");
    }
    bprintf(ts, "    return {");
    for (int j = 0, first = 1; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(ts, "%s%s: ", first ? " " : ", ", camel(f->name));
        first = 0;
        if (packed) {
            const int pos = (int)f->off * 8 + f->lo;
            bprintf(ts, "%s(raw << %d) %s %d%s", f->kind == 'b' ? "(" : "", 32 - pos - f->width,
                    f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            continue;
        }
        if (f->width) {
            int bytes = (f->lo + f->width + 7) / 8;
            if (bytes == 3) bytes = 4;
            char W[64];
            window(W, sizeof W, f->off, bytes);
            bprintf(ts, "%s(%s << %d) %s %d%s", f->kind == 'b' ? "(" : "", W, 32 - f->lo - f->width,
                    f->kind == 'i' ? ">>" : ">>>", 32 - f->width, f->kind == 'b' ? ") !== 0" : "");
            continue;
        }
        Rec *t = &recs[f->type];
        if (t->kind == 'p') {   // counted (snap_mark): copied above, or a counted string
            if (recs[t->pointee].charlike) { *strings |= STR_UTF8_GET; bprintf(ts, "utf8Get(m, a_%s, n_%s)", f->name, f->name); }
            else bprintf(ts, "%s", camel(f->name));
        } else if (t->charlike && f->nd == 1) {
            if (count_for(r->name, f->name)) { *strings |= STR_UTF8_GET; bprintf(ts, "utf8Get(m, %s, n_%s)", at(f->off), f->name); }
            else { *strings |= STR_CSTR_GET; bprintf(ts, "cstrGet(m, %s, %ld)", at(f->off), f->dims[0]); }
        } else if (f->nd == 1) {
            bprintf(ts, "%s", camel(f->name));
        } else if (t->record) {
            bprintf(ts, "read%s(m, %s)", t->name, at(f->off));
        } else {
            const char *vt;
            bprintf(ts, scalar_access(t->kind, t->size, 0, &vt), at(f->off));
        }
    }
    bprintf(ts, " };\n};\n");
}

// ---- writers ----------------------------------------------------------------------

// A scalar store of `val` at `addr`.
static void scalar_store(Buf *ts, char kind, long size, const char *addr, const char *val) {
    static const struct { char k; long sz; const char *wr; } T[] = {
        { 'i', 1, "m.i8[%s] = %s" }, { 'u', 1, "m.u8[%s] = %s" }, { 'b', 1, "m.u8[%s] = %s ? 1 : 0" },
        { 'i', 2, "m.dv.setInt16(%s, %s, true)" }, { 'u', 2, "m.dv.setUint16(%s, %s, true)" },
        { 'i', 4, "m.dv.setInt32(%s, %s, true)" }, { 'u', 4, "m.dv.setUint32(%s, %s, true)" },
        { 'f', 4, "m.dv.setFloat32(%s, %s, true)" }, { 'f', 8, "m.dv.setFloat64(%s, %s, true)" },
        { 'i', 8, "m.dv.setBigInt64(%s, %s, true)" }, { 'u', 8, "m.dv.setBigUint64(%s, %s, true)" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].k == kind && T[i].sz == size) { bprintf(ts, T[i].wr, addr, val); return; }
    die("no scalar store for kind %c size %ld", kind, size);
}

static void emit_writer(Buf *ts, Rec *r, int *strings) {
    bprintf(ts, "export const write%s = (m: Mem, p: number, s: %s_Snap): void => {\n", r->name, r->name);
    int packed = r->nf > 0 && (r->size == 1 || r->size == 2 || r->size == 4);
    for (int j = 0; j < r->nf; j++) packed &= r->f[j].width > 0;
    if (packed) {   // a record of bitfields that fits one integer is written once
        Buf v = {0};
        bprintf(&v, "(");
        for (int j = 0; j < r->nf; j++) {
            Field *f = &r->f[j];
            const int pos = (int)f->off * 8 + f->lo;
            bprintf(&v, "%s((%ss.%s << %d) & %u)", j ? " | " : "", f->kind == 'b' ? "+" : "", camel(f->name), pos,
                (unsigned)((((1ull << f->width) - 1) << pos) & 0xffffffffull));
        }
        bprintf(&v, ")%s", r->size == 4 ? " >>> 0" : "");
        bprintf(ts, "    ");
        scalar_store(ts, 'u', r->size, "p", v.s);
        bprintf(ts, ";\n};\n");
        free(v.s);
        return;
    }
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        char name[256];
        snprintf(name, sizeof name, "%s", camel(f->name));
        if (f->width) {
            int bytes = (f->lo + f->width + 7) / 8;
            if (bytes == 3) bytes = 4;
            char W[64];
            window(W, sizeof W, f->off, bytes);
            unsigned mask = (unsigned)((((1ull << f->width) - 1) << f->lo) & 0xffffffffull);
            if (bytes == 1)
                bprintf(ts, "    m.u8[%s] = (%s & %d) | ((%ss.%s << %d) & %u);\n", at(f->off), W, (int)~mask, f->kind == 'b' ? "+" : "", name, f->lo, mask);
            else
                bprintf(ts, "    m.dv.setUint%d(%s, ((%s & %d) | ((%ss.%s << %d) & %u)) >>> 0, true);\n",
                    bytes * 8, at(f->off), W, (int)~mask, f->kind == 'b' ? "+" : "", name, f->lo, mask);
            continue;
        }
        Rec *t = &recs[f->type];
        Count *c = count_for(r->name, f->name);
        Field *cf = c ? field_named(r, c->count) : NULL;
        if (t->charlike && f->nd == 1) {
            if (c) {
                *strings |= STR_UTF8_SET;
                char n[400];
                snprintf(n, sizeof n, "utf8Set(m, %s, %ld, s.%s, '%s.%s')", at(f->off), f->dims[0], name, r->name, f->name);
                bprintf(ts, "    ");
                scalar_store(ts, recs[cf->type].kind, recs[cf->type].size, at(cf->off), n);
                bprintf(ts, ";\n");
            } else {
                *strings |= STR_ACCESSORS;
                bprintf(ts, "    cstrSet(m, %s, %ld, s.%s, '%s.%s');\n", at(f->off), f->dims[0], name, r->name, f->name);
            }
            continue;
        }
        if (f->nd == 1) {
            char n[300], a[300], e[300];
            if (c) {
                bprintf(ts, "    const n_%s = s.%s.length;\n    if (n_%s > %ld) throw new RangeError(`%s.%s: ${n_%s} elements do not fit in %ld`);\n    ",
                    f->name, name, f->name, f->dims[0], r->name, f->name, f->name, f->dims[0]);
                snprintf(n, sizeof n, "n_%s", f->name);
                scalar_store(ts, recs[cf->type].kind, recs[cf->type].size, at(cf->off), n);
                bprintf(ts, ";\n");
            } else {
                bprintf(ts, "    if (s.%s.length !== %ld) throw new RangeError(`%s.%s: ${s.%s.length} elements, not %ld`);\n",
                    name, f->dims[0], r->name, f->name, name, f->dims[0]);
                snprintf(n, sizeof n, "%ld", f->dims[0]);
            }
            if (t->size == 1) snprintf(a, sizeof a, "%s + i", at(f->off)); else snprintf(a, sizeof a, "%s + i * %ld", at(f->off), t->size);
            snprintf(e, sizeof e, "s.%s[i]", name);
            bprintf(ts, "    for (let i = 0; i < %s; i++) ", n);
            if (t->record) bprintf(ts, "write%s(m, %s, %s)", t->name, a, e);
            else scalar_store(ts, t->kind, t->size, a, e);
            bprintf(ts, ";\n");
            continue;
        }
        char sv[300];
        snprintf(sv, sizeof sv, "s.%s", name);
        if (t->record) { bprintf(ts, "    write%s(m, %s, %s);\n", t->name, at(f->off), sv); continue; }
        bprintf(ts, "    ");
        scalar_store(ts, t->kind, t->size, at(f->off), sv);
        bprintf(ts, ";\n");
    }
    bprintf(ts, "};\n");
}

void sg_ts_module(Buf *ts, int *strings) {
    // The constants, hashed as they are written: the layout hash takes each one
    // in EMISSION ORDER (sg_hash.c), so the two are one loop.
    if (nconsts) bprintf(ts, "// constants\n");
    for (int p = 0; p < nprefixes; p++)
        for (int m = 1; m >= 0; m--)
            for (int j = 0; j < nconsts; j++)
                if (consts[j].prefix == p && consts[j].macro == m) {
                    bprintf(ts, "export const %s = %lld;\n", consts[j].name, consts[j].value);
                    hput("const %s %lld", consts[j].name, consts[j].value);
                }
    if (!snapshot_only) for (int i = 0; i < nrecs; i++) if (recs[i].record) emit_record(ts, &recs[i], strings);
    for (int i = 0; i < nrecs; i++) if (recs[i].snap) emit_snapshot(ts, &recs[i], strings);
    for (int i = 0; i < nrecs; i++) if (recs[i].writer) emit_writer(ts, &recs[i], strings);
    check_unique_names(ts, "export const ", " =");
}

void sg_ts_write(const char *path, const Buf *ts, int strings) {
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s", path);
    emit_banner(fp);
    fputs("export interface Mem { u8: Uint8Array; i8: Int8Array; dv: DataView }\n"
          "export const memOf = (b: ArrayBuffer): Mem => ({ u8: new Uint8Array(b), i8: new Int8Array(b), dv: new DataView(b) });\n", fp);
    if (strings & (STR_ACCESSORS | STR_UTF8_SET))   // char[N] as UTF-8; ASCII stays off TextEncoder/TextDecoder
        fputs("const utf8Enc = /* @__PURE__ */ new TextEncoder(), utf8Dec = /* @__PURE__ */ new TextDecoder();\n", fp);
    else if (strings) fputs("const utf8Dec = /* @__PURE__ */ new TextDecoder();\n", fp);
    if (strings & (STR_ACCESSORS | STR_CSTR_GET)) fputs(
        "const cstrGet = (m: Mem, a: number, n: number) => {\n"
        "    let s = '', e = a;\n"
        "    for (const end = a + n; e < end; e++) { const c = m.u8[e]; if (c === 0) return s; if (c > 127) break; s += String.fromCharCode(c); }\n"
        "    while (e < a + n && m.u8[e] !== 0) e++;\n"
        "    return e === a + n && s.length === n ? s : utf8Dec.decode(m.u8.subarray(a, e));\n"
        "};\n", fp);
    if (strings & STR_ACCESSORS) fputs(
        "const cstrSet = (m: Mem, a: number, n: number, s: string, what: string) => {\n"
        "    let i = 0;\n"
        "    const len = s.length;\n"
        "    if (len < n) while (i < len) { const c = s.charCodeAt(i); if (c === 0 || c > 127) break; i++; }\n"
        "    if (i === len && len < n) { for (let j = 0; j < len; j++) m.u8[a + j] = s.charCodeAt(j); }\n"
        "    else {\n"
        "        const b = utf8Enc.encode(s);\n"
        "        if (b.length > n - 1) throw new RangeError(`${what}: ${b.length} UTF-8 bytes do not fit in ${n - 1}`);\n"
        "        if (b.includes(0)) throw new RangeError(`${what}: contains a NUL`);\n"
        "        m.u8.set(b, a); i = b.length;\n"
        "    }\n"
        "    for (let j = i; j < n; j++) m.u8[a + j] = 0;\n"
        "};\n", fp);
    if (strings & STR_UTF8_GET) fputs(   // exactly n bytes of UTF-8 (a counted char array)
        "const utf8Get = (m: Mem, a: number, n: number) => {\n"
        "    if (n > 16) return utf8Dec.decode(m.u8.subarray(a, a + n));   // past a few bytes the decoder wins\n"
        "    let s = '';\n"
        "    for (let i = 0; i < n; i++) { const c = m.u8[a + i]; if (c > 127) return utf8Dec.decode(m.u8.subarray(a, a + n)); s += String.fromCharCode(c); }\n"
        "    return s;\n"
        "};\n", fp);
    if (strings & STR_UTF8_SET) fputs(   // a counted char array: its bytes, a NUL when there is room; returns the count
        "const utf8Set = (m: Mem, a: number, n: number, s: string, what: string) => {\n"
        "    let i = 0;\n"
        "    const len = s.length;\n"
        "    while (i < len && i < n && s.charCodeAt(i) < 128) { m.u8[a + i] = s.charCodeAt(i); i++; }\n"
        "    if (i < len) {\n"
        "        const b = utf8Enc.encode(s);\n"
        "        if (b.length > n) throw new RangeError(`${what}: ${b.length} UTF-8 bytes do not fit in ${n}`);\n"
        "        m.u8.set(b, a); i = b.length;\n"
        "    }\n"
        "    if (i < n) m.u8[a + i] = 0;\n"
        "    return i;\n"
        "};\n", fp);
    fwrite(ts->s, 1, ts->n, fp);
    if (fclose(fp)) die("cannot write %s", path);
}
