// ---- the Swift emitter ------------------------------------------------------
//
// The same snapshots and writers as sg_ts.c, over a POINTER TO THE C STRUCT: a
// Swift host links the kernel, so there is no linear memory and no `Mem`.
// Everything else - which records are copied, which arrays are counted, what a
// bad count does - is the model (sg_model.h), shared with the TS emitter.
#include <stdio.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_model.h"
#include "sg_swift.h"

// Swift's reserved words, as far as a member name can collide with one: a
// declaration name may be any of these in backticks, which is what a field
// called `in`, `case` or `default` gets.
static int swift_reserved(const char *s) {
    static const char *kw[] = {
        "associatedtype", "class", "deinit", "enum", "extension", "fileprivate", "func", "import", "init",
        "inout", "internal", "let", "open", "operator", "private", "precedencegroup", "protocol", "public",
        "rethrows", "static", "struct", "subscript", "typealias", "var", "break", "case", "catch", "continue",
        "default", "defer", "do", "else", "fallthrough", "for", "guard", "if", "in", "repeat", "return",
        "throw", "throws", "switch", "where", "while", "Any", "as", "await", "false", "is", "nil", "self",
        "Self", "super", "true", "try", "Protocol", "Type",
    };
    for (size_t i = 0; i < sizeof kw / sizeof *kw; i++) if (!strcmp(s, kw[i])) return 1;
    return 0;
}
// A field's Swift member name: camelCase, in backticks when that is a keyword.
static const char *swift_name(const char *field) {
    static char buf[4][260];
    static int k;
    char *b = buf[k++ % 4];
    const char *c = camel(field);
    if (swift_reserved(c)) snprintf(b, sizeof buf[0], "`%s`", c); else snprintf(b, sizeof buf[0], "%s", c);
    return b;
}

// A scalar's Swift type. Integers WIDEN to Int, as the TS side widens to
// `number`: a snapshot is read and compared, not stored back into the same
// width, and a host that had to write `Int(v.handCount)` at every use would
// grow exactly the conversions this generator exists to remove. Eight bytes
// keep their signedness, where Int would lose the top of a u64 (game_id).
static const char *swift_scalar_type(char kind, long size) {
    if (kind == 'b') return "Bool";
    if (kind == 'f') return "Double";
    if (size == 8) return kind == 'u' ? "UInt64" : "Int64";
    if (size == 1 || size == 2 || size == 4) return "Int";
    return NULL;
}
// The Swift type of a snapshot field's element.
static const char *swift_elem_type(Rec *t) {
    static char buf[4][128];
    static int k;
    char *b = buf[k++ % 4];
    if (t->record) snprintf(b, sizeof buf[0], "%sSnap", t->name);
    else {
        const char *s = swift_scalar_type(t->kind, t->size);
        if (!s) die("no Swift type for kind %c size %ld", t->kind, t->size);
        snprintf(b, sizeof buf[0], "%s", s);
    }
    return b;
}
// The type of a whole snapshot field (an array, a string or one element).
static const char *swift_field_type(Rec *r, Field *f) {
    static char buf[4][160];
    static int k;
    char *b = buf[k++ % 4];
    (void)r;
    if (f->width) return f->kind == 'b' ? "Bool" : "Int";
    Rec *t = &recs[f->type];
    int ptr = t->kind == 'p';
    if (ptr) t = &recs[t->pointee];
    if (t->charlike && (f->nd == 1 || ptr)) return "String";
    if (f->nd == 1 || ptr) { snprintf(b, sizeof buf[0], "[%s]", swift_elem_type(t)); return b; }
    return swift_elem_type(t);
}

