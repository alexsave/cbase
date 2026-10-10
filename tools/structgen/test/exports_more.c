// The second source of test/exports.sh.
//
// A prototype of a function exports.c defines, with other parameter names: the
// interface takes the DEFINITION's names, and a prototype is not a definition.
int ex_add(int a, unsigned b);

// An unnamed parameter is called by its position.
int ex_unnamed(int, int second) { return second; }
