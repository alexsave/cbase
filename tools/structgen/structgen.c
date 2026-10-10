// structgen - C structs -> TypeScript accessors over wasm32 linear memory,
// Swift value snapshots over the same struct in native memory, and Kotlin value
// snapshots over the bytes of it that JNI hands the JVM.
//
// libclang parses the headers for ONE target under ONE build's layout flags;
// every offset, size, bitfield position and array stride comes from clang,
// never from this tool. Run it once per build (layouts differ between builds).
// It writes one TS module of accessors and computes that layout's hash, which
// the build passes to the C side (-DSG_LAYOUT_HASH=...) so a module can prove at
// load time which layout it was compiled with.
//
//   structgen --cwd DIR --header H... --root T... --build NAME=FLAGS
//             [--target TRIPLE] [--fields T=f1,f2,SIZE]... [--const PREFIX]...
//             [--snapshot T]... [--count T.f=c]... [--writer T]... [--snapshot-only]
//             [--ts OUT] [--swift OUT] [--kotlin OUT --kotlin-package PKG]
//             [--hash-ts OUT] [--print-hash]
//
//   structgen --cwd DIR --header H... --build NAME=FLAGS
//             (--const PREFIX | --names PREFIX)... --ts OUT
//
//   structgen --cwd DIR --source C... --build NAME=FLAGS
//             (--link LDFLAGS --exports-name IDENT [--exports-ts OUT] [--check-wasm M])...
//
// The first form is the tool as it has always been. The second is the same run
// with no --root: a module of constants and nothing else, for a host that
// needs a header's numbers and none of its structs. The third asks a different
// question - what a link of a wasm module exports - and is described at the end
// of this comment.
//
// --ts OUT        the accessors (no hash: a host that only checks the hash must
//                 not have to import the accessors, which read the raw struct)
// --swift OUT     the same --snapshot readers (and --writer writers) as Swift
//                 value types over a POINTER TO THE C STRUCT ITSELF. See the
//                 Swift section at the end of this comment.
// --kotlin OUT    the same readers again for a JVM host. See the Kotlin section
//                 at the end of this comment.
// --kotlin-package PKG
//                 the package that module declares. Required with --kotlin.
// --hash-ts OUT   a module holding only `export const LAYOUT_HASH = 0x...;`
// --print-hash    the same hash on stdout
// --target TRIPLE the target libclang parses for; wasm32 by default. A host that
//                 links the C natively (iOS) reads the layout ITS compiler makes,
//                 so its run passes its own triple and its own caps.
//
// --fields T=...  emit (and follow) only these fields of record T; a field that
//                 does not exist is an error. SIZE requests T_SIZE.
// --const PREFIX  emit every enum constant and object-like integer #define whose
//                 name starts with PREFIX; a prefix that matches nothing is an error.
// --names PREFIX  --const PREFIX, and the same constants once more as ONE table
//                 from value to name: `export const <PREFIX>NAMES`, each name
//                 with the prefix taken off (LOG_ -> LOG_NAMES[1] === "ATTACK").
//                 It is how a host prints a code without keeping a list of what
//                 the codes are called. Two constants of one value under the
//                 prefix are refused, since the table could name only one of
//                 them: give a narrower prefix. TypeScript only, and not part
//                 of the layout hash (the constants already are).
//
// --snapshot T    emit `interface T_Snap` and `readT(m, p): T_Snap`, which copies the
//                 record out of wasm memory into a plain object that holds no
//                 pointer into it (so it outlives the memory's next write), and the
//                 same for every record T reaches. Fields are named in camelCase;
//                 nested records become objects, arrays become arrays, char[N]
//                 becomes a string. The types are readonly: a snapshot is a value.
// --count T.f=c   in a snapshot, array T.f holds only its first T.c elements (a
//                 char[N] is then c bytes of UTF-8 rather than NUL-terminated). The
//                 count field itself is not copied (it is the array's length), and
//                 a count outside 0..N throws a RangeError rather than reading past
//                 the array.
// --writer T      emit `writeT(m, p, s)`, the inverse of readT: it writes a T_Snap back
//                 into wasm memory, and the same for every record T reaches, so the
//                 kernel can read a value the host holds (a board the host changed).
//                 T must be a --snapshot. An array writes its count from its length and
//                 a string its UTF-8 bytes; a value that does not fit - more elements
//                 than the array holds, more bytes than the string, an uncounted array
//                 of the wrong length - throws a RangeError rather than truncate.
// --snapshot-only the module holds the snapshot readers and the constants, not the
//                 per-field accessors.
//
// Pointers are read, never written. In wasm32 a pointer is a u32 offset into
// linear memory, so following one means reading that u32 and using it as the
// pointee's `p`. The TS never produces a pointer C will dereference.
//   accessors     `T *f` emits T_f_at (the address of the pointer itself) and
//                 X_f_ptr(m, p): the address it holds, 0 for NULL. When T is a
//                 record or a scalar, also X_f_deref_at(m, p, i): the address of
//                 element i, after checking the pointer is not NULL and element i
//                 lies inside wasm memory (a RangeError otherwise). void *, a
//                 function pointer, an incomplete type or a pointer to a pointer
//                 get the address getter only. There is never a setter, and a
//                 record only a pointer reaches gets its accessors like any other.
//   --snapshot    a pointer field is refused unless it has --count X.f=c; with
//                 one, the snapshot follows it and copies c elements (a char
//                 pointee becomes c bytes of UTF-8). NULL with a nonzero count, or
//                 elements past the end of memory, throw a RangeError; NULL with a
//                 count of 0 is an empty array.
//   --writer      a record that reaches a pointer field is refused.
//   layout hash   a pointer field hashes as a pointer, with its pointee's size and
//                 kind, and a record pointee's own fields (a cycle back to a record
//                 on the path hashes as how far back it points).
//
// ---- the Swift emitter (--swift) --------------------------------------------
//
// Same model, same counts, same refusals; a different host. A Swift host LINKS
// the kernel, so there is no linear memory to index into and no `Mem`: a reader
// takes the address of the C struct itself (`UnsafeRawPointer`) and copies the
// record out of it into a `Sendable` value type, so nothing Swift holds points
// into storage the kernel writes next. That is the same rule as the TS snapshots
// and as the resident-slot discipline the iOS app already keeps.
//
//   --swift implies --snapshot-only: there are no per-field accessors, because a
//   host that wants a field of a struct it can address wants the struct.
//   `T_Snap` is `struct TSnap: Sendable, Equatable`, `readT(p)` builds one, and
//   `writeT(p, s)` writes one back. Both THROW rather than read or write out of
//   range: a count outside 0..N, a NULL pointer with a count, a string or an
//   array too long for its field. A refusal names the field and the value, which
//   is what a returned nil cannot do, and the call sites are few.
//   Integers widen to Int (Int64/UInt64 at 8 bytes), floats to Double, a
//   `char[N]` to String, exactly as the TS side widens to number/bigint/string.
//   A field named after a Swift keyword is emitted in backticks.
//   The module carries `SG_LAYOUT_HASH`, this run's layout hash: a host compares
//   it with the hash the LIBRARY was stamped with, so a stale binding and a
//   stale library are a startup refusal instead of a wrong offset.
//
// A LOAD IS ALIGNED BY CONSTRUCTION, which is why the readers use `load` and not
// `loadUnaligned`: every address they read is a field of a C struct at the
// address C gave it, so it already satisfies that field's alignment.
//
// ---- the Kotlin emitter (--kotlin) -------------------------------------------
//
// Same model again, and the same LP64 offsets the Swift run reads, because the
// kernel is C99 and builds under the Android NDK unchanged. What differs is that
// the JVM cannot hold the kernel's address: the bytes have to be handed to it,
// which is what the `fio_*` bridge already does (a caller buffer, packed bytes
// out) and what JNI wants either way. So a reader takes a `java.nio.ByteBuffer`
// and a byte offset, and one emitted reader serves both a heap buffer over a
// ByteArray JNI filled and a direct buffer over storage the kernel owns.
//
//   `T_Snap` is `data class TSnap`, `readT(buf, p)` builds one and
//   `writeT(buf, p, s)` writes one back, both THROWING an SGLayoutException
//   rather than reading or writing out of range. An array is a `List` and never
//   a Kotlin `Array`, because a data class builds equals() out of its members'
//   and an Array's is identity.
//   A POINTER FIELD IS REFUSED: a ByteBuffer holds bytes and no way to follow an
//   address, so the C side has to flatten such a field before it crosses. That
//   is the one thing Kotlin will not read that Swift will.
//   Integers widen to the narrowest LOSSLESS Kotlin type - Int through int32,
//   Long for uint32 and int64, ULong for uint64 - which is not Swift's rule,
//   because Kotlin's Int is 32 bits and Swift's is 64.
//   The module carries SG_LAYOUT_HASH and the `sgCheckLayout` that compares it
//   with what JNI read out of the library, so the stale-library refusal is
//   generated rather than hand-written.
//
// ---- an exports run (--link) ---------------------------------------------------
//
// `instance.exports` is untyped, so the interface a host casts it to is typed
// by hand and a C signature can change underneath it. This run writes that
// interface from the link line and the C definitions; sg_exports.c says where
// each fact comes from and why it cannot be a second copy of one.
//
// --source C      a C file the module is linked from, parsed as its own
//                 translation unit under the build's flags. Give every file of
//                 the link: the ones that define no export are not read.
// --link LDFLAGS  one link's linker flags, verbatim and as ONE argument - the
//                 `-Wl,--export=NAME` tokens the build passes, plus whatever
//                 else is on that line. The memory is read off it too: the
//                 linker exports it as `memory` unless -Wl,--import-memory is
//                 there. The rest is ignored. Every name must be a function some --source
//                 defines with external linkage, or a symbol the linker makes
//                 itself (__stack_pointer, __wasm_init_tls, ...). A definition
//                 that carries export_name is exported without being listed.
//                 --export-all and --export-dynamic are refused: they export
//                 whatever is there, and there is no list to read.
//                 Opens a group; the three flags below belong to the --link
//                 they follow, and a run may hold several (links of the same
//                 sources and flags share one parse).
// --exports-name IDENT   what the interface is called. Required.
// --exports-ts OUT       `export interface IDENT { ... }`, one member per
//                 export in name order: the memory as WebAssembly.Memory, a
//                 linker global as WebAssembly.Global, and each function with
//                 its C parameter names in camelCase. Types are the wasm value
//                 types C compiles to - `number` for an i32, f32 or f64, `bigint`
//                 for an i64 - and a struct, a union, long double or a 128-bit
//                 integer by value is refused, because none of them crosses as
//                 one value. Unnamed parameters are arg0, arg1, ...
// --check-wasm M  read the built module M and refuse any difference from the
//                 interface: a missing export, an extra one, a different kind,
//                 or a function whose wasm signature is not the one its C
//                 definition gives. Every difference is listed before the
//                 refusal. This is what holds the link line the run was GIVEN
//                 to the link line the build USED.
//
// ---- the parts --------------------------------------------------------------
//
// One thing each, and structgen.h lists them. This file is the run itself: the
// request, the traversal, an emitter per output - plus the two helpers that
// belong to no one emitter because both of them use them (camel and the
// collision check). The rest of the plumbing, shared with tools/datagen, is in
// tools/sgcommon.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_clang.h"
#include "sg_exports.h"
#include "sg_hash.h"
#include "sg_kotlin.h"
#include "sg_model.h"
#include "sg_swift.h"
#include "sg_ts.h"

