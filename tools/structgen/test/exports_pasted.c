// The third source of test/exports.sh, and it must name no export in its text.
//
// The one definition here is pasted together by a macro, so a search of this
// file for the name the link line lists finds nothing, and the sources that DO
// spell a wanted name are read first. It is still a definition the linker
// exports: only the walk of every remaining source finds it.
#define EX_DOOR(n) int ex_##n(int seat) { return seat; }
EX_DOOR(pasted)
