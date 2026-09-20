// ---- the Kotlin emitter -----------------------------------------------------
//
// The same snapshots and writers as sg_swift.c, over a java.nio.ByteBuffer.
//
// WHY A BUFFER AND NOT AN ADDRESS. Swift and Kotlin both read a NATIVELY LINKED
// struct - the kernel is C99 and builds under the Android NDK unchanged - so the
// pointer widths and the offsets are the same LP64 facts, and this emitter and
// sg_swift.c hash to one layout. What differs is the last inch. Swift can hold
// the kernel's own address (`UnsafeRawPointer`) and load out of it; the JVM
// cannot address anything, so the bytes have to be handed to it. The `fio_*`
// bridge already hands them over exactly that way - a caller buffer filled with
// packed bytes - which is the shape JNI wants anyway (GetDirectBufferAddress,
// or a jbyteArray it writes through). So the crossing is a ByteBuffer plus a
// byte offset, and the same emitted reader serves both a heap buffer wrapping a
// ByteArray JNI filled and a direct buffer over storage the kernel owns.
//
// WHY THE READERS STILL COPY. A direct ByteBuffer over kernel storage is a
// window, not a value, and the JVM will happily carry one across a coroutine
// suspension into a Compose recomposition on another thread - where it shows
// whatever the kernel wrote last. That is the same hazard the resident-slot
// discipline names on iOS, with a longer fuse, so the answer is the same one:
// `readT` copies into a `data class` that points at nothing.
//
// WHY A POINTER FIELD IS REFUSED HERE AND NOT THERE. Swift follows a `T *f` by
// loading the address and reading at it. A ByteBuffer holds bytes and no way to
// reach an address in the kernel's address space, so there is nothing to follow:
// a pointer field is a refusal at generation time (see kt_no_pointer), which is
// also why the Kotlin refusal type has no null case. The C side has to flatten
// such a field before it crosses.
//
// WHAT WIDENS TO WHAT is the other divergence, and it is Kotlin's `Int` being 32
// bits where Swift's is 64: Swift widens every integer under 8 bytes to Int and
// loses nothing, and doing that here would turn a uint32 of 4000000000 into a
// negative number. So the rule is the narrowest LOSSLESS type - Int through
// int32, Long for uint32 and int64, ULong for uint64 - and a host still writes
// no conversion at a use site.
//
// Everything else - which records are copied, which arrays are counted, what a
// bad count does - is the model (sg_model.h), shared with the other emitters.
#include <stdio.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_kotlin.h"
#include "sg_model.h"

// Kotlin's HARD keywords, the ones a declaration name cannot be. Soft and
// modifier keywords (`data`, `value`, `by`, ...) are contextual and need no
// escaping; a hard one is emitted in backticks, which is Kotlin's own spelling
// for a member called `in`, `is` or `object`.
static int kt_reserved(const char *s) {
    static const char *kw[] = {
        "as", "break", "class", "continue", "do", "else", "false", "for", "fun", "if", "in",
        "interface", "is", "null", "object", "package", "return", "super", "this", "throw",
        "true", "try", "typealias", "typeof", "val", "var", "when", "while",
    };
    for (size_t i = 0; i < sizeof kw / sizeof *kw; i++) if (!strcmp(s, kw[i])) return 1;
    return 0;
}
// A field's Kotlin property name: camelCase, in backticks when that is a keyword.
static const char *kt_name(const char *field) {
    static char buf[4][260];
    static int k;
    char *b = buf[k++ % 4];
    const char *c = camel(field);
    if (kt_reserved(c)) snprintf(b, sizeof buf[0], "`%s`", c); else snprintf(b, sizeof buf[0], "%s", c);
    return b;
}