// The Swift type one scalar is LOADED as: the C type, exactly.
static const char *swift_raw_type(char kind, long size) {
    if (kind == 'b') return "UInt8";
    if (kind == 'f') return size == 4 ? "Float" : "Double";
    switch (size) {
    case 1: return kind == 'u' ? "UInt8" : "Int8";
    case 2: return kind == 'u' ? "UInt16" : "Int16";
    case 4: return kind == 'u' ? "UInt32" : "Int32";
    case 8: return kind == 'u' ? "UInt64" : "Int64";
    }
    die("no Swift load for kind %c size %ld", kind, size);
    return NULL;
}
// ...and read back as the snapshot's own (widened) type.
static const char *swift_read(char kind, long size, const char *base, const char *off) {
    static char buf[4][512];
    static int k;
    char *b = buf[k++ % 4];
    char load[400];
    snprintf(load, sizeof load, "%s.load(fromByteOffset: %s, as: %s.self)", base, off, swift_raw_type(kind, size));
    if (kind == 'b') snprintf(b, sizeof buf[0], "%s != 0", load);
    else if (kind == 'f') snprintf(b, sizeof buf[0], size == 4 ? "Double(%s)" : "%s", load);
    else if (size == 8) snprintf(b, sizeof buf[0], "%s", load);
    else snprintf(b, sizeof buf[0], "Int(%s)", load);
    return b;
}
// One scalar stored back. `val` is an expression of the snapshot's type.
static void swift_store(Buf *sw, char kind, long size, const char *base, const char *off, const char *val) {
    const char *t = swift_raw_type(kind, size);
    char conv[700];
    if (kind == 'b') snprintf(conv, sizeof conv, "UInt8((%s) ? 1 : 0)", val);
    else if (kind == 'f') snprintf(conv, sizeof conv, size == 4 ? "Float(%s)" : "%s", val);
    else snprintf(conv, sizeof conv, "%s(truncatingIfNeeded: %s)", t, val);
    bprintf(sw, "%s.storeBytes(of: %s, toByteOffset: %s, as: %s.self)", base, conv, off, t);
}
// The unsigned window a bitfield lives in.
static int swift_window_bytes(Field *f) {
    int bytes = (f->lo + f->width + 7) / 8;
    return bytes == 3 ? 4 : bytes;
}
static const char *swift_window(Field *f, const char *base) {
    static char buf[4][300];
    static int k;
    char *b = buf[k++ % 4];
    snprintf(b, sizeof buf[0], "UInt32(%s.load(fromByteOffset: %ld, as: UInt%d.self))", base, f->off, swift_window_bytes(f) * 8);
    return b;
}
// A bitfield read out of that window, as Int or Bool.
static const char *swift_bits_read(Field *f, const char *base) {
    static char buf[4][600];
    static int k;
    char *b = buf[k++ % 4];
    const int up = 32 - f->lo - f->width, down = 32 - f->width;
    if (f->kind == 'i')
        snprintf(b, sizeof buf[0], "Int(Int32(bitPattern: %s << %d) >> %d)", swift_window(f, base), up, down);
    else if (f->kind == 'b')
        snprintf(b, sizeof buf[0], "((%s << %d) >> %d) != 0", swift_window(f, base), up, down);
    else
        snprintf(b, sizeof buf[0], "Int((%s << %d) >> %d)", swift_window(f, base), up, down);
    return b;
}

