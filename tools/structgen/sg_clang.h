// The traversal: the only part of structgen that talks to libclang.
#ifndef SG_CLANG_H
#define SG_CLANG_H

// Parses the requested headers for the requested target under the build's
// flags, and fills the model (sg_model.h) with every record the roots reach and
// every constant a --const prefix matches. Nothing calls libclang after it
// returns, so it holds the translation unit for exactly as long as it walks it.
void sg_load(void);

#endif