// A reader's local for an array field: the field's own name, because that is
// what makes the generated line readable - unless it is one of the names the
// reader itself uses, because a C field really can be called `p` or `i`.
static const char *kt_local(const char *field) {
    static char buf[4][280];
    static int k;
    char *b = buf[k++ % 4];
    const char *c = camel(field);
    if (kt_reserved(c) || !strcmp(c, "sgb") || !strcmp(c, "buf") || !strcmp(c, "p") || !strcmp(c, "s") || !strcmp(c, "i"))
        snprintf(b, sizeof buf[0], "a_%s", c);
    else snprintf(b, sizeof buf[0], "%s", c);
    return b;
}

// The refusal a pointer field earns. Said in one place because it is one fact
// about the crossing rather than a case each caller handles differently.
static void kt_no_pointer(Rec *r, Field *f) {
    die("%s.%s is a pointer: Kotlin reads a ByteBuffer, which is a window over bytes and "
        "holds no way to follow an address in the kernel's address space. Flatten the field "
        "in C (a counted array beside its count) before it crosses.", r->name, f->name);
}

// A scalar's Kotlin type: the narrowest one that loses nothing. See the note at
// the top about why this is not Swift's rule.
static const char *kt_scalar_type(char kind, long size) {
    if (kind == 'b') return "Boolean";
    if (kind == 'f') return "Double";
    if (size == 8) return kind == 'u' ? "ULong" : "Long";
    if (size == 4 && kind == 'u') return "Long";
    if (size == 1 || size == 2 || size == 4) return "Int";
    return NULL;
}
// The Kotlin type of a snapshot field's element.
static const char *kt_elem_type(Rec *t) {
    static char buf[4][128];
    static int k;
    char *b = buf[k++ % 4];
    if (t->record) snprintf(b, sizeof buf[0], "%sSnap", t->name);
    else {
        const char *s = kt_scalar_type(t->kind, t->size);
        if (!s) die("no Kotlin type for kind %c size %ld", t->kind, t->size);
        snprintf(b, sizeof buf[0], "%s", s);
    }
    return b;
}
// The type of a whole snapshot field. An array is a List and never a KotlinArray:
// `data class` builds its equals() out of the members' own, and an Array's is
// identity - which would make two snapshots of the same bytes unequal and quietly
// break the one property a snapshot exists to have.
static const char *kt_field_type(Rec *r, Field *f) {
    static char buf[4][160];
    static int k;
    char *b = buf[k++ % 4];
    if (f->width) return f->kind == 'b' ? "Boolean" : "Int";
    Rec *t = &recs[f->type];
    if (t->kind == 'p') kt_no_pointer(r, f);
    if (t->charlike && f->nd == 1) return "String";
    if (f->nd == 1) { snprintf(b, sizeof buf[0], "List<%s>", kt_elem_type(t)); return b; }
    return kt_elem_type(t);
}