static void emit_swift_snapshot(Buf *sw, Rec *r, int *strings) {
    bprintf(sw, "\n/// %s (c struct), copied out of the kernel's storage.\n", r->name);
    bprintf(sw, "public struct %sSnap: Sendable, Equatable {\n", r->name);
    bprintf(sw, "    /// sizeof(%s) under this build: what a host allocates to hand C one.\n", r->name);
    bprintf(sw, "    public static let cSize = %ld\n", r->size);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(sw, "    public let %s: %s\n", swift_name(f->name), swift_field_type(r, f));
    }
    // The memberwise init is internal by default, and a snapshot is a public
    // value: a host builds one to hand back through a writer.
    bprintf(sw, "    public init(");
    for (int j = 0, first = 1; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(sw, "%s%s: %s", first ? "" : ", ", swift_name(f->name), swift_field_type(r, f));
        first = 0;
    }
    bprintf(sw, ") {\n");
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(sw, "        self.%s = %s\n", swift_name(f->name), swift_name(f->name));
    }
    bprintf(sw, "    }\n}\n");

    bprintf(sw, "public func read%s(_ p: UnsafeRawPointer) throws -> %sSnap {\n", r->name, r->name);
    // Counts first: every one is checked before anything is read with it.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        Count *c = f->width ? NULL : count_for(r->name, f->name);
        if (!c) continue;
        Field *cf = field_named(r, c->count);
        char off[32];
        snprintf(off, sizeof off, "%ld", cf->off);
        bprintf(sw, "    let n_%s = %s\n", f->name, swift_read(recs[cf->type].kind, recs[cf->type].size, "p", off));
        Rec *t = &recs[f->type];
        if (t->kind == 'p') {
            bprintf(sw, "    let a_%s = p.load(fromByteOffset: %ld, as: UnsafeRawPointer?.self)\n", f->name, f->off);
            bprintf(sw, "    if n_%s != 0 && a_%s == nil { throw SGLayoutError.null(field: \"%s.%s\", count: n_%s) }\n",
                    f->name, f->name, r->name, f->name, f->name);
            // A pointer has no capacity to check a count against, but a NEGATIVE
            // count is still not one: it would be read as a range and crash.
            bprintf(sw, "    if n_%s < 0 { throw SGLayoutError.count(field: \"%s.%s\", got: n_%s, capacity: Int.max) }\n",
                    f->name, r->name, f->name, f->name);
            continue;
        }
        bprintf(sw, "    if n_%s < 0 || n_%s > %ld { throw SGLayoutError.count(field: \"%s.%s\", got: n_%s, capacity: %ld) }\n",
                f->name, f->name, f->dims[0], r->name, f->name, f->name, f->dims[0]);
    }
    // Then the arrays, each bounded by a count that has been checked.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        const int ptr = t->kind == 'p';
        if (ptr) t = &recs[t->pointee];
        if (!ptr && f->nd != 1) continue;
        if (t->charlike) continue;
        char n[64];
        if (count_for(r->name, f->name)) snprintf(n, sizeof n, "n_%s", f->name); else snprintf(n, sizeof n, "%ld", f->dims[0]);
        bprintf(sw, "    var %s: [%s] = []\n", swift_name(f->name), swift_elem_type(t));
        bprintf(sw, "    %s.reserveCapacity(%s)\n", swift_name(f->name), n);
        char elem[128];
        if (t->size == 1) snprintf(elem, sizeof elem, "i"); else snprintf(elem, sizeof elem, "i * %ld", t->size);
        if (ptr) {
            bprintf(sw, "    if let a = a_%s {\n        for i in 0..<%s { %s.append(", f->name, n, swift_name(f->name));
            if (t->record) bprintf(sw, "try read%s(a + %s)", t->name, elem);
            else bprintf(sw, "%s", swift_read(t->kind, t->size, "a", elem));
            bprintf(sw, ") }\n    }\n");
            continue;
        }
        char off[128];
        if (t->size == 1) snprintf(off, sizeof off, "%ld + i", f->off); else snprintf(off, sizeof off, "%ld + i * %ld", f->off, t->size);
        bprintf(sw, "    for i in 0..<%s { %s.append(", n, swift_name(f->name));
        if (t->record) bprintf(sw, "try read%s(p + %s)", t->name, off);
        else bprintf(sw, "%s", swift_read(t->kind, t->size, "p", off));
        bprintf(sw, ") }\n");
    }
    bprintf(sw, "    return %sSnap(", r->name);
    for (int j = 0, first = 1; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(sw, "%s%s: ", first ? "" : ", ", swift_name(f->name));
        first = 0;
        if (f->width) { bprintf(sw, "%s", swift_bits_read(f, "p")); continue; }
        Rec *t = &recs[f->type];
        char off[64];
        snprintf(off, sizeof off, "%ld", f->off);
        if (t->kind == 'p') {   // counted (snap_mark): copied above, or a counted string
            if (recs[t->pointee].charlike) {
                *strings |= STR_UTF8_GET;
                bprintf(sw, "a_%s.map { sgUTF8($0, 0, n_%s) } ?? \"\"", f->name, f->name);
            } else bprintf(sw, "%s", swift_name(f->name));
        } else if (t->charlike && f->nd == 1) {
            if (count_for(r->name, f->name)) { *strings |= STR_UTF8_GET; bprintf(sw, "sgUTF8(p, %ld, n_%s)", f->off, f->name); }
            else { *strings |= STR_CSTR_GET; bprintf(sw, "sgCStr(p, %ld, %ld)", f->off, f->dims[0]); }
        } else if (f->nd == 1) {
            bprintf(sw, "%s", swift_name(f->name));
        } else if (t->record) {
            bprintf(sw, "try read%s(p + %ld)", t->name, f->off);
        } else {
            bprintf(sw, "%s", swift_read(t->kind, t->size, "p", off));
        }
    }
    bprintf(sw, ")\n}\n");
}

