// The traversal: libclang parses the headers for ONE target under ONE build's
// layout flags, and every offset, size, bitfield position and array stride in
// the model comes from here. No other part of structgen includes clang-c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clang-c/Index.h>
#include "sgc_probe.h"
#include "structgen.h"
#include "sg_args.h"
#include "sg_clang.h"
#include "sg_model.h"

static char kind_of(CXType t) {
    switch (t.kind) {
    case CXType_Bool: return 'b';
    case CXType_Float: case CXType_Double: case CXType_LongDouble: return 'f';
    case CXType_Char_U: case CXType_UChar: case CXType_UShort: case CXType_UInt: case CXType_ULong:
    case CXType_ULongLong: case CXType_UInt128: return 'u';
    case CXType_Pointer: case CXType_BlockPointer: return 'p';
    case CXType_Enum:    // an enum is stored as its underlying integer type
        return kind_of(clang_getCanonicalType(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t))));
    default: return 'i';
    }
}

// `key` is the canonical type's spelling, used ONLY to recognise the same type
// reached twice within this run. It is never emitted and never hashed: libclang
// renders a canonical type differently across LLVM versions. Qualifiers are
// not part of it: `const Card *` points at the same Card as a Card field.
static int want(CXType t, const char *name, const char *expr) {
    t = clang_getUnqualifiedType(t);
    char *key = str(clang_getTypeSpelling(t));
    for (int i = 0; i < nrecs; i++) if (!strcmp(recs[i].key, key)) { free(key); return i; }
    GROW(recs, nrecs, caprecs);
    Rec *r = &recs[nrecs];
    memset(r, 0, sizeof *r);
    r->key = key; r->name = xstrdup(name); r->expr = xstrdup(expr); r->t = t;
    long long sz = clang_Type_getSizeOf(t);
    if (sz < 0) die("%s: no size (incomplete type?)", name);
    r->size = (long)sz;
    r->record = t.kind == CXType_Record;
    r->is_union = r->record && clang_getCursorKind(clang_getTypeDeclaration(t)) == CXCursor_UnionDecl;
    if (!r->record) r->kind = kind_of(t);
    r->pointee = -1;
    r->charlike = t.kind == CXType_Char_S || t.kind == CXType_Char_U;   // plain char; int8_t/uint8_t arrays are bytes
    return nrecs++;
}