// One scalar read out of the buffer, as the snapshot's own (widened) type. Every
// form is parenthesised: `and`, `shl` and `ushr` are infix functions and bind
// looser than the calls around them.
static const char *kt_read(char kind, long size, const char *base, const char *off) {
    static char buf[4][512];
    static int k;
    char *b = buf[k++ % 4];
    if (kind == 'b') snprintf(b, sizeof buf[0], "(%s.get(%s).toInt() != 0)", base, off);
    else if (kind == 'f') snprintf(b, sizeof buf[0], size == 4 ? "%s.getFloat(%s).toDouble()" : "%s.getDouble(%s)", base, off);
    else if (size == 1) snprintf(b, sizeof buf[0], kind == 'u' ? "(%s.get(%s).toInt() and 0xFF)" : "%s.get(%s).toInt()", base, off);
    else if (size == 2) snprintf(b, sizeof buf[0], kind == 'u' ? "(%s.getShort(%s).toInt() and 0xFFFF)" : "%s.getShort(%s).toInt()", base, off);
    else if (size == 4) snprintf(b, sizeof buf[0], kind == 'u' ? "(%s.getInt(%s).toLong() and 0xFFFFFFFFL)" : "%s.getInt(%s)", base, off);
    else if (size == 8) snprintf(b, sizeof buf[0], kind == 'u' ? "%s.getLong(%s).toULong()" : "%s.getLong(%s)", base, off);
    else die("no Kotlin load for kind %c size %ld", kind, size);
    return b;
}
// One scalar stored back. `val` is an expression of the snapshot's type, and
// every toByte/toShort/toInt here truncates, which is Swift's truncatingIfNeeded.
static void kt_store(Buf *kt, char kind, long size, const char *base, const char *off, const char *val) {
    if (kind == 'b') { bprintf(kt, "%s.put(%s, (if (%s) 1 else 0).toByte())", base, off, val); return; }
    if (kind == 'f') { bprintf(kt, size == 4 ? "%s.putFloat(%s, (%s).toFloat())" : "%s.putDouble(%s, %s)", base, off, val); return; }
    switch (size) {
    case 1: bprintf(kt, "%s.put(%s, (%s).toByte())", base, off, val); return;
    case 2: bprintf(kt, "%s.putShort(%s, (%s).toShort())", base, off, val); return;
    case 4: bprintf(kt, "%s.putInt(%s, (%s).toInt())", base, off, val); return;
    case 8: bprintf(kt, kind == 'u' ? "%s.putLong(%s, (%s).toLong())" : "%s.putLong(%s, %s)", base, off, val); return;
    }
    die("no Kotlin store for kind %c size %ld", kind, size);
}

// The window a bitfield lives in, read as a 32-bit Int. Int is exactly the width
// Swift's UInt32 window is, so the shifts below are the same arithmetic; `ushr`
// is what makes it unsigned where Kotlin has no unsigned Int shift.
static int kt_window_bytes(Field *f) {
    int bytes = (f->lo + f->width + 7) / 8;
    if (bytes == 3) bytes = 4;
    if (bytes > 4) die("bitfield %s spans %d bytes: Kotlin reads a bitfield window as an Int", f->name, bytes);
    return bytes;
}
static const char *kt_window(Field *f, const char *base) {
    static char buf[4][300];
    static int k;
    char *b = buf[k++ % 4];
    const int bytes = kt_window_bytes(f);
    if (bytes == 1) snprintf(b, sizeof buf[0], "(%s.get(p + %ld).toInt() and 0xFF)", base, f->off);
    else if (bytes == 2) snprintf(b, sizeof buf[0], "(%s.getShort(p + %ld).toInt() and 0xFFFF)", base, f->off);
    else snprintf(b, sizeof buf[0], "%s.getInt(p + %ld)", base, f->off);
    return b;
}
// A bitfield read out of that window, as Int or Boolean.
static const char *kt_bits_read(Field *f, const char *base) {
    static char buf[4][600];
    static int k;
    char *b = buf[k++ % 4];
    const int up = 32 - f->lo - f->width, down = 32 - f->width;
    if (f->kind == 'i')
        snprintf(b, sizeof buf[0], "((%s shl %d) shr %d)", kt_window(f, base), up, down);
    else if (f->kind == 'b')
        snprintf(b, sizeof buf[0], "(((%s shl %d) ushr %d) != 0)", kt_window(f, base), up, down);
    else
        snprintf(b, sizeof buf[0], "((%s shl %d) ushr %d)", kt_window(f, base), up, down);
    return b;
}

