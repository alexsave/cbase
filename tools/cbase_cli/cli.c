/* cli.c - the shared parts of the deploy tool (cli.h): the error exit, the usage text, the process
 * helpers, the per-run temporary files, the regular expressions and the string helpers.
 */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
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

void cbc_strs_free(cbc_strs *s) {
  for (size_t i = 0; i < s->n; i++) free(s->v[i]);
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

bool cbc_strs_contains(const cbc_strs *s, const char *v) {
  for (size_t i = 0; i < s->n; i++)
    if (strcmp(s->v[i], v) == 0) return true;
  return false;
}

char *cbc_strs_join(const cbc_strs *s, const char *sep) {
  size_t total = 1;
  for (size_t i = 0; i < s->n; i++) total += strlen(s->v[i]) + (i ? strlen(sep) : 0);
  char *out = cbc_xmalloc(total);
  out[0] = '\0';
  for (size_t i = 0; i < s->n; i++) {
    if (i) strcat(out, sep);
    strcat(out, s->v[i]);
  }
  return out;
}

int cbc_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void cbc_buf_add(cbc_buf *b, const char *p, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) cap *= 2;
    b->data = cbc_xrealloc(b->data, cap);
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
}

void cbc_buf_str(cbc_buf *b, const char *s) { cbc_buf_add(b, s, strlen(s)); }

char *cbc_buf_text(const cbc_buf *b) {
  if (!b->data) return cbc_dup("");
  size_t n = b->len;
  while (n > 0 && b->data[n - 1] == '\n') n--;
  return cbc_dupn(b->data, n);
}

char *cbc_buf_first_line(const cbc_buf *b) {
  if (!b->data) return cbc_dup("");
  const char *nl = memchr(b->data, '\n', b->len);
  return cbc_dupn(b->data, nl ? (size_t)(nl - b->data) : b->len);
}

char *cbc_replace_all(const char *s, const char *from, const char *to) {
  cbc_buf b = { 0 };
  size_t fl = strlen(from);
  const char *p = s;
  const char *hit;
  while ((hit = strstr(p, from)) != NULL) {
    cbc_buf_add(&b, p, (size_t)(hit - p));
    cbc_buf_str(&b, to);
    p = hit + fl;
  }
  cbc_buf_str(&b, p);
  return b.data ? b.data : cbc_dup("");
}

/* drain: reads the pipe ends in FDS (-1 for a closed one) into their buffers BUFS until each is at its end,
 * with poll, so neither can fill and block the child. */
static void drain(int fds[2], cbc_buf *bufs[2]) {
  char chunk[4096];
  for (;;) {
    struct pollfd pf[2];
    int map[2], n = 0;
    for (int i = 0; i < 2; i++) {
      if (fds[i] < 0) continue;
      pf[n].fd = fds[i];
      pf[n].events = POLLIN;
      pf[n].revents = 0;
      map[n++] = i;
    }
    if (n == 0) return;
    if (poll(pf, (nfds_t)n, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    for (int k = 0; k < n; k++) {
      if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int i = map[k];
      ssize_t r = read(fds[i], chunk, sizeof chunk);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) {
        close(fds[i]);
        fds[i] = -1;
      } else {
        cbc_buf_add(bufs[i], chunk, (size_t)r);
      }
    }
  }
}

int cbc_run_capture(char *const argv[], cbc_buf *out, cbc_buf *err) {
  int pfd[2][2] = { { -1, -1 }, { -1, -1 } };   /* [0] stdout, [1] stderr */
  cbc_buf *bufs[2] = { out, err };
  for (int i = 0; i < 2; i++) {
    if (bufs[i] && pipe(pfd[i]) != 0) {
      for (int j = 0; j < i; j++) {
        if (bufs[j]) {
          close(pfd[j][0]);
          close(pfd[j][1]);
        }
      }
      return -1;
    }
  }
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    for (int i = 0; i < 2; i++) {
      if (bufs[i]) {
        close(pfd[i][0]);
        close(pfd[i][1]);
      }
    }
    return -1;
  }
  if (pid == 0) {
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) {
      dup2(nul, STDIN_FILENO);
      close(nul);
    }
    for (int i = 0; i < 2; i++) {
      if (!bufs[i]) continue;
      dup2(pfd[i][1], i == 0 ? STDOUT_FILENO : STDERR_FILENO);
      close(pfd[i][0]);
      close(pfd[i][1]);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  int rfd[2] = { -1, -1 };
  for (int i = 0; i < 2; i++) {
    if (!bufs[i]) continue;
    close(pfd[i][1]);
    rfd[i] = pfd[i][0];
  }
  drain(rfd, bufs);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

int cbc_run(char *const argv[], cbc_buf *out) { return cbc_run_capture(argv, out, NULL); }

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

void cbc_write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f || fputs(text, f) == EOF || fclose(f) != 0) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", path);
}

