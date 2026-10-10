// An exports run: what one link of a wasm module exports, as a typed
// TypeScript interface, and that interface checked against the module the
// linker wrote. structgen.c documents the flags; sg_exports.c says where each
// fact comes from.
#ifndef SG_EXPORTS_H
#define SG_EXPORTS_H

// Reads every --link's flags, loads the definitions they name out of the
// --source files, resolves each link's exports against them (an export no
// source defines is a refusal), and then writes each --exports-ts and makes
// each --check-wasm.
void sg_exports_run(void);

#endif
