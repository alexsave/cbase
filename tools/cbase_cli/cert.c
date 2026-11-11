/* cert.c - the certificate and the DNS records for an app's hostnames (cert.h). cert.sh's header says the
 * order of the steps and why each stop is where it is; the messages are cert.sh's words.
 */
#include "cert.h"

#include "aws.h"
#include "cli.h"
#include "conf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CERT_REGION "us-east-1"
#define CERT_POLL_TRIES 61

/* aws_call_argv: aws_call with an argument vector, for a call whose arguments are a list (the SANs of a
 * request). Same retries, output and error file as aws_call. NEEDED FROM aws.h (the interface commit has
 * the variadic form only). */
int aws_call_argv(cbc_buf *out, char *const *argv);

/* ---- small helpers ---------------------------------------------------------------------------- */

static char *text_of(const cbc_buf *b) {
  if (!b->data) return cbc_dup("");
  size_t n = b->len;
  while (n > 0 && b->data[n - 1] == '\n') n--;
  return cbc_dupn(b->data, n);
}

static char *fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static char *fmt(const char *f, ...) {
  char *s = NULL;
  va_list ap;
  va_start(ap, f);
  int n = vasprintf(&s, f, ap);
  va_end(ap);
  if (n < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return s;
}

static void push(cbc_strs *s, char *v) {
  if (s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    char **nv = realloc(s->v, cap * sizeof *nv);
    if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    s->v = nv;
    s->cap = cap;
  }
  s->v[s->n++] = v;
}

/* words: the blank-separated words of TEXT (read -a splits them so). */
static void words(const char *text, cbc_strs *out) {
  const char *p = text;
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) break;
    const char *b = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
    push(out, cbc_dupn(b, (size_t)(p - b)));
  }
}

/* joined: the strings of S with SEP between them. */
static char *joined(const cbc_strs *s, const char *sep) {
  char *out = cbc_dup("");
  for (size_t i = 0; i < s->n; i++) {
    char *g = fmt("%s%s%s", out, i ? sep : "", s->v[i]);
    free(out);
    out = g;
  }
  return out;
}

static bool contains(const cbc_strs *s, const char *v) {
  for (size_t i = 0; i < s->n; i++)
    if (strcmp(s->v[i], v) == 0) return true;
  return false;
}

static void free_strs(cbc_strs *s) {
  for (size_t i = 0; i < s->n; i++) free(s->v[i]);
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static int by_bytes(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* cksum_of: POSIX cksum's CRC of the N bytes of S, the number cksum prints (the idempotency token takes it). */
static unsigned long cksum_of(const char *s, size_t n) {
  unsigned long table[256];
  for (unsigned long i = 0; i < 256; i++) {
    unsigned long c = i << 24;
    for (int k = 0; k < 8; k++) c = (c & 0x80000000UL) ? (((c << 1) ^ 0x04C11DB7UL) & 0xFFFFFFFFUL) : ((c << 1) & 0xFFFFFFFFUL);
    table[i] = c;
  }
  unsigned long crc = 0;
  for (size_t i = 0; i < n; i++) crc = ((crc << 8) & 0xFFFFFFFFUL) ^ table[((crc >> 24) ^ (unsigned char)s[i]) & 0xFF];
  for (size_t len = n; len; len >>= 8) crc = ((crc << 8) & 0xFFFFFFFFUL) ^ table[((crc >> 24) ^ (len & 0xFF)) & 0xFF];
  return ~crc & 0xFFFFFFFFUL;
}

/* ---- the certificate -------------------------------------------------------------------------- */

/* cert_names: the certificate's names (its domain among them), in NAMES. */
static void cert_names(const char *arn, cbc_strs *names) {
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", arn,
               "--query", "Certificate.SubjectAlternativeNames", "--output", "text", NULL) != 0)
    aws_fail(fmt("reading certificate %s", arn));
  char *t = text_of(&out);
  words(t, names);
  free(t);
  free(out.data);
}

bool cbc_cert_covers(const char *host, const cbc_strs *names) {
  const char *dot = strchr(host, '.');
  for (size_t i = 0; i < names->n; i++) {
    const char *n = names->v[i];
    if (strcmp(n, host) == 0) return true;
    /* A wildcard covers one label: its rest is the hostname less its first label. */
    if (strncmp(n, "*.", 2) == 0 && dot && strcmp(n + 2, dot + 1) == 0) return true;
  }
  return false;
}

