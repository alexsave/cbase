/* state.c - the state file (state.sh): a group's members, read from S3 with its ETag, changed and
 * written back with S3's conditional PUT, retried when another writer won, and the memory fit a vm
 * join is checked against. The format and every message are state.sh's.
 */
#include "state.h"

#include "aws.h"
#include "conf.h"
#include "../../lib/num_parse.h"

#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATE_VERSION "1"
#define STATE_PORT_MIN 8700
#define STATE_PORT_MAX 8799
#define STATE_ATTEMPTS 8

char *state_origin_in = "";
bool state_origin_need = false;
long state_fit_limit = -1;
char *state_fit_what = "";
char *state_legacy_port = "";
char *state_legacy_data = "";
char *state_port = "";
bool state_joined_new = false;

/* ST_*: the file as read. Member i is ST_LINES[i], app ST_APPS[i], port ST_PORTS[i] (-1 for a static
 * member, which has none). */
typedef struct {
  cbc_strs lines;
  cbc_strs apps;
  long *ports;
  size_t n;
  size_t cap;
} members_t;
static members_t st;
char *state_member_origin = "";        /* ST_ORIGIN: the header's origin, "" when none */
char *state_member_origin_addr = "";   /* ST_ORIGIN_ADDR: ipv6 or dualstack */
char *state_fit_report = "";
static char *state_etag = "";
/* state_no_bucket: STATE_NO_BUCKET, a dry run's read with no state bucket yet: no file is read (rc 3). */
bool state_no_bucket = false;

static void members_clear(void) {
  st.n = 0;
  st.lines.n = 0;
  st.apps.n = 0;
}

static void member_push(const char *line, const char *app, long port) {
  if (st.n == st.cap) {
    size_t cap = st.cap ? st.cap * 2 : 8;
    long *np = realloc(st.ports, cap * sizeof *np);
    if (!np) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    st.ports = np;
    st.cap = cap;
  }
  cbc_strs_push(&st.lines, cbc_dup(line));
  cbc_strs_push(&st.apps, cbc_dup(app));
  st.ports[st.n++] = port;
}

static void member_remove(size_t at) {
  for (size_t i = at; i + 1 < st.n; i++) {
    st.lines.v[i] = st.lines.v[i + 1];
    st.apps.v[i] = st.apps.v[i + 1];
    st.ports[i] = st.ports[i + 1];
  }
  st.n--;
  st.lines.n--;
  st.apps.n--;
}

/* state_die: a state file that is not the format (state.sh's state_die). */
static void state_die(const char *where, int no, const char *sentence) __attribute__((noreturn));
static void state_die(const char *where, int no, const char *sentence) {
  cbc_fail(CBC_EXIT_FAIL, "state file %s line %d %s. It is written by ./cbase alone; fix or restore it by hand only with care.", where, no, sentence);
}

/* match: the groups of RE against S (with regmatch slots), false when it does not match. */
static bool match(const char *re, const char *s, regmatch_t *m, size_t n) {
  regex_t r;
  if (regcomp(&r, re, REG_EXTENDED) != 0) cbc_fail(CBC_EXIT_FAIL, "internal: bad pattern %s", re);
  bool ok = regexec(&r, s, n, m, 0) == 0;
  regfree(&r);
  return ok;
}

static char *group_text(const char *s, const regmatch_t *m) {
  if (m->rm_so < 0) return cbc_dup("");
  return cbc_dupn(s + m->rm_so, (size_t)(m->rm_eo - m->rm_so));
}

