// The probe translation unit: how both generators ask libclang anything.
//
// Neither tool parses a file off disk. Each writes a few lines of C in memory -
// the headers to include, plus whatever declares the thing it wants to ask
// about - and hands that to libclang as an unsaved file. The question differs
// (structgen declares a pointer per --root, datagen names a table); the probe,
// the flags it is parsed with and the rule that any error-severity diagnostic
// ends the run do not.
#ifndef SGC_PROBE_H
#define SGC_PROBE_H
#include <clang-c/Index.h>

// A CXString, copied out and disposed.
char *str(CXString cs);

// One more argument for the probe's command line.
void sgc_arg(const char *a);
// The arguments every probe starts with, in this order: the target (NULL leaves
// the host's own), -ffreestanding, the tool's --cwd for "" includes, and the
// builtin-header directory of the clang this tool was BUILT against, because
// libclang does not find its own (stdint.h, stdbool.h). Whatever a tool adds
// after this - a build's layout flags, a warning switch - it adds itself.
void sgc_args_base(const char *target);

// The probe, parsed under those arguments. `name` is the in-memory file's name,
// `opts` whatever the caller needs on top of SkipFunctionBodies, and `errnote`
// is appended to the "N compile error(s)" refusal ("" for nothing to add).
CXTranslationUnit sgc_parse(CXIndex idx, const char *name, const char *src, unsigned opts, const char *errnote);
// The same probe with function bodies KEPT. Skipping them is what every other
// question wants, but libclang then reports a function with a body as a mere
// declaration, and a caller that has to tell a definition from a prototype
// (structgen's exports run) cannot ask that of a skipped one.
CXTranslationUnit sgc_parse_bodies(CXIndex idx, const char *name, const char *src, unsigned opts, const char *errnote);

#endif
