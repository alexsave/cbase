/* cli.h - the deploy tool's shared parts: the error-and-exit conventions the shell tool keeps (every
 * message starts "cbase: ", a usage error exits 2, a failure exits 1), the usage text, the process
 * helpers every AWS and ssh call goes through, the per-run temporary files and the string helpers.
 * Nothing here reads a setting or calls AWS.
 */
#ifndef CBC_CLI_H
#define CBC_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CBC_EXIT_FAIL 1
#define CBC_EXIT_USAGE 2

/* cbc_fail: "cbase: <message>" on stderr, then exit with STATUS. Every error the tool stops on goes
 * through here, so the prefix and the exit status are one rule. */
void cbc_fail(int status, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

/* cbc_usage: the usage block, the shell's --help text, to FILE. */
void cbc_usage(FILE *to);

/* Memory: every allocation the tool makes goes through these, so "out of memory" stops it in one place.
 * cbc_xmalloc and cbc_xrealloc are malloc and realloc that never return NULL. */
void *cbc_xmalloc(size_t n);
void *cbc_xrealloc(void *p, size_t n);

/* cbc_fmt: printf into a new string (the caller may free it). cbc_asprintf: the same, into *OUT. Both
 * stop the tool when out of memory. */
char *cbc_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cbc_asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A list of strings, grown as it is filled. */
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} cbc_strs;
/* cbc_strs_push: appends V (the string itself, not a copy). */
void cbc_strs_push(cbc_strs *s, char *v);
/* cbc_strs_free: frees each string and the list; S is empty again. */
void cbc_strs_free(cbc_strs *s);
/* cbc_strs_contains: whether one of S is exactly V (a linear scan). */
bool cbc_strs_contains(const cbc_strs *s, const char *v);
/* cbc_strs_join: the strings of S with SEP between them, as a new string ("" for an empty list). */
char *cbc_strs_join(const cbc_strs *s, const char *sep);

/* cbc_cmp_str: a qsort comparator for an array of char pointers, in byte order (LC_ALL=C sort). */
int cbc_cmp_str(const void *a, const void *b);

/* A growing byte buffer, NUL-terminated after its len bytes (data is NULL until something is added): a
 * captured command's output, or a text the tool builds. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} cbc_buf;

/* cbc_buf_add: appends the N bytes at P to B. cbc_buf_str: appends the string S to B. */
void cbc_buf_add(cbc_buf *b, const char *p, size_t n);
void cbc_buf_str(cbc_buf *b, const char *s);

/* cbc_buf_text: a new string of B's text with its trailing newlines removed, as $(...) keeps it ("" when
 * B holds none). B itself is not changed. */
char *cbc_buf_text(const cbc_buf *b);

/* cbc_buf_first_line: a new string of B's text up to its first newline (the whole text when it has none;
 * "" when B holds none). B itself is not changed. */
char *cbc_buf_first_line(const cbc_buf *b);

/* cbc_replace_all: a new string, S with every FROM replaced by TO. FROM must not be empty. */
char *cbc_replace_all(const char *s, const char *from, const char *to);

/* cbc_run_capture: runs ARGV (ARGV[0] searched on PATH) with stdin from /dev/null. Its stdout is appended
 * to OUT (NUL-terminated) and its stderr to ERR, each over a pipe read together (poll) so neither can
 * fill and block the child; a NULL buffer leaves that stream inherited. Returns the child's exit status,
 * 128 + signal if it was killed, or -1 when it could not start. cbc_run is the form for a caller
 * that lets stderr through. */
int cbc_run_capture(char *const argv[], cbc_buf *out, cbc_buf *err);
int cbc_run(char *const argv[], cbc_buf *out);

/* cbc_strip_newlines: removes the newlines at the end of S, as $(...) does. */
void cbc_strip_newlines(cbc_buf *b);

/* cbc_on_path: whether NAME is an executable on PATH (command -v). */
bool cbc_on_path(const char *name);

/* cbc_read_file: FILE's bytes, whole, into *data (NUL-terminated) and *len. False when it cannot be
 * opened or read. */
bool cbc_read_file(const char *file, char **data, size_t *len);

/* cbc_write_file: writes TEXT to PATH, created or truncated; stops the tool when that fails. */
void cbc_write_file(const char *path, const char *text);

/* cbc_is_file, cbc_is_dir: PATH exists and is a regular file (a directory), following symbolic links. */
bool cbc_is_file(const char *path);
bool cbc_is_dir(const char *path);

/* Splitting. Each split takes a text and makes new strings of its pieces; they differ where the
 * shell's read does: cbc_split_on is read -a with the separators SEPS (runs of them make one break, and
 * empty pieces are dropped); cbc_split_tab cuts at the first tab only; cbc_split_tabs gives N fields at
 * each tab and leaves a missing one NULL; cbc_read_tab_fields is IFS=$'\t' read -r, where runs of tabs
 * separate and the last field keeps the rest of the line. The last three edit LINE in place. */
void cbc_split_on(const char *text, const char *seps, cbc_strs *out);
char *cbc_split_tab(char *text, char **rest);
void cbc_split_tabs(char *line, char **f, int n);
void cbc_read_tab_fields(char *line, char **f, int n);

/* cbc_tmp_path: a path in the run's temporary directory, named NAME; removed when the tool exits. */
char *cbc_tmp_path(const char *name);

/* cbc_sleep_ms: waits MS milliseconds. */
void cbc_sleep_ms(long ms);

/* cbc_cksum: POSIX cksum's CRC of the bytes of S (the length appended, as cksum does). */
unsigned long cbc_cksum(const char *s);

/* Strings: a NUL-terminated copy that is never freed (the tool is one short run). */
char *cbc_dup(const char *s);
char *cbc_dupn(const char *s, size_t n);

/* cbc_re: whether the POSIX extended regular expression RE matches anywhere in S (anchor it with ^
 * and $ as the shell's [[ =~ ]] did). A bad RE stops the tool. cbc_re_icase is grep -i. */
bool cbc_re(const char *re, const char *s);
bool cbc_re_icase(const char *re, const char *s);

/* cbc_trim: removes the blanks (space and tab) at either end of S, in place; returns S. */
char *cbc_trim(char *s);

#endif
