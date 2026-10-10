// The traversal: the only part of structgen that talks to libclang.
#ifndef SG_CLANG_H
#define SG_CLANG_H

// Parses the requested headers for the requested target under the build's
// flags, and fills the model (sg_model.h) with every record the roots reach and
// every constant a --const prefix matches. Nothing calls libclang after it
// returns, so it holds the translation unit for exactly as long as it walks it.
void sg_load(void);

// The same for an exports run: parses each --source as its own translation
// unit, under the build's flags, and fills the model's functions with every
// external definition named in `want` and every one that carries export_name.
// Sources that spell no wanted name are read last, and only while a wanted name
// is still undefined (sg_clang.c says why that is an ordering and not a filter).
void sg_load_exports(char **want, int nwant);

#endif