static int is_ident(const char *s) {
    if (!(*s == '_' || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (; *s; s++) if (!(*s == '_' || (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    return 1;
}

// The name the HEADER gives a field's element type: the typedef or struct/union
// tag exactly as declared, or NULL for an unnamed record. This, not a rendering
// of the type, names the generated accessors (Card_get_suit): declared names are
// what the header author wrote, so every libclang reports them alike.
// `t` is the field's declared (sugared) type with its `nd` array levels still on.
static char *declared_name(CXType t, int nd) {
    for (;;) {
        if (t.kind == CXType_Elaborated) { t = clang_Type_getNamedType(t); continue; }
        if (nd > 0) {   // an array level, possibly behind a typedef of the array type
            if (t.kind == CXType_Typedef) { t = clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(t)); continue; }
            if (t.kind != CXType_ConstantArray) return NULL;
            t = clang_getArrayElementType(t); nd--;
            continue;
        }
        if (t.kind == CXType_Typedef) return str(clang_getCursorSpelling(clang_getTypeDeclaration(t)));
        if (t.kind == CXType_Record || t.kind == CXType_Enum) {
            CXCursor d = clang_getTypeDeclaration(t);
            return clang_Cursor_isAnonymous(d) ? NULL : str(clang_getCursorSpelling(d));
        }
        return NULL;
    }
}

// The pointee of a pointer field as the header declares it (sugar kept, so
// declared_name can name it). `t` is the field's declared type with its `nd`
// array levels still on; typedefs of the pointer or array type are looked through.
static CXType sugared_pointee(CXType t, int nd) {
    for (;;) {
        if (t.kind == CXType_Elaborated) { t = clang_Type_getNamedType(t); continue; }
        if (t.kind == CXType_Attributed) { t = clang_Type_getModifiedType(t); continue; }
        if (t.kind == CXType_Typedef) { t = clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(t)); continue; }
        if (nd > 0 && t.kind == CXType_ConstantArray) { t = clang_getArrayElementType(t); nd--; continue; }
        if (t.kind == CXType_Pointer) return clang_getPointeeType(t);
        return clang_getPointeeType(clang_getCanonicalType(t));
    }
}

// Resolves what pointer `pi` points to: a record or a scalar with a size, or
// nothing to follow (pointee -1). Runs after every record embedded in the roots
// has been walked, so a pointer never reorders the records a module emits.
// Returns whether it reached a type not seen before.
static int resolve_pointee(int pi) {
    Rec *p = &recs[pi];
    if (p->pointee_done || !p->ptr_rec) return 0;   // a pointer no field declares (a pointee of a pointer) is not followed
    p->pointee_done = 1;
    CXType pt = clang_getCanonicalType(clang_getPointeeType(p->t));
    int followable = pt.kind == CXType_Record || pt.kind == CXType_Enum || (pt.kind > CXType_Void && pt.kind <= CXType_LastBuiltin);
    if (!followable || clang_Type_getSizeOf(pt) <= 0) return 0;   // void, a function, an array, a pointer, an incomplete type
    char *id = declared_name(sugared_pointee(p->ptr_decl, p->ptr_nd), 0);
    Buf name = {0}, pexpr = {0};
    if (id && is_ident(id)) bprintf(&name, "%s", id); else bprintf(&name, "%s_%s", p->ptr_rec, p->ptr_field);
    bprintf(&pexpr, "*(%s)", p->ptr_expr);
    int before = nrecs, t = want(pt, name.s, pexpr.s);   // may move recs: p is stale from here
    recs[pi].pointee = t;
    free(id); free(name.s); free(pexpr.s);
    return nrecs != before;
}

typedef struct { int rec; long long base; } Walk;
static enum CXVisitorResult on_field(CXCursor c, CXClientData d) {
    Walk *w = d;
    long long bits = clang_Cursor_getOffsetOfField(c);
    if (bits < 0) die("%s: no offset for a field", recs[w->rec].name);
    bits += w->base;
    CXType ft = clang_getCanonicalType(clang_getCursorType(c));
    if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(ft))) {   // members reachable by name
        Walk inner = { w->rec, bits };
        clang_Type_visitFields(ft, on_field, &inner);
        return CXVisit_Continue;
    }
    Field f = { .name = str(clang_getCursorSpelling(c)), .off = (long)(bits / 8), .type = -1 };
    Spec *s = spec_for(recs[w->rec].name);
    if (s && !spec_has(s, f.name)) { free(f.name); return CXVisit_Continue; }
    if (clang_Cursor_isBitField(c)) {
        f.lo = (int)(bits % 8); f.width = clang_getFieldDeclBitWidth(c); f.kind = kind_of(ft);
        if (f.lo + f.width > 32) die("%s.%s: bitfields wider than a 32-bit window are not supported", recs[w->rec].name, f.name);
    } else {
        while (ft.kind == CXType_ConstantArray) {
            if (f.nd == 8) die("%s.%s: too many array dimensions", recs[w->rec].name, f.name);
            f.dims[f.nd++] = (long)clang_getArraySize(ft);
            ft = clang_getCanonicalType(clang_getArrayElementType(ft));
        }
        if (ft.kind == CXType_IncompleteArray || ft.kind == CXType_VariableArray)
            die("%s.%s: arrays without a constant size are not supported", recs[w->rec].name, f.name);
        char *id = declared_name(clang_getCursorType(c), f.nd);
        Buf name = {0}, expr = {0};
        if (id && is_ident(id)) bprintf(&name, "%s", id); else bprintf(&name, "%s_%s", recs[w->rec].name, f.name);
        bprintf(&expr, "(%s).%s", recs[w->rec].expr, f.name);
        for (int i = 0; i < f.nd; i++) bprintf(&expr, "[0]");
        f.type = want(ft, name.s, expr.s);
        Rec *pr = &recs[f.type];
        if (pr->kind == 'p' && !pr->ptr_rec) {   // followed later (resolve_pointee), as this field declares it
            pr->ptr_decl = clang_getCursorType(c); pr->ptr_nd = f.nd;
            pr->ptr_rec = xstrdup(recs[w->rec].name); pr->ptr_field = xstrdup(f.name); pr->ptr_expr = xstrdup(expr.s);
        }
        free(id); free(name.s); free(expr.s);
    }
    Rec *r = &recs[w->rec];
    GROW(r->f, r->nf, r->capf);
    r->f[r->nf++] = f;
    return CXVisit_Continue;
}

// ---- constants ----------------------------------------------------------------
static enum CXChildVisitResult on_macro(CXCursor c, CXCursor parent, CXClientData d) {
    (void)parent; (void)d;
    if (clang_getCursorKind(c) == CXCursor_MacroDefinition && !clang_Cursor_isMacroFunctionLike(c) && !clang_Cursor_isMacroBuiltin(c)) {
        char *name = str(clang_getCursorSpelling(c));
        add_const(name, 1, 0);
        free(name);
    }
    return CXChildVisit_Continue;
}
static enum CXChildVisitResult on_decl(CXCursor c, CXCursor parent, CXClientData d) {
    (void)parent; (void)d;
    enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_EnumConstantDecl) {
        char *name = str(clang_getCursorSpelling(c));
        int idx;
        if (sscanf(name, "__structgen_m%d", &idx) == 1) consts[idx].value = clang_getEnumConstantDeclValue(c);
        else add_const(name, 0, clang_getEnumConstantDeclValue(c));
        free(name);
    }
    if (k == CXCursor_EnumDecl || k == CXCursor_TypedefDecl || k == CXCursor_StructDecl || k == CXCursor_UnionDecl)
        return CXChildVisit_Recurse;
    if (k == CXCursor_VarDecl) {
        char *name = str(clang_getCursorSpelling(c));
        int i;
        if (sscanf(name, "__structgen_root%d", &i) == 1) {
            CXType t = clang_getCanonicalType(clang_getPointeeType(clang_getCursorType(c)));
            Buf expr = {0};
            bprintf(&expr, "*(%s *)0", roots[i]);
            int r = want(t, roots[i], expr.s);
            if (!recs[r].record) die("root %s is not a struct or union", roots[i]);
            root_rec[i] = r;
            free(expr.s);
        }
        free(name);
    }
    return CXChildVisit_Continue;
}

