/* cli.h - the deploy tool's shared parts: the error-and-exit conventions the shell tool keeps (every
 * message starts "cbase: ", a usage error exits 2, a failure exits 1), the usage text, and the process
 * helper every AWS and ssh call goes through. Nothing here reads a setting or calls AWS.
 */
#ifndef CBC_CLI_H
#define CBC_CLI_H

#include <stddef.h>
#include <stdio.h>

/* The name the tool is run as in its messages ("cbase"), and the exit statuses the shell uses. */
#define CBC_EXIT_FAIL 1
#define CBC_EXIT_USAGE 2

/* cbc_fail: "cbase: <message>" on stderr, then exit with STATUS. Every error the tool stops on goes
 * through here, so the prefix and the exit status are one rule. */
void cbc_fail(int status, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

/* cbc_usage: the usage block, the shell's --help text, to FILE. */
void cbc_usage(FILE *to);

/* A growing byte buffer for a captured command's output. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} cbc_buf;

/* cbc_run: runs ARGV (ARGV[0] is searched on PATH) with stdin from /dev/null and stderr inherited.
 * Its stdout goes to OUT when OUT is not NULL, and to this process's stdout when it is NULL.
 * Returns the child's exit status, 128 + signal if it was killed, or -1 when it could not start. */
int cbc_run(char *const argv[], cbc_buf *out);

/* Strings: a NUL-terminated copy that is never freed (the tool is one short run). */
char *cbc_dup(const char *s);
char *cbc_dupn(const char *s, size_t n);

/* cbc_re: whether the POSIX extended regular expression RE matches anywhere in S (anchor it with ^
 * and $ as the shell's [[ =~ ]] did). A bad RE is a programming error and stops the tool. */
int cbc_re(const char *re, const char *s);

/* cbc_trim: removes the blanks (space and tab) at either end of S, in place; returns S. */
char *cbc_trim(char *s);

#endif