/* covers_all: the certificate covers every one of NEED. A failed read stops the tool. */
static bool covers_all(const char *arn, const cbc_strs *need) {
  cbc_strs names = { 0, 0, 0 };
  cert_names(arn, &names);
  bool ok = names.n > 0;
  for (size_t i = 0; ok && i < need->n; i++) ok = cbc_cert_covers(need->v[i], &names);
  free_strs(&names);
  return ok;
}

/* cert_find: the first certificate of STATUS, by ARN, that covers every hostname in NEED; "" when none. */
static char *cert_find(const char *status, const cbc_strs *need) {
  cbc_buf out = { 0, 0, 0 };
  /* Every key type CloudFront serves (ListCertificates names RSA_2048 alone unless asked). */
  if (aws_call(&out, "acm", "--region", CERT_REGION, "list-certificates", "--certificate-statuses", status,
               "--includes", "keyTypes=RSA_2048,EC_prime256v1", "--query", "CertificateSummaryList[].CertificateArn",
               "--output", "text", NULL) != 0)
    aws_fail("listing ACM certificates");
  char *t = text_of(&out);
  free(out.data);
  cbc_strs arns = { 0, 0, 0 };
  words(t, &arns);
  free(t);
  size_t kept = 0;
  for (size_t i = 0; i < arns.n; i++) {
    if (strcmp(arns.v[i], "None") == 0) free(arns.v[i]);
    else arns.v[kept++] = arns.v[i];
  }
  arns.n = kept;
  qsort(arns.v, arns.n, sizeof *arns.v, by_bytes);
  char *found = cbc_dup("");
  for (size_t i = 0; i < arns.n; i++) {
    if (covers_all(arns.v[i], need)) {
      free(found);
      found = cbc_dup(arns.v[i]);
      break;
    }
  }
  free_strs(&arns);
  return found;
}

static char *cert_status_of(const char *arn) {
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", arn,
               "--query", "Certificate.Status", "--output", "text", NULL) != 0)
    aws_fail(fmt("reading certificate %s", arn));
  char *s = text_of(&out);
  free(out.data);
  return s;
}

/* zone_for_host: HOST's hosted zone id (CBASE_ZONE, or the public zone named by the longest suffix of
 * HOST), or "". Two public zones of one name stop the tool: CBASE_ZONE says which. */
static char *zone_for_host(const char *host) {
  const cbc_conf_t *conf = &cbc_conf;
  if (conf->zone && *conf->zone) return cbc_dup(conf->zone);
  char *s = cbc_dup(host);
  while (strchr(s, '.')) {
    char *query = fmt("HostedZones[?Name=='%s.' && Config.PrivateZone==`false`].Id", s);
    cbc_buf out = { 0, 0, 0 };
    if (aws_call(&out, "route53", "list-hosted-zones-by-name", "--dns-name", s, "--max-items", "10",
                 "--query", query, "--output", "text", NULL) != 0)
      aws_fail(fmt("looking for a Route 53 hosted zone for %s", s));
    char *t = text_of(&out);
    free(out.data);
    free(query);
    cbc_strs ids = { 0, 0, 0 };
    words(t, &ids);
    free(t);
    if (ids.n > 0 && strcmp(ids.v[0], "None") != 0) {
      if (ids.n > 1) {
        fprintf(stderr, "cbase: %zu public Route 53 hosted zones are named %s: set CBASE_ZONE in cbase.conf to the one that serves the domain\n", ids.n, s);
        exit(CBC_EXIT_FAIL);
      }
      const char *slash = strrchr(ids.v[0], '/');
      char *zone = cbc_dup(slash ? slash + 1 : ids.v[0]);
      free_strs(&ids);
      free(s);
      return zone;
    }
    free_strs(&ids);
    memmove(s, strchr(s, '.') + 1, strlen(strchr(s, '.') + 1) + 1);
  }
  free(s);
  return cbc_dup("");
}

/* ---- validation records ----------------------------------------------------------------------- */

/* A validation record: the hostname it proves, and the CNAME ACM wants at NAME. */
typedef struct {
  char *host, *name, *type, *value;
} vrecord;

static char *poll_arn;   /* the certificate the poll reads */
static char *poll_text;  /* its last read */

