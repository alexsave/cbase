// unit_version.c - CB_VERSION (ctx.h) is the release VERSION names.
//
// VERSION, one line at cbase's root, is the one source of the release number: a release is
// tagged v<VERSION> (ci.yml's version job fails a tag that is anything else), and this test fails
// when CB_VERSION, which /health reports, says something other than VERSION. So bumping the
// release is one edit to VERSION, which this test then makes the second edit, to ctx.h, impossible
// to forget. It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_tree.h"

#include "ctx.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void cb_version_is_the_version_file(void) {
    char p[TT_PATH_MAX], v[64] = { 0 };
    FILE *f = fopen(tt_path(p, sizeof p, "VERSION"), "rb");
    T_CHECK(f != NULL);
    if (!f) return;
    size_t n = fread(v, 1, sizeof v - 1, f);
    fclose(f);
    T_CHECK(n > 0 && v[n - 1] == '\n');   // one line, ended
    while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
    T_CHECK(strchr(v, '\n') == NULL);
    // MAJOR.MINOR.PATCH, digits and two dots, no "v" (the tag adds it)
    int dots = 0;
    bool shape = n > 0 && isdigit((unsigned char)v[0]) && isdigit((unsigned char)v[n - 1]);
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '.') { dots++; shape = shape && i + 1 < n && v[i + 1] != '.'; }
        else shape = shape && isdigit((unsigned char)v[i]);
    }
    T_CHECK(shape && dots == 2);
    T_EQ_STR(CB_VERSION, v);
}

int main(void) {
    T_RUN(cb_version_is_the_version_file);
    return T_DONE();
}