// A count is read into a Long whatever width it has, checked there, and only
// then narrowed for a loop bound: the check is what makes the narrowing safe,
// and a count too wide for an Int is refused by the same line that refuses a
// negative one rather than wrapping into a plausible small number.
static void emit_kotlin_snapshot(Buf *kt, Rec *r, int *strings) {
    int nprops = 0;
    for (int j = 0; j < r->nf; j++) if (!is_count_field(r->name, r->f[j].name)) nprops++;
    if (!nprops) die("%s has nothing to copy: every field of it is a --count", r->name);

    bprintf(kt, "\n/** %s (c struct), copied out of the kernel's storage. */\n", r->name);
    bprintf(kt, "data class %sSnap(\n", r->name);
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(kt, "    val %s: %s,\n", kt_name(f->name), kt_field_type(r, f));
    }
    bprintf(kt, ") {\n    companion object {\n");
    bprintf(kt, "        /** sizeof(%s) under this build: what a host allocates to hand C one. */\n", r->name);
    bprintf(kt, "        const val C_SIZE = %ld\n    }\n}\n", r->size);

    bprintf(kt, "fun read%s(buf: ByteBuffer, p: Int): %sSnap {\n", r->name, r->name);
    bprintf(kt, "    val sgb = sgLE(buf)\n");
    // Counts first: every one is checked before anything is read with it.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        Count *c = f->width ? NULL : count_for(r->name, f->name);
        if (!c) continue;
        if (recs[f->type].kind == 'p') kt_no_pointer(r, f);
        Field *cf = field_named(r, c->count);
        char off[32];
        snprintf(off, sizeof off, "p + %ld", cf->off);
        bprintf(kt, "    val n_%s = (%s).toLong()\n", f->name, kt_read(recs[cf->type].kind, recs[cf->type].size, "sgb", off));
        bprintf(kt, "    if (n_%s < 0L || n_%s > %ldL) throw SGLayoutException.Count(\"%s.%s\", n_%s, %ldL)\n",
                f->name, f->name, f->dims[0], r->name, f->name, f->name, f->dims[0]);
    }
    // Then the arrays, each bounded by a count that has been checked.
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (f->width || is_count_field(r->name, f->name)) continue;
        Rec *t = &recs[f->type];
        if (t->kind == 'p') kt_no_pointer(r, f);
        if (f->nd != 1 || t->charlike) continue;
        char n[64];
        if (count_for(r->name, f->name)) snprintf(n, sizeof n, "n_%s.toInt()", f->name); else snprintf(n, sizeof n, "%ld", f->dims[0]);
        bprintf(kt, "    val %s = ArrayList<%s>(%s)\n", kt_local(f->name), kt_elem_type(t), n);
        char off[128];
        if (t->size == 1) snprintf(off, sizeof off, "p + %ld + i", f->off);
        else snprintf(off, sizeof off, "p + %ld + i * %ld", f->off, t->size);
        bprintf(kt, "    for (i in 0 until %s) %s.add(", n, kt_local(f->name));
        if (t->record) bprintf(kt, "read%s(sgb, %s)", t->name, off);
        else bprintf(kt, "%s", kt_read(t->kind, t->size, "sgb", off));
        bprintf(kt, ")\n");
    }
    bprintf(kt, "    return %sSnap(", r->name);
    for (int j = 0, first = 1; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        bprintf(kt, "%s%s = ", first ? "" : ", ", kt_name(f->name));
        first = 0;
        if (f->width) { bprintf(kt, "%s", kt_bits_read(f, "sgb")); continue; }
        Rec *t = &recs[f->type];
        char off[64];
        snprintf(off, sizeof off, "p + %ld", f->off);
        if (t->charlike && f->nd == 1) {
            if (count_for(r->name, f->name)) { *strings |= STR_UTF8_GET; bprintf(kt, "sgUTF8(sgb, p + %ld, n_%s.toInt())", f->off, f->name); }
            else { *strings |= STR_CSTR_GET; bprintf(kt, "sgCStr(sgb, p + %ld, %ld)", f->off, f->dims[0]); }
        } else if (f->nd == 1) {
            bprintf(kt, "%s", kt_local(f->name));
        } else if (t->record) {
            bprintf(kt, "read%s(sgb, p + %ld)", t->name, f->off);
        } else {
            bprintf(kt, "%s", kt_read(t->kind, t->size, "sgb", off));
        }
    }
    bprintf(kt, ")\n}\n");
}