/* split_tabs: LINE's tab-separated fields in F (up to N); a missing one is NULL. Edits LINE in place. */
static void split_tabs(char *line, char **f, int n) {
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

/* records_read: one read of the validation records; true once every record is there. */
static bool records_read(void) {
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", poll_arn,
               "--query", "Certificate.DomainValidationOptions[].[DomainName,ResourceRecord.Name,ResourceRecord.Type,ResourceRecord.Value]",
               "--output", "text", NULL) != 0)
    aws_fail(fmt("reading certificate %s's validation records", poll_arn));
  free(poll_text);
  poll_text = text_of(&out);
  free(out.data);
  bool missing = strcmp(poll_text, "None") == 0;
  char *copy = cbc_dup(poll_text);
  for (char *line = copy; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    char *f[4];
    split_tabs(line, f, 4);
    /* read -r _ name _ value: a line with no name is skipped; a None name or value is not there yet */
    if (f[1] && *f[1] && (strcmp(f[1], "None") == 0 || (f[3] && strcmp(f[3], "None") == 0))) missing = true;
    if (!eol) break;
    line = eol + 1;
  }
  free(copy);
  return !missing && *poll_text;
}

/* records: the validation records of ARN, once each by name, waiting (a while) for ACM to make them. */
static void records(const char *arn, vrecord **out_v, size_t *out_n) {
  poll_arn = cbc_dup(arn);
  if (!aws_poll(CERT_POLL_TRIES, records_read))
    cbc_fail(CBC_EXIT_FAIL, "ACM has not made certificate %s's validation records yet: deploy again in a minute", arn);
  vrecord *v = NULL;
  size_t n = 0, cap = 0;
  char *copy = cbc_dup(poll_text);
  for (char *line = copy; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    char *f[4];
    split_tabs(line, f, 4);
    if (f[1] && *f[1]) {
      bool seen = false;
      for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, f[1]) == 0) seen = true;
      if (!seen) {
        if (n == cap) {
          cap = cap ? cap * 2 : 8;
          v = realloc(v, cap * sizeof *v);
          if (!v) cbc_fail(CBC_EXIT_FAIL, "out of memory");
        }
        v[n].host = cbc_dup(f[0]);
        v[n].name = cbc_dup(f[1]);
        v[n].type = cbc_dup(f[2] ? f[2] : "");
        v[n].value = cbc_dup(f[3] ? f[3] : "");
        n++;
      }
    }
    if (!eol) break;
    line = eol + 1;
  }
  free(copy);
  *out_v = v;
  *out_n = n;
}

/* r53_change: one change batch in ZONE (the CLI's shorthand), waited for. */
static void r53_change(const char *zone, const char *batch, const char *what) {
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "route53", "change-resource-record-sets", "--hosted-zone-id", zone, "--change-batch", batch,
               "--query", "ChangeInfo.Id", "--output", "text", NULL) != 0)
    aws_fail(fmt("writing %s in Route 53 zone %s", what, zone));
  char *id = text_of(&out);
  free(out.data);
  if (aws_call(NULL, "route53", "wait", "resource-record-sets-changed", "--id", id, NULL) != 0)
    aws_fail(fmt("waiting for Route 53 change %s", id));
  free(id);
}

/* zone_of: the zone cert_resolve found for HOST, or "". */
static const char *zone_of(const cbc_cert_t *c, const char *host) {
  for (size_t i = 0; i < c->hosts.n; i++)
    if (strcmp(c->hosts.v[i], host) == 0) return c->zones.v[i];
  return "";
}

/* ---- the steps -------------------------------------------------------------------------------- */

/* named: the certificate a key names, used as it is: ISSUED, and covering NEED. */
static void named(const char *arn, const char *key, const cbc_strs *need) {
  char *st = cert_status_of(arn);
  if (strcmp(st, "ISSUED") != 0) {
    fprintf(stderr, "cbase: %s %s is %s, not ISSUED\n", key, arn, st);
    exit(CBC_EXIT_FAIL);
  }
  free(st);
  if (!covers_all(arn, need)) {
    cbc_strs names = { 0, 0, 0 };
    cert_names(arn, &names);
    char *all = cbc_dup("");
    for (size_t i = 0; i < names.n; i++) {
      char *g = fmt("%s%s ", all, names.v[i]);
      free(all);
      all = g;
    }
    char *want = joined(need, " ");
    fprintf(stderr, "cbase: %s %s does not cover %s (its names: %s)\n", key, arn, want, all);
    exit(CBC_EXIT_FAIL);
  }
  printf("certificate: %s (%s, used as it is)\n", arn, key);
}

