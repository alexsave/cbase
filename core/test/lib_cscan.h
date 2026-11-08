// lib_cscan.h - the one C scanner the source gates share (unit_random_gate.c,
// unit_num_gate.c, unit_json_gate.c, unit_name_gate.c).
//
// Two halves, each with one owner here:
//
//   * THE WALK (cscan_walk): every file under a directory, read whole and handed to the
//     gate by the name it reports ("core/src/x.c", lib_tree.h), C sources only or every
//     file, with the directories and files the gate does not read left out.
//   * THE LEXER (cscan_lex): a C source as the tokens a gate looks at, with comments
//     skipped: identifiers, numbers (so a suffix like 10l is not an identifier), string
//     and character literals (their raw text between the quotes, escapes as written), and
//     #include lines (the header spec, so a gate can check what is included and nothing
//     in it reads as code). Every other directive is lexed as code, so a #define cannot
//     hide a call or a literal. Anything else is a punctuation token of one character.
//
// The name gate reads every line, comments included, so it takes the walk alone.
#ifndef CB_LIB_CSCAN_H
#define CB_LIB_CSCAN_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CSCAN_IDENT,     // [A-Za-z_][A-Za-z0-9_]*
    CSCAN_NUMBER,    // a digit and every identifier character after it
    CSCAN_STRING,    // "...": the text between the quotes, raw
    CSCAN_CHAR,      // '...': the text between the quotes, raw
    CSCAN_INCLUDE,   // #include <h> / "h": the rest of the line after "include"
    CSCAN_PUNCT,     // any other character but a blank or a newline
} cscan_kind;

typedef struct {
    cscan_kind  kind;
    const char *s;   // into the text: the token (a literal's body, an include's spec)
    size_t      n;
    int         line;   // where the token began, from 1
} cscan_tok;

typedef void (*cscan_tok_fn)(const cscan_tok *t, void *ctx);

// Every token of s[0..n), in order, comments skipped.
void cscan_lex(const char *s, size_t n, cscan_tok_fn fn, void *ctx);

// A file read whole (NUL-terminated, *n its length), or NULL. The caller frees it.
char *cscan_slurp(const char *path, size_t *n);

typedef struct {
    bool               all_files;   // every file, not only *.c and *.h
    const char *const *skip_dirs;   // directory names never entered (NULL-terminated), or NULL
    // A file (by its reported name) the gate does not read, or NULL for none.
    bool (*skip)(const char *name, void *ctx);
    // A file's text, by its reported name.
    void (*file)(const char *name, const char *text, size_t n, void *ctx);
    // A file that cannot be read, or NULL to pass it by.
    void (*unreadable)(const char *name, void *ctx);
    void *ctx;
} cscan_walker;

// Every file under dir (opened as `dir`), each named `name`/<the rest of its path>. Names that
// begin with '.' are never entered or read. Returns the number of files handed to w->file.
int cscan_walk(const cscan_walker *w, const char *dir, const char *name);

#endif