bool cbc_is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool cbc_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void cbc_split_on(const char *text, const char *seps, cbc_strs *out) {
  const char *p = text;
  while (*p) {
    while (*p && strchr(seps, *p)) p++;
    if (!*p) break;
    const char *b = p;
    while (*p && !strchr(seps, *p)) p++;
    cbc_strs_push(out, cbc_dupn(b, (size_t)(p - b)));
  }
}

/* Splits TEXT at its first tab; the rest is what follows it ("" when there is no tab). */
char *cbc_split_tab(char *text, char **rest) {
  char *tab = strchr(text, '\t');
  if (!tab) {
    *rest = "";
    return text;
  }
  *tab = '\0';
  *rest = tab + 1;
  return text;
}

void cbc_split_tabs(char *line, char **f, int n) {
  for (int k = 0; k < n; k++) {
    f[k] = line;
    char *tab = strchr(line, '\t');
    if (!tab) {
      for (int j = k + 1; j < n; j++) f[j] = NULL;
      return;
    }
    *tab = '\0';
    line = tab + 1;
  }
}

void cbc_read_tab_fields(char *line, char **f, int n) {
  char *p = line;
  for (int i = 0; i < n; i++) f[i] = "";
  for (int i = 0; i < n; i++) {
    while (*p == '\t') p++;
    if (!*p) break;
    if (i == n - 1) {
      size_t len = strlen(p);
      while (len > 0 && p[len - 1] == '\t') p[--len] = '\0';
      f[i] = p;
      break;
    }
    char *b = p;
    while (*p && *p != '\t') p++;
    if (*p) *p++ = '\0';
    f[i] = b;
  }
}

/* THE RUN'S TEMPORARY DIRECTORY. cbc_tmp_path makes one directory, cbase.XXXXXX under TMPDIR, the first
 * time a path is asked for, and keeps a list of every path the run makes in it: the directory first, then
 * each file and directory in the order it was made (cbc_tmp_note records the static tree's own). The list
 * is removed in reverse, so a file goes before its directory. That runs at a normal exit (atexit) and at
 * SIGINT, SIGTERM or SIGHUP (tmp_on_signal), as the shell tool's EXIT trap runs for those three.
 *
 * The signal handler does not wait for anything and does not return. It unlinks or removes each listed
 * path (unlink, rmdir: async-signal-safe, and the list is read without allocating), restores the default
 * action and raises the signal again, so the process ends by that signal. It does not wait for a child: a
 * child left running is the terminal's process group's business, as the shell tool leaves it. A child that
 * is between fork and exec runs the handler's copy too; the owner check (getpid against tmp_owner) keeps it
 * from removing its parent's files. The list is changed only with these signals blocked (tmp_list_add), so
 * the handler never sees the pointer and the count of a list that is half changed. A signal that was ignored
 * when the run started (SIG_IGN, a background job's SIGINT) stays ignored, as it does in the shell.
 */