/* auto_cert: the issued certificate covering NEED, else the pending one, else a new request (the DNS
 * records are written where a zone holds them, printed where none does, and ACM is waited for). A dry
 * run requests nothing. Stops the tool (status 3) for records to add by hand. */
static void auto_cert(cbc_cert_t *c, bool dry) {
  char *issued = cert_find("ISSUED", &c->need);
  if (*issued) {
    free(c->arn);
    c->arn = issued;
    char *need = joined(&c->need, " ");
    printf("certificate: %s, issued, covers %s\n", c->arn, need);
    free(need);
    return;
  }
  free(issued);
  char *a = cert_find("PENDING_VALIDATION", &c->need);
  if (*a) {
    printf("certificate: %s, requested before, still pending validation\n", a);
  } else if (dry) {
    char *need = joined(&c->need, " ");
    printf("certificate: none covers %s; a deploy would request one (DNS validation). This dry run goes on without the hostnames.\n", need);
    free(need);
    free(a);
    return;
  } else {
    /* The request: DNS validation for the first hostname, the others as SANs. ACM's idempotency token
     * makes the same app's request for the same hostnames within the hour one request. */
    cbc_strs args = { 0, 0, 0 };
    push(&args, cbc_dup("acm"));
    push(&args, cbc_dup("--region"));
    push(&args, cbc_dup(CERT_REGION));
    push(&args, cbc_dup("request-certificate"));
    push(&args, cbc_dup("--domain-name"));
    push(&args, cbc_dup(c->need.v[0]));
    push(&args, cbc_dup("--validation-method"));
    push(&args, cbc_dup("DNS"));
    if (c->need.n != 1) {
      push(&args, cbc_dup("--subject-alternative-names"));
      for (size_t i = 1; i < c->need.n; i++) push(&args, cbc_dup(c->need.v[i]));
    }
    char *seed = fmt("%s %s", cbc_conf.app, joined(&c->need, " "));
    push(&args, cbc_dup("--idempotency-token"));
    push(&args, fmt("cbase%lu", cksum_of(seed, strlen(seed))));
    free(seed);
    push(&args, cbc_dup("--query"));
    push(&args, cbc_dup("CertificateArn"));
    push(&args, cbc_dup("--output"));
    push(&args, cbc_dup("text"));
    push(&args, NULL);
    cbc_buf out = { 0, 0, 0 };
    if (aws_call_argv(&out, args.v) != 0) {
      char *need = joined(&c->need, " ");
      aws_fail(fmt("requesting a certificate for %s", need));
    }
    char *arn = text_of(&out);
    free(out.data);
    free_strs(&args);
    char *need = joined(&c->need, " ");
    printf("certificate: requested %s for %s (DNS validation)\n", arn, need);
    free(need);
    free(a);
    a = arn;
  }
  vrecord *rec = NULL;
  size_t nrec = 0;
  records(a, &rec, &nrec);
  cbc_strs hand = { 0, 0, 0 };
  for (size_t i = 0; i < nrec; i++) {
    const char *z = zone_of(c, rec[i].host);
    if (*z && !dry) {
      char *batch = fmt("Changes=[{Action=UPSERT,ResourceRecordSet={Name=%s,Type=%s,TTL=300,ResourceRecords=[{Value=%s}]}}]",
                        rec[i].name, rec[i].type, rec[i].value);
      char *what = fmt("the validation record for %s", rec[i].host);
      r53_change(z, batch, what);
      printf("dns: wrote %s %s %s in zone %s (validates the certificate for %s)\n", rec[i].name, rec[i].type, rec[i].value, z, rec[i].host);
      free(batch);
      free(what);
    } else {
      push(&hand, fmt("%s %s %s %s", rec[i].name, rec[i].type, rec[i].value, rec[i].host));
    }
  }
  if (hand.n > 0) {
    if (dry) printf("dns: the certificate's validation records (a deploy writes those with a zone, and stops on the rest):\n");
    else printf("cbase: add these DNS records where the domain's DNS is served (no Route 53 zone in this account covers them):\n");
    for (size_t i = 0; i < hand.n; i++) {
      char *name = NULL, *type = NULL, *value = NULL, *host = NULL;
      char *line = cbc_dup(hand.v[i]);
      name = line;
      type = strchr(name, ' ') + 1;
      *(type - 1) = '\0';
      value = strchr(type, ' ') + 1;
      *(value - 1) = '\0';
      host = strchr(value, ' ') + 1;
      *(host - 1) = '\0';
      printf("  %s  %s  %s   (proves the certificate for %s)\n", name, type, value, host);
      free(line);
    }
    free_strs(&hand);
    if (dry) {
      printf("dns: this dry run goes on without the hostnames\n");
      free(a);
      return;
    }
    printf("then run ./cbase deploy again: once ACM has issued the certificate (minutes after the records are visible) it goes on, and prints the record that points each hostname at the app. Nothing else was changed.\n");
    exit(3);
  }
  free_strs(&hand);
  if (dry) {
    printf("dns: this dry run goes on without the hostnames (the certificate is pending)\n");
    free(a);
    return;
  }
  printf("certificate: waiting for ACM to validate %s\n", a);
  if (aws_call(NULL, "acm", "--region", CERT_REGION, "wait", "certificate-validated", "--certificate-arn", a, NULL) != 0)
    aws_fail(fmt("waiting for certificate %s to be issued", a));
  free(c->arn);
  c->arn = a;
  printf("certificate: %s, issued\n", c->arn);
}

