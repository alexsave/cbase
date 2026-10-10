// The exports of one link, typed.
//
// A wasm module's exports are untyped where a host meets them: `instance.exports`
// is a bag of functions that take and return numbers, so the interface a host
// casts it to is whatever somebody typed by hand, and a C signature that changes
// underneath it still compiles and fails at run time. This writes that interface
// from the two places the facts already live.
//
//   WHICH functions   the link line. --link takes the linker flags of ONE link
//                     verbatim - the same `-Wl,--export=NAME` tokens the build
//                     hands the linker, read out of the same build variables -
//                     so the list of names is not written down a second time.
//                     A function that carries export_name is exported by every
//                     link of its object, and is found by that attribute.
//   WHAT they take    the C definition of each one, in the --source files, under
//                     the build's flags: the parameter names are the
//                     definition's, and each type is the wasm value type the C
//                     type compiles to (sg_clang.c valtype).
//
// ONE INTERFACE PER LINK, not one with optional members. Two links of the same
// objects export different sets - a server module and the smaller one a browser
// downloads - and `f?()` says "this may be missing" of a function that, for any
// given module, is known at build time to be there or not. A run takes several
// --link groups because those links share their sources and their flags, and
// parsing them is the cost.
//
// AND THE LINKER HAS THE LAST WORD. The interface is derived from the build's
// variables, the module from the build's recipe, and nothing makes a recipe use
// the variables this run was given. --check-wasm reads the built module's own
// type, import, function and export sections and refuses any difference, in
// either direction: an export the interface does not have, a member the module
// does not export, or one whose wasm signature is not the one its C definition
// compiles to. That is also what checks this file's idea of the C ABI against
// the compiler's.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "structgen.h"
#include "sg_args.h"
#include "sg_clang.h"
#include "sg_exports.h"
#include "sg_model.h"

// ---- what the linker makes itself ------------------------------------------------
// Symbols wasm-ld synthesises, which a link line can export and no C file
// defines. kind: g a global, f a function (with its wasm signature and how
// TypeScript spells it).
static const struct { const char *name; char kind; const char *sig, *ts; } LINKER_MADE[] = {
    { "__stack_pointer", 'g', NULL, NULL }, { "__tls_base", 'g', NULL, NULL },
    { "__tls_size", 'g', NULL, NULL },      { "__tls_align", 'g', NULL, NULL },
    { "__heap_base", 'g', NULL, NULL },     { "__heap_end", 'g', NULL, NULL },
    { "__data_end", 'g', NULL, NULL },      { "__global_base", 'g', NULL, NULL },
    { "__wasm_call_ctors", 'f', ">", "(): void" },
    { "__wasm_init_tls", 'f', "i>", "(tlsBlock: number): void" },
};
#define N_LINKER_MADE ((int)(sizeof LINKER_MADE / sizeof *LINKER_MADE))
static int linker_made(const char *name) {
    for (int i = 0; i < N_LINKER_MADE; i++) if (!strcmp(LINKER_MADE[i].name, name)) return i;
    return -1;
}

// ---- one link's exports -----------------------------------------------------------
// kind: f a function, g a global, m the memory, t the function table.
typedef struct { char *name; char kind; const Fn *fn; int made; int optional; } Member;
typedef struct { Member *m; int n, cap; int imports_memory; } Members;
static Members mem[MAXLINK];

static Member *member_named(Members *ms, const char *name) {
    for (int i = 0; i < ms->n; i++) if (!strcmp(ms->m[i].name, name)) return &ms->m[i];
    return NULL;
}
static void add_member(Members *ms, const char *name, char kind, int optional) {
    Member *old = member_named(ms, name);
    if (old) { old->optional &= optional; return; }   // a list may name an export twice; the linker does not mind
    GROW(ms->m, ms->n, ms->cap);
    ms->m[ms->n++] = (Member){ .name = xstrdup(name), .kind = kind, .made = -1, .optional = optional };
}