static void emit_swift_writer(Buf *sw, Rec *r, int *strings) {
    bprintf(sw, "public func write%s(_ p: UnsafeMutableRawPointer, _ s: %sSnap) throws {\n", r->name, r->name);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        char name[300];
        snprintf(name, sizeof name, "s.%s", swift_name(f->name));
        if (f->width) {
            const unsigned mask = (unsigned)((((1ull << f->width) - 1) << f->lo) & 0xffffffffull);
            const int bytes = swift_window_bytes(f);
            char val[400];
            if (f->kind == 'b') snprintf(val, sizeof val, "UInt32(%s ? 1 : 0)", name);
            else snprintf(val, sizeof val, "UInt32(truncatingIfNeeded: %s)", name);
            bprintf(sw, "    p.storeBytes(of: UInt%d(truncatingIfNeeded: (%s & ~UInt32(%u)) | ((%s << %d) & UInt32(%u))), toByteOffset: %ld, as: UInt%d.self)\n",
                    bytes * 8, swift_window(f, "p"), mask, val, f->lo, mask, f->off, bytes * 8);
            continue;
        }
        Rec *t = &recs[f->type];
        Count *c = count_for(r->name, f->name);
        Field *cf = c ? field_named(r, c->count) : NULL;
        char coff[32] = "0";
        if (cf) snprintf(coff, sizeof coff, "%ld", cf->off);
        if (t->charlike && f->nd == 1) {
            if (c) {
                *strings |= STR_UTF8_SET;
                char n[500];
                snprintf(n, sizeof n, "try sgUTF8Set(p, %ld, %ld, %s, \"%s.%s\")", f->off, f->dims[0], name, r->name, f->name);
                bprintf(sw, "    ");
                swift_store(sw, recs[cf->type].kind, recs[cf->type].size, "p", coff, n);
                bprintf(sw, "\n");
            } else {
                *strings |= STR_ACCESSORS;
                bprintf(sw, "    try sgCStrSet(p, %ld, %ld, %s, \"%s.%s\")\n", f->off, f->dims[0], name, r->name, f->name);
            }
            continue;
        }
        if (f->nd == 1) {
            // `name` is the Swift expression for the field (up to the 300 bytes
            // declared above), and this holds it plus ".count", so it is sized
            // off that buffer rather than guessed: gcc's -Wformat-truncation
            // reads the declared size, and a smaller one is an error under -Werror.
            char n[sizeof name + 8];
            if (c) {
                bprintf(sw, "    if %s.count > %ld { throw SGLayoutError.tooLong(field: \"%s.%s\", got: %s.count, capacity: %ld) }\n",
                        name, f->dims[0], r->name, f->name, name, f->dims[0]);
                snprintf(n, sizeof n, "%s.count", name);
                bprintf(sw, "    ");
                swift_store(sw, recs[cf->type].kind, recs[cf->type].size, "p", coff, n);
                bprintf(sw, "\n");
            } else {
                bprintf(sw, "    if %s.count != %ld { throw SGLayoutError.tooLong(field: \"%s.%s\", got: %s.count, capacity: %ld) }\n",
                        name, f->dims[0], r->name, f->name, name, f->dims[0]);
                snprintf(n, sizeof n, "%ld", f->dims[0]);
            }
            char off[128];
            if (t->size == 1) snprintf(off, sizeof off, "%ld + i", f->off); else snprintf(off, sizeof off, "%ld + i * %ld", f->off, t->size);
            bprintf(sw, "    for i in 0..<%s { ", n);
            if (t->record) bprintf(sw, "try write%s(p + %s, %s[i])", t->name, off, name);
            else {
                char el[400];
                snprintf(el, sizeof el, "%s[i]", name);
                swift_store(sw, t->kind, t->size, "p", off, el);
            }
            bprintf(sw, " }\n");
            continue;
        }
        char off[32];
        snprintf(off, sizeof off, "%ld", f->off);
        if (t->record) { bprintf(sw, "    try write%s(p + %ld, %s)\n", t->name, f->off, name); continue; }
        bprintf(sw, "    ");
        swift_store(sw, t->kind, t->size, "p", off, name);
        bprintf(sw, "\n");
    }
    bprintf(sw, "}\n");
}