void cbc_cert_resolve(cbc_cert_t *c, bool dry) {
  const cbc_conf_t *conf = &cbc_conf;
  free(c->arn);
  free(c->api_arn);
  c->arn = cbc_dup("");
  c->api_arn = cbc_dup("");
  free_strs(&c->need);
  free_strs(&c->hosts);
  free_strs(&c->zones);
  if (!*conf->domain) return;
  for (size_t i = 0; i < conf->domains.n; i++) push(&c->hosts, cbc_dup(conf->domains.v[i]));
  if (*conf->api_host) push(&c->hosts, cbc_dup(conf->api_host));
  for (size_t i = 0; i < c->hosts.n; i++) {
    char *z = zone_for_host(c->hosts.v[i]);
    push(&c->zones, z);
    if (*z) printf("dns: %s is in Route 53 zone %s\n", c->hosts.v[i], z);
    else printf("dns: no Route 53 hosted zone in this account covers %s\n", c->hosts.v[i]);
  }
  if (*conf->api_cert_arn) {
    cbc_strs need = { 0, 0, 0 };
    push(&need, cbc_dup(conf->api_host));
    named(conf->api_cert_arn, "CBASE_API_CERT_ARN", &need);
    free_strs(&need);
    free(c->api_arn);
    c->api_arn = cbc_dup(conf->api_cert_arn);
  }
  for (size_t i = 0; i < conf->domains.n; i++) push(&c->need, cbc_dup(conf->domains.v[i]));
  if (*conf->api_host && !*conf->api_cert_arn) push(&c->need, cbc_dup(conf->api_host));
  if (*conf->cert_arn) {
    named(conf->cert_arn, "CBASE_CERT_ARN", &c->need);
    free(c->arn);
    c->arn = cbc_dup(conf->cert_arn);
    if (!*c->api_arn) {
      free(c->api_arn);
      c->api_arn = cbc_dup(c->arn);
    }
    return;
  }
  auto_cert(c, dry);
  if (*c->arn) {
    if (!*c->api_arn) {
      free(c->api_arn);
      c->api_arn = cbc_dup(c->arn);
    }
  } else {
    free(c->api_arn);
    c->api_arn = cbc_dup("");
  }
}

/* cert_txt: HOST's TXT record name, CloudFront's proof of ownership: "_" before a name of three labels or
 * more, "_." before an apex. */
static char *cert_txt(const char *host) {
  size_t dots = 0;
  for (const char *p = host; *p; p++)
    if (*p == '.') dots++;
  return dots >= 2 ? fmt("_%s", host) : fmt("_.%s", host);
}