// ---- TU ----------------------------------------------------------------------
//
// ONLY THE FLAGS THAT CAN MOVE A LAYOUT are taken out of the build's command
// line: the probe is not a compile, so an optimisation switch or a warning has
// nothing to say about an offset, and passing one on would only be a way for
// two clangs to disagree. Everything else about the probe is tools/sgcommon.
static void build_args(const char *flags) {
    sgc_args_base(target);
    static const char *with_arg[] = { "-isystem", "-include", "-I", "-D", "-U", "-iquote", "-idirafter" };
    static const char *keep[] = { "-D", "-U", "-I", "-isystem", "-iquote", "-idirafter", "-include", "-std=",
                                  "-fsigned-char", "-funsigned-char", "-fshort-enums", "-fno-short-enums", "-fpack-struct", "-m" };
    char *fl = xstrdup(flags);   // lives for the process: the argument vector points into it
    for (char *tok = strtok(fl, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
        int pair = 0;
        for (size_t k = 0; k < sizeof with_arg / sizeof *with_arg; k++) pair |= !strcmp(tok, with_arg[k]);
        if (pair) {
            const char *flag = tok;
            sgc_arg(flag);
            if (!(tok = strtok(NULL, " \t\n"))) die("build flag %s needs a value", flag);
            sgc_arg(tok);
            continue;
        }
        for (size_t k = 0; k < sizeof keep / sizeof *keep; k++)
            if (!strncmp(tok, keep[k], strlen(keep[k]))) { sgc_arg(tok); break; }
    }
}

// A --const prefix that matches a #define which is not an integer constant
// reaches clang as `enum { __structgen_m3 = (...) };` and comes back as a
// compile error about that name, so the refusal says where it came from.
static const char *const_note(void) {
    return nconsts ? " (a --const prefix matching a non-integer #define shows as __structgen_m*)" : "";
}

void sg_load(void) {
    build_args(build_flags);

    Buf src = {0};
    for (int i = 0; i < nheaders; i++) bprintf(&src, "#include \"%s\"\n", headers[i]);
    for (int i = 0; i < nroots; i++) bprintf(&src, "%s *__structgen_root%d;\n", roots[i], i);
    CXIndex idx = clang_createIndex(0, 0);
    CXTranslationUnit tu = sgc_parse(idx, "__structgen_probe.c", src.s,
        nprefixes ? CXTranslationUnit_DetailedPreprocessingRecord : 0, const_note());
    if (nprefixes) {
        // #defines have no value in the AST: collect their names, then let clang
        // evaluate each one as an enumerator in a second parse.
        clang_visitChildren(clang_getTranslationUnitCursor(tu), on_macro, NULL);
        if (nconsts) {
            for (int i = 0; i < nconsts; i++) bprintf(&src, "enum { __structgen_m%d = (%s) };\n", i, consts[i].name);
            clang_disposeTranslationUnit(tu);
            tu = sgc_parse(idx, "__structgen_probe.c", src.s, 0, const_note());
        }
    }
    clang_visitChildren(clang_getTranslationUnitCursor(tu), on_decl, NULL);
    for (int i = 0; i < nprefixes; i++) {
        int hit = 0;
        for (int j = 0; j < nconsts; j++) hit |= consts[j].prefix == i;
        if (!hit) die("--const %s matches no enum constant or #define", prefixes[i]);
    }
    // Every record the roots embed, then what their pointers reach, and so on
    // until a pass reaches nothing new.
    for (int walked = 0, more = 1; more; ) {
        for (; walked < nrecs; walked++) if (recs[walked].record) {
            Walk w = { walked, 0 };
            clang_Type_visitFields(recs[walked].t, on_field, &w);
        }
        more = 0;
        for (int i = 0; i < nrecs; i++) if (recs[i].kind == 'p') more |= resolve_pointee(i);
    }
    // Everything downstream reads sizes, offsets and names, never a cursor: the
    // CXTypes a Rec keeps are for this walk and are dead once it is over.
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
}