void sg_swift_module(Buf *sw, int *strings) {
    if (nconsts) bprintf(sw, "\n// constants\n");
    for (int p = 0; p < nprefixes; p++)
        for (int m = 1; m >= 0; m--)
            for (int j = 0; j < nconsts; j++)
                if (consts[j].prefix == p && consts[j].macro == m)
                    bprintf(sw, "public let %s = %lld\n", consts[j].name, consts[j].value);
    for (int i = 0; i < nrecs; i++) if (recs[i].snap) emit_swift_snapshot(sw, &recs[i], strings);
    for (int i = 0; i < nrecs; i++) if (recs[i].writer) emit_swift_writer(sw, &recs[i], strings);
    // The struct and function names only: `public let` is also how a member
    // is declared, and two records are free to have a field of one name.
    // Constants cannot collide with each other (add_const dedups by name).
    check_unique_names(sw, "public struct ", ":");
    check_unique_names(sw, "public func ", "(");
}

void sg_swift_write(const char *path, const Buf *sw, int strings, unsigned hash) {
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s", path);
    emit_banner(fp);
    fprintf(fp, "// target: %s\n\n", target);
    fprintf(fp,
        "/// The layout THESE readers were generated for. A host compares it with the\n"
        "/// hash the library it links was stamped with (-DSG_LAYOUT_HASH), so a stale\n"
        "/// binding or a stale library is a refusal at startup and never a wrong offset.\n"
        "public let SG_LAYOUT_HASH: UInt32 = 0x%08x\n\n", hash);
    fputs("/// Why a generated reader or writer refused. Every one of them names the field\n"
          "/// and the value: a payload that does not read whole is not read at all.\n"
          "public enum SGLayoutError: Error, Equatable, Sendable {\n"
          "    /// A count outside 0...capacity - a struct that does not describe itself.\n"
          "    /// Behind a pointer there is no capacity to check, and the capacity is Int.max.\n"
          "    case count(field: String, got: Int, capacity: Int)\n"
          "    /// A NULL pointer with a count, so there is nothing to copy the count from.\n"
          "    case null(field: String, count: Int)\n"
          "    /// A value with more elements or bytes than the field holds.\n"
          "    case tooLong(field: String, got: Int, capacity: Int)\n"
          "}\n", fp);
    if (strings & STR_UTF8_GET) fputs(
        "\n@inline(__always) private func sgUTF8(_ p: UnsafeRawPointer, _ o: Int, _ n: Int) -> String {\n"
        "    String(decoding: UnsafeRawBufferPointer(start: p + o, count: n), as: UTF8.self)\n"
        "}\n", fp);
    if (strings & STR_CSTR_GET) fputs(
        "\n@inline(__always) private func sgCStr(_ p: UnsafeRawPointer, _ o: Int, _ n: Int) -> String {\n"
        "    let b = UnsafeRawBufferPointer(start: p + o, count: n)\n"
        "    var e = 0\n"
        "    while e < n && b[e] != 0 { e += 1 }\n"
        "    return String(decoding: UnsafeRawBufferPointer(rebasing: b[0..<e]), as: UTF8.self)\n"
        "}\n", fp);
    if (strings & STR_UTF8_SET) fputs(
        "\n// A counted char array: its bytes, a NUL when there is room; returns the count.\n"
        "@inline(__always) private func sgUTF8Set(_ p: UnsafeMutableRawPointer, _ o: Int, _ n: Int,\n"
        "                                        _ s: String, _ what: String) throws -> Int {\n"
        "    let b = Array(s.utf8)\n"
        "    if b.count > n { throw SGLayoutError.tooLong(field: what, got: b.count, capacity: n) }\n"
        "    let d = (p + o).bindMemory(to: UInt8.self, capacity: n)\n"
        "    for i in 0..<b.count { d[i] = b[i] }\n"
        "    if b.count < n { d[b.count] = 0 }\n"
        "    return b.count\n"
        "}\n", fp);
    if (strings & STR_ACCESSORS) fputs(
        "\n// A NUL-terminated char[N]: at most N-1 bytes, NUL-padded to N.\n"
        "@inline(__always) private func sgCStrSet(_ p: UnsafeMutableRawPointer, _ o: Int, _ n: Int,\n"
        "                                        _ s: String, _ what: String) throws {\n"
        "    let b = Array(s.utf8)\n"
        "    if b.count > n - 1 { throw SGLayoutError.tooLong(field: what, got: b.count, capacity: n - 1) }\n"
        "    let d = (p + o).bindMemory(to: UInt8.self, capacity: n)\n"
        "    for i in 0..<b.count { d[i] = b[i] }\n"
        "    for i in b.count..<n { d[i] = 0 }\n"
        "}\n", fp);
    fwrite(sw->s, 1, sw->n, fp);
    if (fclose(fp)) die("cannot write %s", path);
}