bool cbc_cert_alias_check(cbc_cert_t *c, const char *to_main, const char *to_api, const cbc_strs *own, bool dry) {
  free_strs(&c->moves);
  if (c->hosts.n == 0 || strcmp(cbc_conf.aliases, "off") == 0) return false;
  cbc_buf out = { 0, 0, 0 };
  /* shell's JMESPath: the distributions that have alternate names, as id, domain and names joined by ','. */
  if (aws_call(&out, "cloudfront", "list-distributions",
               "--query", "DistributionList.Items[?Aliases.Quantity > `0`].[Id,DomainName,join(`,`,Aliases.Items)]",
               "--output", "text", NULL) != 0)
    aws_fail("listing the account's CloudFront distributions");
  char *text = text_of(&out);
  free(out.data);
  bool found = false;
  for (char *line = text; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    char *f[3];
    split_tabs(line, f, 3);
    const char *id = f[0] ? f[0] : "", *dom = f[1] ? f[1] : "", *aliases = f[2] ? f[2] : "";
    if (*id && strcmp(id, "None") != 0 && !contains(own, id)) {
      char *names = cbc_dup(aliases);
      for (size_t hi = 0; hi < c->hosts.n; hi++) {
        const char *h = c->hosts.v[hi];
        bool api = *cbc_conf.api_host && strcmp(h, cbc_conf.api_host) == 0;
        for (char *a = names; a;) {
          char *comma = strchr(a, ',');
          if (comma) *comma = '\0';
          if (strcmp(a, h) == 0) {
            const char *to = api ? to_api : to_main;
            if (*to) {
              push(&c->moves, fmt("%s %s %s", h, id, to));
              printf("attach: %s moves from the distribution %s (%s) to this app's %s (associate-alias: atomic, no downtime)%s\n",
                     h, id, dom, to, dry ? "; a dry run makes no move" : "");
            } else {
              found = true;
              fprintf(stderr, "cbase: %s is an alternate domain name of the distribution %s (%s), which is not this app's, and CloudFront serves a hostname from one distribution alone (CNAMEAlreadyExists).\n", h, id, dom);
              fprintf(stderr, "  free it: remove %s from %s's alternate domain names (in the stack that made %s, or in the CloudFront console), then deploy again\n", h, id, id);
              fprintf(stderr, "  or move it with no downtime (a staged attach): ./cbase deploy --no-aliases, set the DNS records it prints, then ./cbase deploy --attach\n");
            }
          }
          a = comma ? comma + 1 : NULL;
        }
      }
      free(names);
    }
    if (!eol) break;
    line = eol + 1;
  }
  free(text);
  return found;
}

void cbc_cert_attach(const cbc_cert_t *c) {
  for (size_t i = 0; i < c->moves.n; i++) {
    char h[256], from[64], to[64];
    if (sscanf(c->moves.v[i], "%255s %63s %63s", h, from, to) != 3) continue;
    if (aws_call(NULL, "cloudfront", "associate-alias", "--target-distribution-id", to, "--alias", h, NULL) != 0) {
      char *txt = cert_txt(h);
      cbc_buf dn = { 0, 0, 0 };
      char *dom;
      if (aws_call(&dn, "cloudfront", "get-distribution", "--id", to, "--query", "Distribution.DomainName", "--output", "text", NULL) == 0) {
        dom = text_of(&dn);
      } else {
        dom = fmt("<%s's cloudfront.net name>", to);
      }
      free(dn.data);
      fprintf(stderr, "cbase: add TXT %s = %s, then run ./cbase deploy --attach again\n", txt, dom);
      free(dom);
      free(txt);
      aws_fail(fmt("moving %s from the distribution %s to %s (associate-alias)", h, from, to));
    }
    printf("attach: %s moved from %s to %s\n", h, from, to);
  }
}

void cbc_cert_point(cbc_cert_t *c, const char *host, const char *target, const char *cf_zone_id) {
  if (strcmp(cbc_conf.aliases, "off") == 0) {
    char *txt = cert_txt(host);
    printf("point %s at %s (a CNAME; an ALIAS or flattened record at a domain's apex)\n", host, target);
    printf("  and add: %s TXT %s (CloudFront's proof of ownership, which deploy --attach needs)\n", txt, target);
    free(txt);
    return;
  }
  const char *z = zone_of(c, host);
  if (*z) {
    char *alias = fmt("AliasTarget={HostedZoneId=%s,DNSName=%s,EvaluateTargetHealth=false}", cf_zone_id, target);
    char *batch = fmt("Changes=[{Action=UPSERT,ResourceRecordSet={Name=%s,Type=A,%s}},{Action=UPSERT,ResourceRecordSet={Name=%s,Type=AAAA,%s}}]",
                      host, alias, host, alias);
    char *what = fmt("the alias for %s", host);
    r53_change(z, batch, what);
    printf("dns: %s is an alias (A and AAAA) of %s in zone %s\n", host, target, z);
    free(alias);
    free(batch);
    free(what);
  } else {
    push(&c->notes, fmt("note: point %s at %s where its DNS is served, if it isn't already: a CNAME (an ALIAS or ANAME record at a domain's apex)", host, target));
  }
}
