// One in-memory probe, parsed by libclang, for both generators.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clang-c/Index.h>
#include "sgc.h"
#include "sgc_probe.h"

char *str(CXString cs) { char *d = xstrdup(clang_getCString(cs)); clang_disposeString(cs); return d; }

static const char *args[512];
static int nargs;

void sgc_arg(const char *a) {
    if (nargs == (int)(sizeof args / sizeof *args)) die("too many clang arguments (%d)", nargs);
    args[nargs++] = a;
}

void sgc_args_base(const char *target) {
    static char triple[256];
    if (target) {
        snprintf(triple, sizeof triple, "--target=%s", target);
        sgc_arg(triple);
    }
    sgc_arg("-ffreestanding"); sgc_arg("-iquote"); sgc_arg(".");
    // libclang does not find its own builtin headers (stdint.h, stdbool.h):
    // use the resource dir of the clang this tool was built against.
    sgc_arg("-resource-dir"); sgc_arg(SG_RESOURCE_DIR);
}

static CXTranslationUnit parse(CXIndex idx, const char *name, const char *src, unsigned opts, const char *errnote) {
    struct CXUnsavedFile probe = { name, src, (unsigned long)strlen(src) };
    CXTranslationUnit tu;
    enum CXErrorCode e = clang_parseTranslationUnit2(idx, name, args, nargs, &probe, 1, opts, &tu);
    if (e != CXError_Success) die("libclang parse failed (CXErrorCode %d)", (int)e);
    int errors = 0;
    for (unsigned i = 0; i < clang_getNumDiagnostics(tu); i++) {
        CXDiagnostic dg = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(dg) >= CXDiagnostic_Error) {
            char *m = str(clang_formatDiagnostic(dg, clang_defaultDiagnosticDisplayOptions()));
            fprintf(stderr, "%s\n", m); free(m); errors++;
        }
        clang_disposeDiagnostic(dg);
    }
    if (errors) die("%d compile error(s)%s", errors, errnote);
    return tu;
}

CXTranslationUnit sgc_parse(CXIndex idx, const char *name, const char *src, unsigned opts, const char *errnote) {
    return parse(idx, name, src, opts | CXTranslationUnit_SkipFunctionBodies, errnote);
}

CXTranslationUnit sgc_parse_bodies(CXIndex idx, const char *name, const char *src, unsigned opts, const char *errnote) {
    return parse(idx, name, src, opts, errnote);
}