/* state_parse: the members and header of FILE (WHERE names it in a message). */
static void state_parse(const char *kind, const char *group, const char *file, const char *where) {
  char *data;
  size_t dlen;
  members_clear();
  state_member_origin = "";
  state_member_origin_addr = "";
  if (!cbc_read_file(file, &data, &dlen)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", file);
  int no = 0;
  size_t start = 0;
  for (size_t i = 0; i <= dlen; i++) {
    if (i < dlen && data[i] != '\n') continue;
    if (i == start && i == dlen) break;
    data[i] = '\0';
    char *line = data + start;
    start = i + 1;
    no++;
    if (no == 1) {
      char hdr[512];
      if (strcmp(kind, "vm") == 0) {
        snprintf(hdr, sizeof hdr, "^cbase-state %s vm %s origin=([a-z0-9.-]+) addr=(ipv6|dualstack)$", STATE_VERSION, group);
        regmatch_t m[3];
        if (match(hdr, line, m, 3)) {
          state_member_origin = group_text(line, &m[1]);
          state_member_origin_addr = group_text(line, &m[2]);
          continue;
        }
      }
      char want[512];
      snprintf(want, sizeof want, "cbase-state %s %s %s", STATE_VERSION, kind, group);
      if (strcmp(line, want) != 0)
      {
        char sentence[600];
        snprintf(sentence, sizeof sentence, "is not \"%s\" (the format's version, the kind and the group; a vm group's may add \" origin=<host> addr=<ipv6|dualstack>\")", want);
        state_die(where, no, sentence);
      }
      continue;
    }
    char *app;
    long port;
    if (strcmp(kind, "vm") == 0) {
      regmatch_t m[8];
      if (!match("^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?) port=([0-9]{4,5}) service=([a-z0-9-]+)\\.service mem=~?([0-9]{1,6})( data=(/[A-Za-z0-9._/-]+) legacy=1)?$", line, m, 8))
        state_die(where, no, "is not \"app=<name> port=<port> service=cbase-<name>.service mem=<MiB>\" (mem=~<MiB> when estimated; \" data=<dir> legacy=1\" after it for a grandfathered member)");
      app = group_text(line, &m[1]);
      int64_t pv = 0;
      char *ps = group_text(line, &m[3]);
      num_parse_i64(ps, strlen(ps), 0, 99999, &pv);
      port = (long)pv;
      char *svc = group_text(line, &m[4]);
      char want_svc[256];
      snprintf(want_svc, sizeof want_svc, "cbase-%s", app);
      if (strcmp(svc, want_svc) != 0)
        cbc_fail(CBC_EXIT_FAIL, "state file %s line %d names service %s.service for app %s (it is cbase-%s.service). It is written by ./cbase alone; fix or restore it by hand only with care.", where, no, svc, app, app);
      if (m[6].rm_so >= 0) {
        if (!(port >= 1024 && port <= 65535 && (port < STATE_PORT_MIN || port > STATE_PORT_MAX))) {
          char msg[256];
          snprintf(msg, sizeof msg, "holds the grandfathered port %ld, which is not 1024-65535 outside %d-%d", port, STATE_PORT_MIN, STATE_PORT_MAX);
          state_die(where, no, msg);
        }
      } else if (!(port >= STATE_PORT_MIN && port <= STATE_PORT_MAX)) {
        char msg[128];
        snprintf(msg, sizeof msg, "holds port %ld, outside %d-%d", port, STATE_PORT_MIN, STATE_PORT_MAX);
        state_die(where, no, msg);
      }
      for (size_t j = 0; j < st.n; j++) {
        if (st.ports[j] == port) {
          char msg[256];
          snprintf(msg, sizeof msg, "gives port %ld to %s, and %s holds it", port, app, st.apps.v[j]);
          state_die(where, no, msg);
        }
      }
    } else {
      regmatch_t m[4];
      if (!match("^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?) folder=([a-z0-9-]+)/$", line, m, 4))
        state_die(where, no, "is not \"app=<name> folder=<name>/\"");
      app = group_text(line, &m[1]);
      char *folder = group_text(line, &m[3]);
      if (strcmp(folder, app) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "gives app %s the folder %s/ (it is %s/)", app, folder, app);
        state_die(where, no, msg);
      }
      port = -1;
    }
    for (size_t j = 0; j < st.apps.n; j++) {
      if (strcmp(st.apps.v[j], app) == 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "lists %s again", app);
        state_die(where, no, msg);
      }
    }
    member_push(line, app, port);
  }
  if (no < 1) {
    char msg[256];
    snprintf(msg, sizeof msg, "is empty (it starts \"cbase-state %s %s %s\")", STATE_VERSION, kind, group);
    state_die(where, 1, msg);
  }
}

/* state_mem: a vm member line's declared MiB (or its estimate) and whether it is the estimate. The
 * number is what follows the last " mem=", up to a blank (a grandfathered member's data follows it). */
static void state_mem(const char *line, long *mib, bool *est) {
  const char *p = NULL;
  for (const char *q = line; (q = strstr(q, " mem=")) != NULL; q += 5) p = q + 5;
  const char *m = p ? p : line;
  *est = false;
  if (*m == '~') {
    *est = true;
    m++;
  }
  char digits[16] = "";
  size_t k = 0;
  while (m[k] && m[k] != ' ' && k + 1 < sizeof digits) {
    digits[k] = m[k];
    k++;
  }
  digits[k] = '\0';
  int64_t v = 0;
  num_parse_i64(digits, k, 0, 999999, &v);
  *mib = (long)v;
}

