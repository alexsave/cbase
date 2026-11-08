// lib_tree.h - where the source gates (unit_name_gate.c, unit_json_gate.c, unit_num_gate.c) find the
// trees they read, wherever the test runs.
//
// cbase.mk runs every test from the including Makefile's directory, which is c_base's own root only
// when c_base is the product's build. So the gates take their trees from the environment cbase.mk
// exports:
//
//   CBASE_ROOT            the c_base root as the test's directory reaches it ("." when unset)
//   CBASE_APP_SRC_ROOTS   the product's source roots, space-separated, as the product spells them
//   CBASE_APP_JSON_ROOTS  the product's trees the JSON gate reads (its sources and its tools)
//   CBASE_APP_NAME_ALLOW  the product's own name-gate allowlist (a path), or empty
//
// A c_base file is opened under CBASE_ROOT and NAMED from it ("core/src/x.c"), so a report and an
// allowlist read the same in every build; a product file is opened and named as the product spells it.
#ifndef CB_LIB_TREE_H
#define CB_LIB_TREE_H

#include <stddef.h>

#define TT_ROOTS_MAX 16
#define TT_PATH_MAX  512

// CBASE_ROOT, or ".".
const char *tt_root(void);
// `rel` (a path from the c_base root) as this process opens it, into out.
const char *tt_path(char *out, size_t cap, const char *rel);
// The words of environment variable `name` into roots[0..n), n returned (at most TT_ROOTS_MAX).
int tt_env_list(const char *name, char roots[][TT_PATH_MAX], int max);

#endif
