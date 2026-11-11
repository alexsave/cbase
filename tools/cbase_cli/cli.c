/* cli.c - the shared parts of the deploy tool (cli.h): the error exit, the usage text, the process
 * helpers, the per-run temporary files, the regular expressions and the string helpers.
 */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

void *cbc_xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return p;
}

void *cbc_xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return q;
}

char *cbc_fmt(const char *fmt, ...) {
  char *s = NULL;
  va_list ap;
  va_start(ap, fmt);
  int n = vasprintf(&s, fmt, ap);
  va_end(ap);
  if (n < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return s;
}

void cbc_asprintf(char **out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vasprintf(out, fmt, ap);
  va_end(ap);
  if (n < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
}

void cbc_strs_push(cbc_strs *s, char *v) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->v = cbc_xrealloc(s->v, s->cap * sizeof *s->v);
  }
  s->v[s->n++] = v;
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

int cbc_run_err(char *const argv[], cbc_buf *out, const char *err_path) {
  int pfd[2] = { -1, -1 };
  if (out && pipe(pfd) != 0) return -1;
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    if (out) {
      close(pfd[0]);
      close(pfd[1]);
    }
    return -1;
  }
  if (pid == 0) {
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) {
      dup2(nul, STDIN_FILENO);
      close(nul);
    }
    if (out) {
      dup2(pfd[1], STDOUT_FILENO);
      close(pfd[0]);
      close(pfd[1]);
    }
    if (err_path) {
      int efd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (efd >= 0) {
        dup2(efd, STDERR_FILENO);
        close(efd);
      }
    }
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

void cbc_strip_newlines(cbc_buf *b) {
  while (b->len > 0 && b->data[b->len - 1] == '\n') b->data[--b->len] = '\0';
}

bool cbc_on_path(const char *name) {
  if (strchr(name, '/')) return access(name, X_OK) == 0;
  const char *path = getenv("PATH");
  if (!path) return false;
  char *copy = cbc_dup(path);
  for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
    size_t need = strlen(dir) + strlen(name) + 2;
    char *full = cbc_xmalloc(need);
    snprintf(full, need, "%s/%s", dir, name);
    bool ok = access(full, X_OK) == 0;
    free(full);
    if (ok) return true;
  }
  return false;
}

bool cbc_read_file(const char *file, char **data, size_t *len) {
  FILE *f = fopen(file, "rb");
  if (!f) return false;
  size_t cap = 65536, n = 0;
  char *buf = cbc_xmalloc(cap + 1);
  size_t r;
  while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
    n += r;
    if (n == cap) {
      cap *= 2;
      buf = cbc_xrealloc(buf, cap + 1);
    }
  }
  bool ok = !ferror(f);
  fclose(f);
  buf[n] = '\0';
  *data = buf;
  *len = n;
  return ok;
}

static char *tmp_dir;
static cbc_strs tmp_files;

static void tmp_cleanup(void) {
  for (size_t i = 0; i < tmp_files.n; i++) unlink(tmp_files.v[i]);
  if (tmp_dir) rmdir(tmp_dir);
}

char *cbc_tmp_path(const char *name) {
  if (!tmp_dir) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    size_t need = strlen(base) + 32;
    char *tmpl = cbc_xmalloc(need);
    snprintf(tmpl, need, "%s/cbase.XXXXXX", base);
    tmp_dir = mkdtemp(tmpl);
    if (!tmp_dir) cbc_fail(CBC_EXIT_FAIL, "cannot make a temporary directory in %s", base);
    atexit(tmp_cleanup);
  }
  size_t need = strlen(tmp_dir) + strlen(name) + 2;
  char *p = cbc_xmalloc(need);
  snprintf(p, need, "%s/%s", tmp_dir, name);
  cbc_strs_push(&tmp_files, p);
  return p;
}

void cbc_sleep_ms(long ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

/* POSIX cksum: the CRC-32 of the bytes, then of the length (least significant byte first, as the
 * standard has it), complemented. */
unsigned long cbc_cksum(const char *s) {
  static unsigned long table[256];
  static bool made;
  if (!made) {
    for (unsigned long i = 0; i < 256; i++) {
      unsigned long r = i << 24;
      for (int k = 0; k < 8; k++) r = (r & 0x80000000UL) ? (((r << 1) ^ 0x04c11db7UL) & 0xffffffffUL) : ((r << 1) & 0xffffffffUL);
      table[i] = r;
    }
    made = true;
  }
  unsigned long crc = 0;
  size_t n = strlen(s);
  for (size_t i = 0; i < n; i++) crc = ((crc << 8) ^ table[((crc >> 24) ^ (unsigned char)s[i]) & 0xff]) & 0xffffffffUL;
  for (unsigned long len = n; len; len >>= 8) crc = ((crc << 8) ^ table[((crc >> 24) ^ (len & 0xff)) & 0xff]) & 0xffffffffUL;
  return ~crc & 0xffffffffUL;
}

char *cbc_dupn(const char *s, size_t n) {
  char *d = cbc_xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

char *cbc_dup(const char *s) { return cbc_dupn(s, strlen(s)); }

static bool re_flags(const char *re, const char *s, int flags) {
  regex_t r;
  if (regcomp(&r, re, REG_EXTENDED | REG_NOSUB | flags) != 0) cbc_fail(CBC_EXIT_FAIL, "internal: bad pattern %s", re);
  bool hit = regexec(&r, s, 0, NULL, 0) == 0;
  regfree(&r);
  return hit;
}

bool cbc_re(const char *re, const char *s) { return re_flags(re, s, 0); }
bool cbc_re_icase(const char *re, const char *s) { return re_flags(re, s, REG_ICASE); }

bool cbc_file_matches(const char *file, const char *re, bool icase) {
  char *data;
  size_t len;
  if (!cbc_read_file(file, &data, &len)) return false;
  char *p = data;
  for (;;) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (icase ? cbc_re_icase(re, p) : cbc_re(re, p)) return true;
    if (!nl) break;
    p = nl + 1;
  }
  return false;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

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