/* state_at: the index of APP among the members, or -1. */
long state_at(const char *app) {
  for (size_t i = 0; i < st.n; i++) {
    if (strcmp(st.apps.v[i], app) == 0) return (long)i;
  }
  return -1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* sorted_copy: the lines (N of them) sorted in byte order, as LC_ALL=C sort does. */
static char **sorted_copy(char **lines, size_t n) {
  char **c = malloc((n ? n : 1) * sizeof *c);
  if (!c) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  for (size_t i = 0; i < n; i++) c[i] = lines[i];
  qsort(c, n, sizeof *c, cmp_str);
  return c;
}

/* state_fit: whether the members, with APP's line made LINE, fit in state_fit_limit MiB. Fills the
 * report, the members by name; false when they do not fit. */
bool state_fit(const char *app, const char *new_line) {
  size_t n = st.n;
  char **lines = malloc((n + 1) * sizeof *lines);
  if (!lines) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  for (size_t i = 0; i < n; i++) lines[i] = st.lines.v[i];
  long at = state_at(app);
  if (at >= 0) lines[at] = (char *)new_line;
  else lines[n] = (char *)new_line;
  size_t total_n = at >= 0 ? n : n + 1;
  char **sorted = sorted_copy(lines, total_n);
  long total = 0;
  char notes[4096] = "";
  char members[4096] = "";
  for (size_t i = 0; i < total_n; i++) {
    const char *line = sorted[i];
    const char *a = line + strlen("app=");
    size_t an = strcspn(a, " ");
    char *name = cbc_dupn(a, an);
    long mib;
    bool est;
    state_mem(line, &mib, &est);
    total += mib;
    char piece[256];
    if (est) {
      snprintf(piece, sizeof piece, "  %s: memory not declared, counted as %ldM\n", name, mib);
      strncat(notes, piece, sizeof notes - strlen(notes) - 1);
      snprintf(piece, sizeof piece, "%s%s ~%ld", members[0] ? ", " : "", name, mib);
    } else {
      snprintf(piece, sizeof piece, "%s%s %ld", members[0] ? ", " : "", name, mib);
    }
    strncat(members, piece, sizeof members - strlen(members) - 1);
  }
  char report[8192];
  snprintf(report, sizeof report, "%s  memory: %s = %ld MiB of the %ld MiB for apps (%s)", notes, members, total,
           state_fit_limit, state_fit_what);
  state_fit_report = cbc_dup(report);
  free(lines);
  free(sorted);
  return total <= state_fit_limit;
}

/* state_origin: the group's origin, from the file or from the joining app's CBASE_VM_ORIGIN (ipv6)
 * when it holds none. One VM has one origin. False when neither names one. */
bool state_origin(const char *group) {
  if (*state_origin_in) {
    if (!*state_member_origin) {
      state_member_origin = cbc_dup(state_origin_in);
      state_member_origin_addr = "ipv6";
    } else if (strcmp(state_member_origin, state_origin_in) != 0) {
      cbc_fail(CBC_EXIT_FAIL, "%s's CBASE_VM_ORIGIN is %s, and vm group %s's VM is reached at %s (its state file): one VM can't have two origins. Drop CBASE_VM_ORIGIN, or set it to %s. Nothing was changed.",
               cbc_conf.app, state_origin_in, group, state_member_origin, state_member_origin);
    }
  }
  return *state_member_origin != '\0';
}

/* state_render: the header and the members sorted by app, written to FILE. */
static void state_render(const char *kind, const char *group, const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", file);
  if (*state_member_origin) fprintf(f, "cbase-state %s %s %s origin=%s addr=%s\n", STATE_VERSION, kind, group, state_member_origin, state_member_origin_addr);
  else fprintf(f, "cbase-state %s %s %s\n", STATE_VERSION, kind, group);
  char **sorted = sorted_copy(st.lines.v, st.n);
  for (size_t i = 0; i < st.n; i++) fprintf(f, "%s\n", sorted[i]);
  free(sorted);
  fclose(f);
}

/* state_port: the port APP's join holds: a grandfathered member's own (LEGACY), the one a member holds,
 * or the lowest one in the range no member holds. Stops the tool when every one is held. */
const char *state_port_of(const char *group, const char *legacy, long at) {
  if (*legacy) return legacy;
  static char buf[16];
  if (at >= 0) {
    const char *line = st.lines.v[at];
    size_t ln = strlen(line);
    bool is_legacy = ln >= strlen(" legacy=1") && strcmp(line + ln - strlen(" legacy=1"), " legacy=1") == 0;
    if (!is_legacy) {
      snprintf(buf, sizeof buf, "%ld", st.ports[at]);
      return buf;
    }
  }
  for (long p = STATE_PORT_MIN; p <= STATE_PORT_MAX; p++) {
    bool held = false;
    for (size_t i = 0; i < st.n; i++) held = held || st.ports[i] == p;
    if (!held) {
      snprintf(buf, sizeof buf, "%ld", p);
      return buf;
    }
  }
  cbc_fail(CBC_EXIT_FAIL, "vm group %s has no free port: all of %d-%d are held", group, STATE_PORT_MIN, STATE_PORT_MAX);
}

/* state_read: the members and the ETag of the group's file ("*" when there is none yet). */
void state_read(const char *kind, const char *group) {
  char *bucket_s;
  char bucket[64];
  snprintf(bucket, sizeof bucket, "cbase-state-%s", aws_account);
  bucket_s = bucket;
  char key[256];
  snprintf(key, sizeof key, "%s/%s.txt", kind, group);
  char *cur = cbc_tmp_path("cur");
  unlink(cur);
  char *etag = NULL;
  /* STATE_NO_BUCKET=1 reads no file at all: the same as a missing one (shell state_read). */
  int rc = state_no_bucket ? 3 : aws_s3_get(bucket_s, key, cur, &etag);
  if (rc == 3) {
    state_etag = "*";
    members_clear();
    state_member_origin = "";
    state_member_origin_addr = "";
    return;
  }
  state_etag = etag;
  char where[512];
  snprintf(where, sizeof where, "s3://%s/%s", bucket_s, key);
  state_parse(kind, group, cur, where);
}

static void state_backoff(int attempt, const char *app) {
  long ms = 100L * (1L << (attempt - 1));
  if (ms > 2000) ms = 2000;
  ms += (long)(cbc_cksum(app) % 100);
  cbc_sleep_ms(ms);
}

void state_change(const char *kind, const char *group, const char *app, const char *mem, const char *op) {
  char bucket[64], key[256];
  snprintf(bucket, sizeof bucket, "cbase-state-%s", aws_account);
  snprintf(key, sizeof key, "%s/%s.txt", kind, group);
  bool leaving = strcmp(op, "leave") == 0;
  for (int attempt = 1; attempt <= STATE_ATTEMPTS; attempt++) {
    state_read(kind, group);
    long at = state_at(app);
    char *line = NULL;
    char *old_origin = "";
    if (leaving) {
      if (at < 0) {
        printf("%s is not in %s group %s: nothing to leave\n", app, kind, group);
        return;
      }
      line = cbc_dup(st.lines.v[at]);
      member_remove((size_t)at);
    } else {
      const char *port = "";
      if (strcmp(kind, "vm") == 0) {
        old_origin = state_member_origin;
        if (!state_origin(group) && state_origin_need) {
          cbc_fail(CBC_EXIT_FAIL, "vm group %s has no origin recorded and %s names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once, in this app's cbase.conf. Nothing was changed.", group, app);
        }
        port = state_port_of(group, state_legacy_port, at);
        state_port = cbc_dup(port);
        if (*state_legacy_port) {
          for (size_t i = 0; i < st.n; i++) {
            if ((long)i == at) continue;
            char buf[16];
            snprintf(buf, sizeof buf, "%ld", st.ports[i]);
            if (strcmp(buf, port) == 0) {
              fprintf(stderr, "cbase: %s's port %s (CBASE_VM_PORT) is %s's in vm group %s: nothing was changed\n", app, port, st.apps.v[i], group);
              exit(CBC_EXIT_FAIL);
            }
          }
        }
        char *vmline;
        {
          size_t need = strlen(app) + strlen(port) + strlen(mem) + strlen(state_legacy_data) + 96;
          vmline = malloc(need);
          if (!vmline) cbc_fail(CBC_EXIT_FAIL, "out of memory");
          snprintf(vmline, need, "app=%s port=%s service=cbase-%s.service mem=%s", app, port, app, mem);
          if (*state_legacy_data) {
            strncat(vmline, " data=", need - strlen(vmline) - 1);
            strncat(vmline, state_legacy_data, need - strlen(vmline) - 1);
            strncat(vmline, " legacy=1", need - strlen(vmline) - 1);
          }
        }
        line = vmline;
        if (state_fit_limit >= 0 && !state_fit(app, line)) {
          fprintf(stderr, "%s\n", state_fit_report);
          cbc_fail(CBC_EXIT_FAIL, "%s does not fit on vm group %s: its members would need more than the %ld MiB its instance leaves for apps (%s). Declare less memory (CBASE_MEMORY), put the app in another group, or give the group a bigger instance (./cbase group-init vm %s --type <type>). Nothing was changed.",
                   app, group, state_fit_limit, state_fit_what, group);
        }
      } else {
        size_t need = strlen(app) * 2 + 16;
        line = malloc(need);
        if (!line) cbc_fail(CBC_EXIT_FAIL, "out of memory");
        snprintf(line, need, "app=%s folder=%s/", app, app);
      }
      state_joined_new = at < 0;
      if (at >= 0 && strcmp(st.lines.v[at], line) == 0 && strcmp(old_origin, state_member_origin) == 0) {
        printf("%s is in %s group %s: %s\n", app, kind, group, line);
        if (state_fit_limit >= 0 && strcmp(kind, "vm") == 0) printf("%s\n", state_fit_report);
        return;
      }
      if (at >= 0) {
        st.lines.v[at] = line;
      } else {
        int64_t port_v = -1;
        if (strcmp(kind, "vm") == 0) num_parse_i64(port, strlen(port), 0, 99999, &port_v);
        long port_n = (long)port_v;
        member_push(line, app, port_n);
      }
    }
    char new_path[512];
    snprintf(new_path, sizeof new_path, "%s", cbc_tmp_path("new"));
    state_render(kind, group, new_path);
    int rc = aws_s3_put(bucket, key, new_path, state_etag);
    if (rc == 0) {
      if (leaving) {
        printf("%s left %s group %s (it held: %s)\n", app, kind, group, line);
      } else {
        printf("%s joined %s group %s: %s\n", app, kind, group, line);
        if (state_fit_limit >= 0 && strcmp(kind, "vm") == 0) printf("%s\n", state_fit_report);
      }
      return;
    }
    fprintf(stderr, "cbase: s3://%s/%s changed while %s's %s was being written (attempt %d of %d): reading it again\n",
            bucket, key, app, op, attempt, STATE_ATTEMPTS);
    if (attempt != STATE_ATTEMPTS) state_backoff(attempt, app);
  }
  cbc_fail(CBC_EXIT_FAIL, "s3://%s/%s kept changing: %s's %s lost %d conditional writes in a row; run it again", bucket, key, app, op, STATE_ATTEMPTS);
}

int state_list(const char *kind, const char *group) {
  state_read(kind, group);
  if (strcmp(state_etag, "*") == 0) return 3;
  char bucket[64], key[256];
  snprintf(bucket, sizeof bucket, "cbase-state-%s", aws_account);
  snprintf(key, sizeof key, "%s/%s.txt", kind, group);
  printf("%s group %s (s3://%s/%s): %zu app%s\n", kind, group, bucket, key, st.n, st.n == 1 ? "" : "s");
  if (st.n == 0) return 0;
  if (strcmp(kind, "vm") == 0) printf("  %-24s %-5s %-34s %s\n", "app", "port", "service", "mem");
  else printf("  %-24s %s\n", "app", "folder");
  char **sorted = sorted_copy(st.lines.v, st.n);
  for (size_t i = 0; i < st.n; i++) {
    const char *line = sorted[i];
    char *f[8] = { 0 };
    size_t nf = 0;
    char *copy = cbc_dup(line);
    for (char *w = strtok(copy, " "); w && nf < 8; w = strtok(NULL, " ")) f[nf++] = w;
    if (strcmp(kind, "vm") == 0) {
      long mib;
      bool est;
      state_mem(line, &mib, &est);
      char m[64];
      if (est) snprintf(m, sizeof m, "~%ld counted", mib);
      else snprintf(m, sizeof m, "%ldM", mib);
      printf("  %-24s %-5s %-34s %s\n", f[0] + 4, f[1] + 5, f[2] + 8, m);
    } else {
      printf("  %-24s %s\n", f[0] + 4, f[1] + 7);
    }
  }
  free(sorted);
  return 0;
}

char *state_bucket(void) {
  char *b = malloc(64);
  if (!b) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  snprintf(b, 64, "cbase-state-%s", aws_account);
  return b;
}

char *state_key(const char *kind, const char *group) {
  size_t need = strlen(kind) + strlen(group) + 8;
  char *k = malloc(need);
  if (!k) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  snprintf(k, need, "%s/%s.txt", kind, group);
  return k;
}

char *state_vm_line(const char *app, const char *port, const char *mem, const char *legacy_data) {
  size_t need = strlen(app) + strlen(port) + strlen(mem) + strlen(legacy_data) + 96;
  char *line = malloc(need);
  if (!line) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  snprintf(line, need, "app=%s port=%s service=cbase-%s.service mem=%s", app, port, app, mem);
  if (*legacy_data) {
    strncat(line, " data=", need - strlen(line) - 1);
    strncat(line, legacy_data, need - strlen(line) - 1);
    strncat(line, " legacy=1", need - strlen(line) - 1);
  }
  return line;
}