// One linker option, however it reached the command line.
static void link_option(Members *ms, const char *link_name, const char *opt) {
    if (!strncmp(opt, "--export=", 9)) add_member(ms, opt + 9, 'f', 0);
    else if (!strncmp(opt, "--export-if-defined=", 20)) add_member(ms, opt + 20, 'f', 1);
    else if (!strcmp(opt, "--export-memory")) add_member(ms, "memory", 'm', 0);
    else if (!strncmp(opt, "--export-memory=", 16)) add_member(ms, opt + 16, 'm', 0);
    else if (!strcmp(opt, "--export-table")) add_member(ms, "__indirect_function_table", 't', 0);
    else if (!strcmp(opt, "--import-memory") || !strncmp(opt, "--import-memory=", 16)) ms->imports_memory = 1;
    else if (!strcmp(opt, "--export-all") || !strcmp(opt, "--export-dynamic") || !strcmp(opt, "-export-dynamic"))
        die("--link %s: %s exports whatever the objects happen to define, which no list can be read out of", link_name, opt);
}

// `-Wl,a,b` is options a and b; anything else is a compiler-driver flag the
// linker never sees as an export, unless it is itself a bare linker option.
static void read_link(Members *ms, const Link *l) {
    char *fl = xstrdup(l->flags);
    for (char *tok = strtok(fl, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
        if (strncmp(tok, "-Wl,", 4)) { link_option(ms, l->name, tok); continue; }
        for (char *opt = tok + 4, *comma; opt; opt = comma ? comma + 1 : NULL) {
            if ((comma = strchr(opt, ','))) *comma = 0;
            link_option(ms, l->name, opt);
        }
    }
    free(fl);
    // THE LINKER EXPORTS THE MEMORY WITHOUT BEING ASKED, as `memory`, unless the
    // module imports it instead - so a line that says nothing about the memory
    // has still said it is exported. --export-memory only makes that explicit
    // (and is how a module that imports its memory exports it as well).
    int has = 0;
    for (int i = 0; i < ms->n; i++) has |= ms->m[i].kind == 'm';
    if (!has && !ms->imports_memory) add_member(ms, "memory", 'm', 0);
}

static int by_name(const void *a, const void *b) { return strcmp(((const Member *)a)->name, ((const Member *)b)->name); }

// Each listed name against what the sources define, then every export_name
// definition on top, and the lot in name order: the order a link line happens
// to list its exports in is not a fact about the module.
static void resolve(Members *ms, const Link *l) {
    int w = 0;
    for (int i = 0; i < ms->n; i++) {
        Member *m = &ms->m[i];
        if (m->kind == 'f') {
            if ((m->made = linker_made(m->name)) >= 0) m->kind = LINKER_MADE[m->made].kind;
            else {
                for (int j = 0; j < nfns && !m->fn; j++) if (!strcmp(fns[j].name, m->name)) m->fn = &fns[j];
                if (!m->fn && m->optional) { free(m->name); continue; }
                if (!m->fn) die("--link %s exports %s, and no --source defines a function of that name with external linkage", l->name, m->name);
                if (strcmp(m->fn->exported, m->name)) { free(m->name); m->name = xstrdup(m->fn->exported); }
            }
        }
        ms->m[w++] = *m;
    }
    ms->n = w;
    for (int j = 0; j < nfns; j++) if (fns[j].attr && !member_named(ms, fns[j].exported)) {
        add_member(ms, fns[j].exported, 'f', 0);
        ms->m[ms->n - 1].fn = &fns[j];
    }
    qsort(ms->m, (size_t)ms->n, sizeof *ms->m, by_name);
    for (int i = 1; i < ms->n; i++)
        if (!strcmp(ms->m[i - 1].name, ms->m[i].name)) die("--link %s: two exports are named %s", l->name, ms->m[i].name);
}

// ---- signatures ---------------------------------------------------------------------
// "ii>i": the parameters' value types, '>', the result's (nothing for void).
static void fn_sig(const Fn *f, char *out) {
    int k = 0;
    for (int i = 0; i < f->np; i++) out[k++] = f->pt[i];
    out[k++] = '>';
    if (f->ret) out[k++] = f->ret;
    out[k] = 0;
}
static const char *member_sig(const Member *m, char *buf) {
    if (m->made >= 0) return LINKER_MADE[m->made].sig;
    fn_sig(m->fn, buf);
    return buf;
}
static const char *val_name(char v) {
    return v == 'i' ? "i32" : v == 'I' ? "i64" : v == 'f' ? "f32" : v == 'F' ? "f64" : "?";
}
// The same signature for a person: "(i32, i32) -> i32".
static void sig_words(Buf *b, const char *sig) {
    const char *gt = strchr(sig, '>');
    bprintf(b, "(");
    for (const char *p = sig; p < gt; p++) bprintf(b, "%s%s", p > sig ? ", " : "", val_name(*p));
    bprintf(b, ") -> %s", gt[1] ? val_name(gt[1]) : "void");
}

// ---- the TypeScript interface -------------------------------------------------------
// An i64 is a bigint to JS and everything else is a number: an i32 arrives
// signed whatever C called it, which the banner says once.
static const char *ts_type(char v) { return v == 'I' ? "bigint" : "number"; }

// A parameter may not be named after a word TypeScript reserves, and a name
// that is one in C is one a C author is free to pick (`in`, `new`, `delete`).
static int ts_reserved(const char *s) {
    static const char *W[] = { "arguments", "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default",
        "delete", "do", "else", "enum", "eval", "export", "extends", "false", "finally", "for", "function", "if", "implements",
        "import", "in", "instanceof", "interface", "let", "new", "null", "package", "private", "protected", "public", "return",
        "static", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield" };
    for (size_t i = 0; i < sizeof W / sizeof *W; i++) if (!strcmp(W[i], s)) return 1;
    return 0;
}

static void emit_fn(Buf *b, const Member *m) {
    const Fn *f = m->fn;
    char names[SG_MAXPARAM][260];
    bprintf(b, "    %s(", m->name);
    for (int i = 0; i < f->np; i++) {
        // The definition's own name, in the camelCase a snapshot gives a field.
        // One it leaves unnamed is called by its position.
        if (f->pn[i]) { const char *c = camel(f->pn[i]); snprintf(names[i], sizeof names[i], "%s%s", c, ts_reserved(c) ? "_" : ""); }
        else snprintf(names[i], sizeof names[i], "arg%d", i);
        for (int j = 0; j < i; j++)
            if (!strcmp(names[j], names[i])) die("%s (%s): parameters %d and %d are both %s once named in camelCase", f->name, f->source, j + 1, i + 1, names[i]);
        bprintf(b, "%s%s: %s", i ? ", " : "", names[i], ts_type(f->pt[i]));
    }
    char sig[SG_MAXPARAM + 4];
    fn_sig(f, sig);
    bprintf(b, "): %s;   // ", f->ret ? ts_type(f->ret) : "void");
    sig_words(b, sig);
    bprintf(b, ", %s\n", f->source);
}

static void write_ts(const Link *l, const Members *ms) {
    int nfn = 0;
    for (int i = 0; i < ms->n; i++) nfn += ms->m[i].kind == 'f';
    Buf b = {0};
    bprintf(&b, "// GENERATED by tools/structgen - do not edit.\n// build: %.*s; link: %s (%d functions)\n",
        (int)(build_name_end - build), build, l->name, nfn);
    bprintf(&b, "//\n"
        "// What this link of the module exports. The names are the link line's, each\n"
        "// signature is its C definition's: the parameter names are the ones C gives\n"
        "// them, and a type is the wasm value type the C type compiles to. An i32 is a\n"
        "// `number` and arrives SIGNED whatever C called it (`>>> 0` reads an unsigned\n"
        "// one back); an i64 is a `bigint`. The comment on each line is that wasm\n"
        "// signature and the file that defines the function.\n"
        "export interface %s {\n", l->name);
    // What is not a C function first: the memory, the table, the linker's own.
    for (int i = 0; i < ms->n; i++) {
        const Member *m = &ms->m[i];
        if (m->kind == 'm') bprintf(&b, "    readonly %s: WebAssembly.Memory;\n", m->name);
        if (m->kind == 't') bprintf(&b, "    readonly %s: WebAssembly.Table;\n", m->name);
    }
    for (int i = 0; i < ms->n; i++) {
        const Member *m = &ms->m[i];
        if (m->kind == 'g') bprintf(&b, "    readonly %s: WebAssembly.Global;\n", m->name);
        if (m->kind == 'f' && m->made >= 0) {
            bprintf(&b, "    %s%s;   // ", m->name, LINKER_MADE[m->made].ts);
            sig_words(&b, LINKER_MADE[m->made].sig);
            bprintf(&b, ", made by the linker\n");
        }
    }
    for (int i = 0; i < ms->n; i++) if (ms->m[i].fn) emit_fn(&b, &ms->m[i]);
    bprintf(&b, "}\n");
    FILE *fp = fopen(l->out_ts, "w");
    if (!fp) die("cannot write %s", l->out_ts);
    fwrite(b.s, 1, b.n, fp);
    if (fclose(fp)) die("cannot write %s", l->out_ts);
    free(b.s);
}

// ---- the module the linker wrote (--check-wasm) -------------------------------------
//
// Four sections of the binary format and nothing else: the function types, the
// imports (an imported function takes an index before any defined one), the
// defined functions' type indices, and the exports. No names section, no code.
typedef struct { const unsigned char *p, *end; const char *path; } Rd;
static void truncated(Rd *r) { die("%s is not a whole wasm module (it ends inside a section)", r->path); }
static unsigned long long leb(Rd *r) {
    unsigned long long v = 0;
    for (int shift = 0; ; shift += 7) {
        if (r->p >= r->end || shift > 63) truncated(r);
        unsigned char c = *r->p++;
        v |= (unsigned long long)(c & 0x7f) << shift;
        if (!(c & 0x80)) return v;
    }
}
static unsigned char byte(Rd *r) { if (r->p >= r->end) truncated(r); return *r->p++; }
static char *wname(Rd *r) {
    unsigned long long n = leb(r);
    if (n > (unsigned long long)(r->end - r->p)) truncated(r);
    char *s = malloc((size_t)n + 1);
    if (!s) die("out of memory");
    memcpy(s, r->p, (size_t)n); s[n] = 0;
    r->p += n;
    return s;
}
static void limits(Rd *r) { unsigned long long fl = leb(r); leb(r); if (fl & 1) leb(r); }
static char val_of(unsigned char b) { return b == 0x7f ? 'i' : b == 0x7e ? 'I' : b == 0x7d ? 'f' : b == 0x7c ? 'F' : '?'; }

typedef struct { char *name; char kind; unsigned long long index; } WExport;
typedef struct {
    char **types; int ntypes;                  // each as "ii>i"
    unsigned long long *fn_type; int nfn;      // by function index, imports first
    WExport *ex; int nex;
} WModule;

static void wasm_read(const char *path, WModule *w) {
    FILE *fp = fopen(path, "rb");
    if (!fp) die("--check-wasm: cannot read %s (the module has to be built before it can be checked)", path);
    unsigned char *data = NULL; size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap && !(data = realloc(data, cap = cap * 2 + 65536))) die("out of memory");
        size_t k = fread(data + n, 1, cap - n, fp);
        if (!k) break;
        n += k;
    }
    fclose(fp);
    if (n < 8 || memcmp(data, "\0asm", 4)) die("%s is not a wasm module", path);
    Rd all = { data + 8, data + n, path };
    int capfn = 0, capex = 0;
    while (all.p < all.end) {
        unsigned char id = byte(&all);
        unsigned long long size = leb(&all);
        if (size > (unsigned long long)(all.end - all.p)) truncated(&all);
        Rd r = { all.p, all.p + size, path };
        all.p += size;
        if (id == 1) {
            w->ntypes = (int)leb(&r);
            w->types = calloc((size_t)w->ntypes + 1, sizeof *w->types);
            for (int i = 0; i < w->ntypes; i++) {
                if (byte(&r) != 0x60) die("%s: type %d is not a plain function type, which is all this reads", path, i);
                Buf sig = {0};
                for (unsigned long long k = leb(&r); k; k--) bprintf(&sig, "%c", val_of(byte(&r)));
                bprintf(&sig, ">");
                for (unsigned long long k = leb(&r); k; k--) bprintf(&sig, "%c", val_of(byte(&r)));
                w->types[i] = sig.s;
            }
        } else if (id == 2) {
            for (unsigned long long k = leb(&r); k; k--) {
                free(wname(&r)); free(wname(&r));
                switch (byte(&r)) {
                case 0: GROW(w->fn_type, w->nfn, capfn); w->fn_type[w->nfn++] = leb(&r); break;
                case 1: byte(&r); limits(&r); break;
                case 2: limits(&r); break;
                case 3: byte(&r); byte(&r); break;
                case 4: byte(&r); leb(&r); break;
                default: die("%s: an import of a kind this does not read", path);
                }
            }
        } else if (id == 3) {
            for (unsigned long long k = leb(&r); k; k--) { GROW(w->fn_type, w->nfn, capfn); w->fn_type[w->nfn++] = leb(&r); }
        } else if (id == 7) {
            for (unsigned long long k = leb(&r); k; k--) {
                GROW(w->ex, w->nex, capex);
                WExport *e = &w->ex[w->nex++];
                e->name = wname(&r);
                unsigned char kind = byte(&r);
                e->kind = kind == 0 ? 'f' : kind == 1 ? 't' : kind == 2 ? 'm' : kind == 3 ? 'g' : '?';
                e->index = leb(&r);
            }
        }
    }
    free(data);
}