static void emit_kotlin_writer(Buf *kt, Rec *r, int *strings) {
    bprintf(kt, "fun write%s(buf: ByteBuffer, p: Int, s: %sSnap) {\n", r->name, r->name);
    bprintf(kt, "    val sgb = sgLE(buf)\n");
    for (int j = 0; j < r->nf; j++) {
        Field *f = &r->f[j];
        if (is_count_field(r->name, f->name)) continue;
        char name[300];
        snprintf(name, sizeof name, "s.%s", kt_name(f->name));
        if (f->width) {
            const unsigned mask = (unsigned)((((1ull << f->width) - 1) << f->lo) & 0xffffffffull);
            const int bytes = kt_window_bytes(f);
            char val[400];
            if (f->kind == 'b') snprintf(val, sizeof val, "(if (%s) 1 else 0)", name);
            else snprintf(val, sizeof val, "%s", name);
            // The mask is spelled 0x%08x.toInt() so one form covers both halves
            // of the range: below 0x80000000 it is an Int literal and toInt() is
            // the identity, above it Kotlin reads a Long literal and toInt()
            // takes the low 32 bits, which is the window.
            bprintf(kt, "    sgb.put%s(p + %ld, (((%s and 0x%08x.toInt().inv()) or ((%s shl %d) and 0x%08x.toInt()))).to%s())\n",
                    bytes == 1 ? "" : bytes == 2 ? "Short" : "Int", f->off,
                    kt_window(f, "sgb"), mask, val, f->lo, mask,
                    bytes == 1 ? "Byte" : bytes == 2 ? "Short" : "Int");
            continue;
        }
        Rec *t = &recs[f->type];
        if (t->kind == 'p') kt_no_pointer(r, f);
        Count *c = count_for(r->name, f->name);
        Field *cf = c ? field_named(r, c->count) : NULL;
        char coff[32] = "0";
        if (cf) snprintf(coff, sizeof coff, "p + %ld", cf->off);
        if (t->charlike && f->nd == 1) {
            if (c) {
                *strings |= STR_UTF8_SET;
                char n[500];
                snprintf(n, sizeof n, "sgUTF8Set(sgb, p + %ld, %ld, %s, \"%s.%s\")", f->off, f->dims[0], name, r->name, f->name);
                bprintf(kt, "    ");
                kt_store(kt, recs[cf->type].kind, recs[cf->type].size, "sgb", coff, n);
                bprintf(kt, "\n");
            } else {
                *strings |= STR_ACCESSORS;
                bprintf(kt, "    sgCStrSet(sgb, p + %ld, %ld, %s, \"%s.%s\")\n", f->off, f->dims[0], name, r->name, f->name);
            }
            continue;
        }
        if (f->nd == 1) {
            // `name` is the Kotlin expression for the field (up to the 300 bytes
            // declared above); this holds it plus ".size", sized off that buffer
            // rather than guessed, because gcc's -Wformat-truncation reads the
            // declared size and a smaller one is an error under -Werror.
            char n[sizeof name + 8];
            if (c) {
                bprintf(kt, "    if (%s.size > %ld) throw SGLayoutException.TooLong(\"%s.%s\", %s.size.toLong(), %ldL)\n",
                        name, f->dims[0], r->name, f->name, name, f->dims[0]);
                snprintf(n, sizeof n, "%s.size", name);
                bprintf(kt, "    ");
                kt_store(kt, recs[cf->type].kind, recs[cf->type].size, "sgb", coff, n);
                bprintf(kt, "\n");
            } else {
                bprintf(kt, "    if (%s.size != %ld) throw SGLayoutException.TooLong(\"%s.%s\", %s.size.toLong(), %ldL)\n",
                        name, f->dims[0], r->name, f->name, name, f->dims[0]);
                snprintf(n, sizeof n, "%ld", f->dims[0]);
            }
            char off[128];
            if (t->size == 1) snprintf(off, sizeof off, "p + %ld + i", f->off);
            else snprintf(off, sizeof off, "p + %ld + i * %ld", f->off, t->size);
            bprintf(kt, "    for (i in 0 until %s) ", n);
            if (t->record) bprintf(kt, "write%s(sgb, %s, %s[i])", t->name, off, name);
            else {
                char el[400];
                snprintf(el, sizeof el, "%s[i]", name);
                kt_store(kt, t->kind, t->size, "sgb", off, el);
            }
            bprintf(kt, "\n");
            continue;
        }
        char off[32];
        snprintf(off, sizeof off, "p + %ld", f->off);
        if (t->record) { bprintf(kt, "    write%s(sgb, p + %ld, %s)\n", t->name, f->off, name); continue; }
        bprintf(kt, "    ");
        kt_store(kt, t->kind, t->size, "sgb", off, name);
        bprintf(kt, "\n");
    }
    bprintf(kt, "}\n");
}

