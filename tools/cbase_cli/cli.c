/* cli.c - the shared parts of the deploy tool (cli.h): the error exit, the usage text, the process
 * helper and the string helpers.
 */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void cbc_fail(int status, const char *fmt, ...) {
  va_list ap;
  fputs("cbase: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(status);
}

/* The shell tool's usage(): tools/cbase's header lines from "# usage:" to "#   state join|leave", with
 * the "# " taken off each. README.md's "cbase/tools/cbase --help" block is the same text. */
void cbc_usage(FILE *to) {
  fputs("usage: cbase/tools/cbase [-f FILE] COMMAND    (FILE: the app's settings, default ./cbase.conf)\n"
        "\n"
        "  config check       cbase.conf read and printed, defaults filled in (no AWS)\n"
        "  doctor             the config, the AWS account, each group and its members\n"
        "  apps [GROUP]       a group's apps, with each one's port, service and memory\n"
        "                     (default: cbase.conf's groups)\n"
        "  deploy [--dry-run] [--accept-drift] [--allow-downtime] [--prune] [--no-aliases|--attach]\n"
        "                     the app: its service on its group's VM, its static files, its\n"
        "                     distribution; DRY_RUN=1, CBASE_ACCEPT_DRIFT=1, CBASE_ALLOW_DOWNTIME=1\n"
        "                     and CBASE_PRUNE=1 are the same as the flags; --no-aliases and --attach\n"
        "                     are CBASE_ALIASES off and on, a staged attach\n"
        "  undeploy [--delete-files] [--delete-data]\n"
        "                     the app's distribution and service gone, its groups left; its files\n"
        "                     and its data kept unless a flag says otherwise\n"
        "  group-init vm|static GROUP [--type TYPE] [--ipv4] [--yes]\n"
        "                     makes a group (an IPv6-only instance, dual-stack with a public IPv4\n"
        "                     with --ipv4; or a bucket), the one command that does; a VM's needs\n"
        "                     --yes or a typed yes (it costs money)\n"
        "  group-destroy vm|static GROUP [--delete-data] [--yes]\n"
        "                     deletes an empty group's stack (refused while it has members)\n"
        "  template           the app's stack template, as deploy renders it (no AWS)\n"
        "  group-template vm|static GROUP [--ipv4]\n"
        "                     a group's stack template (no AWS)\n"
        "  state join|leave   the membership step deploy and undeploy run, for repair\n",
        to);
}

static void buf_add(cbc_buf *b, const char *p, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n + 1) cap *= 2;
    char *d = realloc(b->data, cap);
    if (!d) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
}

int cbc_run(char *const argv[], cbc_buf *out) {
  int pfd[2] = { -1, -1 };
  if (out && pipe(pfd) != 0) return -1;
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    if (out) { close(pfd[0]); close(pfd[1]); }
    return -1;
  }
  if (pid == 0) {
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
    if (out) { dup2(pfd[1], STDOUT_FILENO); close(pfd[0]); close(pfd[1]); }
    execvp(argv[0], argv);
    _exit(127);
  }
  if (out) {
    close(pfd[1]);
    char chunk[4096];
    ssize_t r;
    while ((r = read(pfd[0], chunk, sizeof chunk)) != 0) {
      if (r < 0) {
        if (errno == EINTR) continue;
        break;
      }
      buf_add(out, chunk, (size_t)r);
    }
    close(pfd[0]);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

char *cbc_dupn(const char *s, size_t n) {
  char *d = malloc(n + 1);
  if (!d) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

char *cbc_dup(const char *s) { return cbc_dupn(s, strlen(s)); }

int cbc_re(const char *re, const char *s) {
  regex_t r;
  if (regcomp(&r, re, REG_EXTENDED | REG_NOSUB) != 0) cbc_fail(CBC_EXIT_FAIL, "internal: bad pattern %s", re);
  int hit = regexec(&r, s, 0, NULL, 0) == 0;
  regfree(&r);
  return hit;
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

char *cbc_trim(char *s) {
  char *a = s;
  while (is_blank(*a)) a++;
  char *e = a + strlen(a);
  while (e > a && is_blank(e[-1])) e--;
  size_t n = (size_t)(e - a);
  memmove(s, a, n);
  s[n] = '\0';
  return s;
}