static const char *kind_word(char k) {
    return k == 'f' ? "a function" : k == 'g' ? "a global" : k == 'm' ? "a memory" : k == 't' ? "a table" : "an export of an unknown kind";
}

static void check_wasm(const Link *l, const Members *ms) {
    WModule w = {0};
    wasm_read(l->check_wasm, &w);
    // EVERY difference, then the refusal: a list is fixed in one pass, and the
    // first of thirty tells nobody whether the other twenty-nine are the same bug.
    int bad = 0;
    for (int i = 0; i < ms->n; i++) {
        const Member *m = &ms->m[i];
        const WExport *e = NULL;
        for (int j = 0; j < w.nex && !e; j++) if (!strcmp(w.ex[j].name, m->name)) e = &w.ex[j];
        if (!e) { fprintf(stderr, "  %s: in %s, not exported by the module\n", m->name, l->name); bad++; continue; }
        if (e->kind != m->kind) {
            fprintf(stderr, "  %s: %s in %s, %s in the module\n", m->name, kind_word(m->kind), l->name, kind_word(e->kind));
            bad++; continue;
        }
        if (m->kind != 'f') continue;
        if (e->index >= (unsigned long long)w.nfn || w.fn_type[e->index] >= (unsigned long long)w.ntypes)
            die("%s: export %s names a function or a type the module does not have", l->check_wasm, m->name);
        char buf[SG_MAXPARAM + 4];
        const char *want = member_sig(m, buf), *got = w.types[w.fn_type[e->index]];
        if (strcmp(want, got)) {
            Buf a = {0}, b = {0};
            sig_words(&a, want); sig_words(&b, got);
            fprintf(stderr, "  %s: %s from its C definition%s%s, %s in the module\n", m->name, a.s,
                m->fn ? " in " : "", m->fn ? m->fn->source : "", b.s);
            free(a.s); free(b.s);
            bad++;
        }
    }
    for (int j = 0; j < w.nex; j++) {
        int known = 0;
        for (int i = 0; i < ms->n && !known; i++) known = !strcmp(ms->m[i].name, w.ex[j].name);
        if (!known) { fprintf(stderr, "  %s: exported by the module (%s), not in %s\n", w.ex[j].name, kind_word(w.ex[j].kind), l->name); bad++; }
    }
    if (bad) die("--check-wasm: %s and %s disagree about %d export%s (above)", l->name, l->check_wasm, bad, bad == 1 ? "" : "s");
    for (int i = 0; i < w.ntypes; i++) free(w.types[i]);
    for (int j = 0; j < w.nex; j++) free(w.ex[j].name);
    free(w.types); free(w.fn_type); free(w.ex);
}