static char *tmp_dir;
static cbc_strs tmp_paths;
static pid_t tmp_owner;
static const int tmp_sigs[] = { SIGINT, SIGTERM, SIGHUP };
#define TMP_NSIGS (sizeof tmp_sigs / sizeof tmp_sigs[0])

static void tmp_remove_all(void) {
  for (size_t i = tmp_paths.n; i-- > 0;) {
    if (unlink(tmp_paths.v[i]) != 0) rmdir(tmp_paths.v[i]);
  }
}

static void tmp_on_signal(int sig) {
  if (getpid() == tmp_owner) tmp_remove_all();
  signal(sig, SIG_DFL);
  sigset_t only;
  sigemptyset(&only);
  sigaddset(&only, sig);
  sigprocmask(SIG_UNBLOCK, &only, NULL);
  raise(sig);
  _exit(128 + sig);
}

static void tmp_block_sigs(sigset_t *old) {
  sigset_t block;
  sigemptyset(&block);
  for (size_t k = 0; k < TMP_NSIGS; k++) sigaddset(&block, tmp_sigs[k]);
  sigprocmask(SIG_BLOCK, &block, old);
}

/* tmp_list_add: appends P (the list keeps the pointer) with the signals blocked. */
static void tmp_list_add(char *p) {
  sigset_t old;
  tmp_block_sigs(&old);
  cbc_strs_push(&tmp_paths, p);
  sigprocmask(SIG_SETMASK, &old, NULL);
}

void cbc_tmp_note(const char *path) { tmp_list_add(cbc_dup(path)); }

/* tmp_make: the directory, its removal at exit, and the three handlers (not for a signal that is ignored
 * at the start). All of it with the signals blocked, so a signal lands either before the directory exists
 * (nothing to remove) or after the handlers are in place. */
static void tmp_make(void) {
  const char *base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";
  size_t need = strlen(base) + 32;
  char *tmpl = cbc_xmalloc(need);
  snprintf(tmpl, need, "%s/cbase.XXXXXX", base);
  sigset_t old;
  tmp_block_sigs(&old);
  tmp_dir = mkdtemp(tmpl);
  if (tmp_dir) {
    tmp_owner = getpid();
    cbc_strs_push(&tmp_paths, tmp_dir);
    atexit(tmp_remove_all);
    for (size_t k = 0; k < TMP_NSIGS; k++) {
      struct sigaction cur, sa;
      if (sigaction(tmp_sigs[k], NULL, &cur) == 0 && cur.sa_handler == SIG_IGN) continue;
      memset(&sa, 0, sizeof sa);
      sa.sa_handler = tmp_on_signal;
      sigemptyset(&sa.sa_mask);
      sigaction(tmp_sigs[k], &sa, NULL);
    }
  }
  sigprocmask(SIG_SETMASK, &old, NULL);
  if (!tmp_dir) cbc_fail(CBC_EXIT_FAIL, "cannot make a temporary directory in %s", base);
}

char *cbc_tmp_path(const char *name) {
  if (!tmp_dir) tmp_make();
  size_t need = strlen(tmp_dir) + strlen(name) + 2;
  char *p = cbc_xmalloc(need);
  snprintf(p, need, "%s/%s", tmp_dir, name);
  tmp_list_add(p);
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

cbc_flags_t cbc_flags;

static bool env_is_one(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "1") == 0;
}

void cbc_flags_from_env(void) {
  const char *dry = getenv("DRY_RUN");
  cbc_flags.dry_run = dry && strcmp(dry, "1") == 0;
  cbc_flags.dry_banner = dry && *dry;
  cbc_flags.accept_drift = env_is_one("CBASE_ACCEPT_DRIFT");
  cbc_flags.prune = env_is_one("CBASE_PRUNE");
  cbc_flags.allow_downtime = env_is_one("CBASE_ALLOW_DOWNTIME");
}

bool cbc_re(const char *re, const char *s) { return re_flags(re, s, 0); }
bool cbc_re_icase(const char *re, const char *s) { return re_flags(re, s, REG_ICASE); }

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