void sg_kotlin_module(Buf *kt, int *strings) {
    if (nconsts) bprintf(kt, "\n// constants\n");
    for (int p = 0; p < nprefixes; p++)
        for (int m = 1; m >= 0; m--)
            for (int j = 0; j < nconsts; j++)
                if (consts[j].prefix == p && consts[j].macro == m)
                    bprintf(kt, "const val %s = %lld\n", consts[j].name, consts[j].value);
    for (int i = 0; i < nrecs; i++) if (recs[i].snap) emit_kotlin_snapshot(kt, &recs[i], strings);
    for (int i = 0; i < nrecs; i++) if (recs[i].writer) emit_kotlin_writer(kt, &recs[i], strings);
    // The data class and function names only: `val` is also how a property is
    // declared, and two records are free to have a field of one name. Constants
    // cannot collide with each other (add_const dedups by name).
    check_unique_names(kt, "data class ", "(");
    check_unique_names(kt, "fun ", "(");
}

void sg_kotlin_write(const char *path, const Buf *kt, int strings, unsigned hash) {
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write %s", path);
    emit_banner(fp);
    fprintf(fp, "// target: %s\n\n", target);
    fprintf(fp, "package %s\n\nimport java.nio.ByteBuffer\nimport java.nio.ByteOrder\n\n", kotlin_package);
    fprintf(fp,
        "/**\n"
        " * The layout THESE readers were generated for. A host compares it with the hash\n"
        " * the library it links was stamped with (-DSG_LAYOUT_HASH, fio_layout_hash), so a\n"
        " * stale binding or a stale .so is a refusal at startup and never a wrong offset.\n"
        " */\n"
        "val SG_LAYOUT_HASH: UInt = 0x%08xu\n\n", hash);
    fputs("/**\n"
          " * The handshake itself, generated rather than hand-written: a host passes what\n"
          " * JNI read out of the library and has nothing else to get right. It THROWS,\n"
          " * because a wrong offset is not a degraded read a caller can fall back from -\n"
          " * the fields it returns are the bytes of other fields, and every rule computed\n"
          " * downstream is then computed from them.\n"
          " */\n"
          "fun sgCheckLayout(libraryHash: UInt) {\n"
          "    if (libraryHash == SG_LAYOUT_HASH) return\n"
          "    throw IllegalStateException(\n"
          "        \"The kernel and its generated Kotlin bindings are not a pair.\\n\" +\n"
          "        \"  library  (the .so JNI loaded): 0x\" + libraryHash.toString(16) + \"\\n\" +\n"
          "        \"  bindings (sdk/kotlin/gen): 0x\" + SG_LAYOUT_HASH.toString(16) + \"\\n\" +\n"
          "        \"Rebuild both from this tree: bash tools/structgen/gen.sh\"\n"
          "    )\n"
          "}\n\n", fp);
    fputs("/**\n"
          " * Why a generated reader or writer refused. Every one of them names the field\n"
          " * and the value: a payload that does not read whole is not read at all.\n"
          " *\n"
          " * There is no null case, and that is the crossing rather than an omission: a\n"
          " * pointer field is refused when the module is GENERATED, because a ByteBuffer\n"
          " * holds no way to follow an address (tools/structgen/sg_kotlin.c).\n"
          " */\n"
          "sealed class SGLayoutException(message: String) : RuntimeException(message) {\n"
          "    /** A count outside 0..capacity - a struct that does not describe itself. */\n"
          "    class Count(val field: String, val got: Long, val capacity: Long) :\n"
          "        SGLayoutException(\"$field: a count of $got is outside 0..$capacity\")\n"
          "    /** A value with more elements or bytes than the field holds. */\n"
          "    class TooLong(val field: String, val got: Long, val capacity: Long) :\n"
          "        SGLayoutException(\"$field: $got does not fit in $capacity\")\n"
          "}\n\n", fp);
    fputs("// THE JVM READS BIG-ENDIAN BY DEFAULT and every target this kernel is built for\n"
          "// is little-endian, so a buffer that arrives in the JVM's default order would\n"
          "// read every multi-byte field byte-reversed - silently, and plausibly, for a\n"
          "// one-byte field. Rather than demand the caller remember, each reader asks for\n"
          "// the order it needs: already little-endian is the identity, and anything else\n"
          "// gets a duplicate, which shares the bytes and not the position or the order.\n"
          "// Byte order is NOT part of the layout hash - the hash is offsets and sizes -\n"
          "// which is the other reason it is settled here and not left to a host.\n"
          "private fun sgLE(b: ByteBuffer): ByteBuffer =\n"
          "    if (b.order() == ByteOrder.LITTLE_ENDIAN) b else b.duplicate().order(ByteOrder.LITTLE_ENDIAN)\n", fp);
    if (strings & STR_UTF8_GET) fputs(
        "\nprivate fun sgUTF8(b: ByteBuffer, o: Int, n: Int): String {\n"
        "    val a = ByteArray(n)\n"
        "    for (i in 0 until n) a[i] = b.get(o + i)\n"
        "    return String(a, Charsets.UTF_8)\n"
        "}\n", fp);
    if (strings & STR_CSTR_GET) fputs(
        "\nprivate fun sgCStr(b: ByteBuffer, o: Int, n: Int): String {\n"
        "    var e = 0\n"
        "    while (e < n && b.get(o + e).toInt() != 0) e++\n"
        "    val a = ByteArray(e)\n"
        "    for (i in 0 until e) a[i] = b.get(o + i)\n"
        "    return String(a, Charsets.UTF_8)\n"
        "}\n", fp);
    if (strings & STR_UTF8_SET) fputs(
        "\n// A counted char array: its bytes, a NUL when there is room; returns the count.\n"
        "private fun sgUTF8Set(b: ByteBuffer, o: Int, n: Int, s: String, what: String): Int {\n"
        "    val a = s.toByteArray(Charsets.UTF_8)\n"
        "    if (a.size > n) throw SGLayoutException.TooLong(what, a.size.toLong(), n.toLong())\n"
        "    for (i in a.indices) b.put(o + i, a[i])\n"
        "    if (a.size < n) b.put(o + a.size, 0.toByte())\n"
        "    return a.size\n"
        "}\n", fp);
    if (strings & STR_ACCESSORS) fputs(
        "\n// A NUL-terminated char[N]: at most N-1 bytes, NUL-padded to N.\n"
        "private fun sgCStrSet(b: ByteBuffer, o: Int, n: Int, s: String, what: String) {\n"
        "    val a = s.toByteArray(Charsets.UTF_8)\n"
        "    if (a.size > n - 1) throw SGLayoutException.TooLong(what, a.size.toLong(), (n - 1).toLong())\n"
        "    for (i in a.indices) b.put(o + i, a[i])\n"
        "    for (i in a.size until n) b.put(o + i, 0.toByte())\n"
        "}\n", fp);
    fwrite(kt->s, 1, kt->n, fp);
    if (fclose(fp)) die("cannot write %s", path);
}