// snake_case -> camelCase, into a static buffer.
const char *camel(const char *s) {
    static char buf[8][256];
    static int k;
    char *b = buf[k++ % 8];
    size_t w = 0;
    for (int up = 0; *s && w + 1 < sizeof buf[0]; s++) {
        if (*s == '_' && w > 0) { up = 1; continue; }
        b[w++] = (char)(up && *s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        up = 0;
    }
    b[w] = 0;
    return b;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
void check_unique_names(const Buf *ts, const char *decl, const char *stop) {   // generated names are joined with '_' and could collide
    char **names = NULL; int n = 0, cap = 0;
    const size_t dl = strlen(decl);
    for (const char *p = ts->s; p && (p = strstr(p, decl)); ) {
        p += dl;
        size_t len = strcspn(p, stop);
        GROW(names, n, cap);
        names[n] = malloc(len + 1); memcpy(names[n], p, len); names[n++][len] = 0;
    }
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    for (int i = 1; i < n; i++) if (!strcmp(names[i - 1], names[i])) die("generated name %s is emitted twice", names[i]);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

int main(int argc, char **argv) {
    sg_args(argc, argv);        // the request
    // An exports run asks its own question of its own inputs (sg_exports.c),
    // and has no layout to hash.
    if (nlinks) { sg_exports_run(); return 0; }
    sg_load();                  // the headers, through libclang, into the model
    sg_check();                 // the model against the request

    // ---- the module body, and the layout hash beside it -----------------------
    // The TS module is built whatever is asked for: the hash takes the constants
    // in the order that emitter writes them, so it is not a per-output cost.
    Buf ts = {0};
    int strings = 0;
    sg_ts_module(&ts, &strings);
    unsigned hash = sg_layout_hash();

    // The Swift module: the same snapshots and writers, over the struct itself.
    Buf sw = {0};
    int swstrings = 0;
    if (out_swift) sg_swift_module(&sw, &swstrings);

    // ...and the Kotlin one: the same again, over the bytes of it.
    Buf kt = {0};
    int ktstrings = 0;
    if (out_kotlin) sg_kotlin_module(&kt, &ktstrings);

    if (out_ts) sg_ts_write(out_ts, &ts, strings);
    if (out_swift) sg_swift_write(out_swift, &sw, swstrings, hash);
    if (out_kotlin) sg_kotlin_write(out_kotlin, &kt, ktstrings, hash);
    if (out_hash_ts) sg_hash_write_ts(out_hash_ts, hash);
    if (print_hash) printf("0x%08x\n", hash);
    return 0;
}