void sg_exports_run(void) {
    if (strncmp(target, "wasm32", 6)) die("--link: exports are a wasm32 module's; --target %s has none to type", target);
    // Every name any link lists that C has to define, once.
    char **want = NULL; int nwant = 0, capwant = 0;
    for (int i = 0; i < nlinks; i++) {
        for (int j = 0; j < i; j++)
            if (!strcmp(links[i].name, links[j].name)) die("two --link groups are named %s", links[i].name);
        read_link(&mem[i], &links[i]);
        int named = 0;
        for (int k = 0; k < mem[i].n; k++) named |= mem[i].m[k].kind != 'm';
        if (!named) die("--link %s: the flags export nothing (no -Wl,--export=NAME among them)", links[i].name);
        for (int k = 0; k < mem[i].n; k++) {
            const Member *m = &mem[i].m[k];
            if (m->kind != 'f' || linker_made(m->name) >= 0) continue;
            int seen = 0;
            for (int q = 0; q < nwant && !seen; q++) seen = !strcmp(want[q], m->name);
            if (seen) continue;
            GROW(want, nwant, capwant);
            want[nwant++] = m->name;
        }
    }
    sg_load_exports(want, nwant);
    free(want);
    for (int i = 0; i < nlinks; i++) {
        resolve(&mem[i], &links[i]);
        if (links[i].out_ts) write_ts(&links[i], &mem[i]);
        if (links[i].check_wasm) check_wasm(&links[i], &mem[i]);
    }
}
