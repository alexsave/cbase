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

/* A list of strings, grown as it is filled. */
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} cbc_strs;
void cbc_strs_push(cbc_strs *s, char *v);

/* A growing byte buffer for a captured command's output. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} cbc_buf;

/* cbc_run_err: runs ARGV (ARGV[0] searched on PATH) with stdin from /dev/null. Its stdout goes to OUT
 * when OUT is not NULL (appended to it, NUL-terminated), and to this process's stdout when it is NULL.
 * Its stderr goes to the file ERR_PATH, created or truncated, when ERR_PATH is not NULL, and is
 * inherited when it is. Returns the child's exit status, 128 + signal if it was killed, or -1 when it
 * could not start. */
int cbc_run_err(char *const argv[], cbc_buf *out, const char *err_path);

/* cbc_strip_newlines: removes the newlines at the end of S, as $(...) does. */
void cbc_strip_newlines(cbc_buf *b);

/* cbc_on_path: whether NAME is an executable on PATH (command -v). */
bool cbc_on_path(const char *name);

/* cbc_read_file: FILE's bytes, whole, into *data (NUL-terminated) and *len. False when it cannot be
 * opened or read. */
bool cbc_read_file(const char *file, char **data, size_t *len);

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

/* cbc_file_matches: whether any line of FILE matches RE (grep -q, line by line); ICASE as grep -i. */
bool cbc_file_matches(const char *file, const char *re, bool icase);

/* cbc_trim: removes the blanks (space and tab) at either end of S, in place; returns S. */
char *cbc_trim(char *s);

#endif
