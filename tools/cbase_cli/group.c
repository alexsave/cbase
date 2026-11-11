/* group.c - a group's own stack: group-init, group-destroy and group-template (group.h; group.sh's header
 * says what each command makes and why its refusals are where they are). The messages are group.sh's words.
 */
#include "group.h"

#include "aws.h"
#include "cli.h"
#include "conf.h"
#include "stack.h"
#include "state.h"
#include "vm.h"
#include "vm_remote.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GROUP_AMI_PARAM "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64"
#define GROUP_PREFIX_LIST "com.amazonaws.global.ipv6.cloudfront.origin-facing"
#define GROUP_PREFIX_LIST_IPV4 "com.amazonaws.global.cloudfront.origin-facing"
#define GROUP_TYPE_DEFAULT "t4g.nano"

/* ---- small helpers ---------------------------------------------------------------------------- */

/* text_of: a captured output with its trailing newlines taken off, as $(...) does. */
static char *text_of(const cbc_buf *b) {
  if (!b->data) return cbc_dup("");
  size_t n = b->len;
  while (n > 0 && b->data[n - 1] == '\n') n--;
  return cbc_dupn(b->data, n);
}


/* replace_all: S with every FROM replaced by TO, as a new string. */
static char *replace_all(const char *s, const char *from, const char *to) {
  size_t fl = strlen(from), tl = strlen(to);
  size_t cap = strlen(s) + 1, len = 0;
  char *out = cbc_xmalloc(cap);
  for (const char *p = s; *p;) {
    if (strncmp(p, from, fl) == 0 && fl > 0) {
      if (len + tl + 1 > cap) {
        cap = (len + tl + 1) * 2;
        out = cbc_xrealloc(out, cap);
      }
      memcpy(out + len, to, tl);
      len += tl;
      p += fl;
    } else {
      if (len + 2 > cap) {
        cap *= 2;
        out = cbc_xrealloc(out, cap);
      }
      out[len++] = *p++;
    }
  }
  out[len] = '\0';
  return out;
}

/* group_stack: the stack's name, cbase-group-<kind>-<group>. */
static char *group_stack(const char *kind, const char *group) { return cbc_fmt("cbase-group-%s-%s", kind, group); }

/* group_cost: a sentence of what the instance type costs a month (us-east-1 on-demand Linux, 730 h). */
static char *group_cost(const char *type) {
  static const struct {
    const char *type;
    int month;
  } table[] = {
    { "t4g.nano", 3 },   { "t4g.micro", 6 },   { "t4g.small", 12 }, { "t4g.medium", 25 },
    { "t4g.large", 49 }, { "t4g.xlarge", 98 }, { "t4g.2xlarge", 196 },
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(table[i].type, type) == 0)
      return cbc_fmt("a %s runs about $%d/month (us-east-1 on-demand), plus about $0.64/month for its 8 GB disk", type, table[i].month);
  }
  return cbc_fmt("a %s: see https://aws.amazon.com/ec2/pricing/on-demand/ for its price", type);
}

/* read_answer: one line from stdin without its newline, or NULL at the end of the input. */
static char *read_answer(void) {
  size_t cap = 64, len = 0;
  char *s = cbc_xmalloc(cap);
  int c = EOF;
  while ((c = getchar()) != EOF && c != '\n') {
    if (len + 1 >= cap) {
      cap *= 2;
      s = cbc_xrealloc(s, cap);
    }
    s[len++] = (char)c;
  }
  if (c == EOF && len == 0) {
    free(s);
    return NULL;
  }
  s[len] = '\0';
  return s;
}

/* group_confirm: true with --yes, or a "yes" typed at a terminal; else false. */
static bool group_confirm(bool yes, const char *what) {
  if (yes) return true;
  if (!isatty(STDIN_FILENO)) return false;
  printf("%s? Type yes to go on: ", what);
  fflush(stdout);
  char *answer = read_answer();
  bool ok = answer && strcmp(answer, "yes") == 0;
  free(answer);
  return ok;
}

/* ---- the arguments ---------------------------------------------------------------------------- */

void cbc_group_args(const char *cmd, const char *kind, int argc, char **argv, cbc_group_opts *o) {
  bool init = strcmp(cmd, "group-init") == 0;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--yes") == 0) {
      o->yes = true;
    } else if (strcmp(a, "--type") == 0) {
      if (!init) cbc_fail(CBC_EXIT_USAGE, "%s takes no --type", cmd);
      if (i + 1 >= argc) cbc_fail(CBC_EXIT_USAGE, "--type wants an instance type (t4g.small)");
      const char *t = argv[i + 1];
      if (!cbc_re("^[a-z][a-z0-9-]*\\.[a-z0-9]+$", t)) cbc_fail(CBC_EXIT_USAGE, "%s is not an instance type (t4g.small)", t);
      o->type = cbc_dup(t);
      i++;
    } else if (strcmp(a, "--ipv4") == 0) {
      if (!init || strcmp(kind, "vm") != 0) cbc_fail(CBC_EXIT_USAGE, "--ipv4 is group-init vm's alone");
      o->ipv4 = true;
    } else if (strcmp(a, "--delete-data") == 0) {
      if (init) cbc_fail(CBC_EXIT_USAGE, "%s takes no --delete-data", cmd);
      o->delete_data = true;
    } else {
      cbc_fail(CBC_EXIT_USAGE, "%s takes %s, not %s", cmd, init ? "--yes, --type TYPE and --ipv4" : "--yes and --delete-data", a);
    }
  }
}

/* ---- the state bucket ------------------------------------------------------------------------- */

bool cbc_group_state_bucket_found(void) {
  char *bucket = state_bucket();
  if (aws_call(NULL, "s3api", "head-bucket", "--bucket", bucket, NULL) == 0) return true;
  const char *code = aws_code();
  if (strcmp(code, "404") == 0 || strcmp(code, "NoSuchBucket") == 0 || strcmp(code, "NotFound") == 0) return false;
  aws_fail(cbc_fmt("reading the state bucket s3://%s", bucket));
}

/* state_bucket_make: the state bucket, made in REGION (a name this account holds already is it). */
static void state_bucket_make(const char *region) {
  char *bucket = state_bucket();
  int rc;
  if (strcmp(region, "us-east-1") == 0) {
    rc = aws_call(NULL, "s3api", "create-bucket", "--bucket", bucket, "--region", region, NULL);
  } else {
    char *loc = cbc_fmt("LocationConstraint=%s", region);
    rc = aws_call(NULL, "s3api", "create-bucket", "--bucket", bucket, "--region", region, "--create-bucket-configuration", loc, NULL);
    free(loc);
  }
  if (rc != 0) {
    const char *code = aws_code();
    if (strcmp(code, "BucketAlreadyOwnedByYou") == 0) {
      /* made since the read, by this account: it is the one */
    } else if (strcmp(code, "BucketAlreadyExists") == 0) {
      fprintf(stderr, "cbase: the state bucket's name s3://%s is another account's\n", bucket);
      exit(CBC_EXIT_FAIL);
    } else {
      aws_fail(cbc_fmt("making the state bucket s3://%s", bucket));
    }
  }
  if (aws_call(NULL, "s3api", "put-public-access-block", "--bucket", bucket, "--region", region,
               "--public-access-block-configuration", "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true", NULL) != 0)
    aws_fail(cbc_fmt("blocking public access to s3://%s", bucket));
  if (aws_call(NULL, "s3api", "put-bucket-versioning", "--bucket", bucket, "--region", region,
               "--versioning-configuration", "Status=Enabled", NULL) != 0)
    aws_fail(cbc_fmt("turning on versioning of s3://%s", bucket));
  printf("state bucket: s3://%s made (%s; versioned, public access blocked)\n", bucket, region);
}

/* state_bucket_here: the state bucket, made when the account has none. */
static void state_bucket_here(const char *region) {
  char *bucket = state_bucket();
  if (cbc_group_state_bucket_found()) {
    printf("state bucket: s3://%s\n", bucket);
    return;
  }
  state_bucket_make(region);
}

bool cbc_group_state_bucket_adopted(const char *region, const char *group, bool dry, const char *app, const char *vm_port, const char *vm_data) {
  char *bucket = state_bucket();
  if (cbc_group_state_bucket_found()) return false;
  if (dry) {
    char *grand = *vm_port ? cbc_fmt(" (grandfathered: port %s, data %s)", vm_port, vm_data) : cbc_dup("");
    printf("dry run: would create state bucket s3://%s (%s; versioned, public access blocked) and seed %s with %s%s\n", bucket, region,
           state_key("vm", group), app, grand);
    free(grand);
    return true;
  }
  state_bucket_make(region);
  return false;
}

/* ---- the template ----------------------------------------------------------------------------- */

/* boot_text: vm_boot.sh as the template takes it: its comments and blank lines left out (the shebang
 * stays), each ${ written ${! (Fn::Sub's literal), the origin's address the one Fn::Sub names, and every
 * line indented 12 spaces. No trailing newline (a command substitution's). */
static char *boot_text(const char *src) {
  cbc_buf out = { 0, 0, 0 };
  int no = 0;
  for (const char *p = src; *p;) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char *line = cbc_dupn(p, len);
    no++;
    const char *q = line;
    while (*q == ' ' || *q == '\t') q++;
    bool keep = no == 1 || (*q != '#' && len > 0);
    if (keep) {
      char *t = replace_all(line, "${", "${!");
      if (strcmp(t, "want='@ORIGIN_IPV6@'") == 0) {
        free(t);
        t = cbc_dup("want='${OriginIpv6}'");
      }
      char *ind = cbc_fmt("            %s\n", t);
      free(t);
      size_t need = out.len + strlen(ind) + 1;
      if (need > out.cap) {
        out.cap = need * 2;
        out.data = realloc(out.data, out.cap);
        if (!out.data) cbc_fail(CBC_EXIT_FAIL, "out of memory");
      }
      memcpy(out.data + out.len, ind, strlen(ind) + 1);
      out.len += strlen(ind);
      free(ind);
    }
    free(line);
    if (!eol) break;
    p = eol + 1;
  }
  char *text = text_of(&out);
  free(out.data);
  return text;
}

/* cbc_group_render: the group's template for KIND, its T_ values set as group_render sets them, written to F. */
void cbc_group_render(const char *kind, const char *group, FILE *f, bool ipv4) {
  template_set("GROUP", group);
  template_set("AMIPARAM", GROUP_AMI_PARAM);
  template_set("PREFIXLIST", GROUP_PREFIX_LIST);
  template_set("PREFIXLISTIPV4", GROUP_PREFIX_LIST_IPV4);
  char *boot = cbc_dup("");
  if (strcmp(kind, "vm") == 0) {
    char *path = cbc_fmt("%s/cbase.d/vm_boot.sh", cbc_tools_dir());
    char *src = NULL;
    size_t srclen = 0;
    if (!cbc_read_file(path, &src, &srclen)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", path);
    free(path);
    free(boot);
    boot = boot_text(src);
    free(src);
  }
  template_set("BOOT", boot);
  free(boot);
  template_render(cbc_fmt("group-%s.yaml", kind), ipv4 ? "ipv4" : "", f);
}

void cbc_group_template(const char *kind, const char *group, bool ipv4) { cbc_group_render(kind, group, stdout, ipv4); }

/* prefix_list: the id of the managed prefix list NAME (CloudFront's origin-facing lists). */
static char *prefix_list(const char *name) {
  cbc_buf out = { 0, 0, 0 };
  char *filter = cbc_fmt("Name=prefix-list-name,Values=%s", name);
  if (aws_call(&out, "ec2", "describe-managed-prefix-lists", "--filters", filter, "--query", "PrefixLists[0].PrefixListId",
               "--output", "text", NULL) != 0)
    aws_fail(cbc_fmt("finding %s", name));
  free(filter);
  char *id = text_of(&out);
  free(out.data);
  if (!cbc_re("^pl-[0-9a-f]+$", id)) {
    fprintf(stderr, "cbase: there is no %s prefix list in %s\n", name, stk_region);
    exit(CBC_EXIT_FAIL);
  }
  return id;
}

/* ---- group-init ------------------------------------------------------------------------------- */

/* vm_instance: the group's instance type, its image and its zone, as group-init sets them up (a new
 * group, or the stack's own values kept). */
static void vm_instance(const cbc_group_opts *o, const char *region) {
  const char *type = o->type ? o->type : "";
  if (!*type && strcmp(stk_status, "NONE") == 0) type = GROUP_TYPE_DEFAULT;
  if (*type) {
    cbc_buf out = { 0, 0, 0 };
    if (aws_call(&out, "ec2", "describe-instance-types", "--instance-types", type, "--query",
                 "InstanceTypes[0].[MemoryInfo.SizeInMiB,join(`,`,ProcessorInfo.SupportedArchitectures)]", "--output", "text", NULL) != 0) {
      if (strcmp(aws_code(), "InvalidInstanceType") == 0) {
        fprintf(stderr, "cbase: %s is not an EC2 instance type in %s\n", type, region);
        exit(CBC_EXIT_FAIL);
      }
      aws_fail(cbc_fmt("reading instance type %s", type));
    }
    char *text = text_of(&out);
    free(out.data);
    /* the architectures are the text's tab-separated second field */
    char *tab = strchr(text, '\t');
    char *archs = tab ? tab + 1 : "";
    char *archs_c = cbc_fmt(",%s,", archs);
    if (!strstr(archs_c, ",arm64,")) {
      fprintf(stderr, "cbase: %s is %s, and cbase builds the server for linux arm64 alone (make serve-binary): pick an arm64 (Graviton) type, t4g.nano to t4g.2xlarge\n", type, archs);
      exit(CBC_EXIT_FAIL);
    }
    char *mib_text = tab ? cbc_dupn(text, (size_t)(tab - text)) : text;
    long long mib = cbc_vm_mib_text(type, mib_text);
    printf("instance: %s; %lld MiB, of which %lld for apps (%d kept for the OS)\n", group_cost(type), mib, mib - CBC_VM_OS_RESERVE,
           CBC_VM_OS_RESERVE);
    stack_param_add("InstanceType", type);
  } else {
    stack_param_previous("InstanceType");
  }
  if (strcmp(stk_status, "NONE") == 0) {
    cbc_buf out = { 0, 0, 0 };
    if (aws_call(&out, "ssm", "get-parameter", "--name", GROUP_AMI_PARAM, "--query", "Parameter.Value", "--output", "text", NULL) != 0)
      aws_fail(cbc_fmt("reading the Amazon Linux 2023 image (%s)", GROUP_AMI_PARAM));
    char *image = text_of(&out);
    free(out.data);
    if (!cbc_re("^ami-[0-9a-f]+$", image)) {
      fprintf(stderr, "cbase: %s names \"%.40s\", not an image\n", GROUP_AMI_PARAM, image);
      exit(CBC_EXIT_FAIL);
    }
    printf("image: %s (Amazon Linux 2023 arm64, kept for the group's life)\n", image);
    stack_param_add("ImageId", image);
    free(image);
  } else {
    stack_param_previous("ImageId");
  }
  if (strcmp(stk_status, "NONE") == 0) {
    /* the subnet's zone, one that offers the type, picked once: a new zone is a new subnet and instance */
    char *filter = cbc_fmt("Name=instance-type,Values=%s", type);
    cbc_buf out = { 0, 0, 0 };
    if (aws_call(&out, "ec2", "describe-instance-type-offerings", "--location-type", "availability-zone", "--filters", filter,
                 "--query", "sort(InstanceTypeOfferings[].Location)[0]", "--output", "text", NULL) != 0)
      aws_fail(cbc_fmt("finding a zone that offers %s", type));
    free(filter);
    char *zone = text_of(&out);
    free(out.data);
    if (!cbc_re("^[a-z0-9-]+[a-z]$", zone)) {
      fprintf(stderr, "cbase: no zone of %s offers %s\n", region, type);
      exit(CBC_EXIT_FAIL);
    }
    printf("zone: %s (offers %s; kept for the group's life)\n", zone, type);
    stack_param_add("AvailabilityZone", zone);
    free(zone);
  } else {
    stack_param_previous("AvailabilityZone");
  }
}

/* state_origin_line: the origin the state file records for a dual-stack VM (its public DNS name), or "". */
static char *public_dns_of(const char *group) {
  char *id = NULL;
  if (aws_group_find("vm", group, &id) != 0) exit(CBC_EXIT_FAIL);
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "ec2", "describe-instances", "--instance-ids", id, "--query", "Reservations[0].Instances[0].PublicDnsName",
               "--output", "text", NULL) != 0)
    aws_fail(cbc_fmt("reading vm group %s's instance", group));
  char *dns = text_of(&out);
  free(out.data);
  if (strcmp(dns, "None") == 0) {
    free(dns);
    return cbc_dup("");
  }
  return dns;
}

void cbc_group_init(const char *kind, const char *group, const cbc_group_opts *o) {
  char *region = aws_region_required();
  char *stack = group_stack(kind, group);
  stack_use(stack, region, cbc_fmt("Key=cbase:group,Value=%s/%s", kind, group));
  bool vm = strcmp(kind, "vm") == 0;
  printf("group-init: %s group %s in account %s (%s), stack %s\n", kind, group, aws_account, region, stk_name);
  stack_status();
  if (vm) {
    vm_instance(o, region);
    char *prefix = prefix_list(GROUP_PREFIX_LIST);
    stack_param_add("CloudFrontPrefixList", prefix);
    free(prefix);
    if (o->ipv4) {
      char *prefix4 = prefix_list(GROUP_PREFIX_LIST_IPV4);
      stack_param_add("CloudFrontPrefixListIpv4", prefix4);
      free(prefix4);
      printf("address: dual-stack (--ipv4): a public IPv4 is about $3.60 a month, an Elastic IP\n");
    } else {
      printf("address: IPv6-only, no public IPv4 (--ipv4 adds one, about $3.60 a month)\n");
    }
    stack_param_add("ArtifactBucket", state_bucket());
    stk_caps = "CAPABILITY_IAM";
  } else {
    printf("bucket: S3 bills what it stores and serves (cents a month for a site)\n");
  }
  stk_template = cbc_tmp_path("stack.yaml");
  FILE *f = fopen(stk_template, "w");
  if (!f) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", stk_template);
  cbc_group_render(kind, group, f, o->ipv4);
  fclose(f);
  stack_changeset();
  if (*stk_changes && vm) {
    if (!group_confirm(o->yes, cbc_fmt("Run this change set on %s", stk_name))) {
      stack_discard();
      fprintf(stderr, "cbase: a VM costs money, so group-init vm runs its change set only with --yes (or yes typed at a terminal): the change set is deleted, and nothing was made or changed\n");
      exit(2);
    }
  }
  /* The state bucket first: the instance's role names it, and the state file goes in it. */
  state_bucket_here(region);
  if (*stk_changes) stack_execute();
  /* The group's state file, empty, so apps and doctor see the group; one that exists is kept. A
   * dual-stack group's records its origin: the instance's public DNS name, dualstack. An IPv6-only
   * instance has no public DNS name of AWS's: CloudFront reaches it at an AAAA record of its fixed address,
   * which the group's first app names (CBASE_VM_ORIGIN) and its join records (addr=ipv6, state.sh). */
  char *bucket = state_bucket();
  char *key = state_key(kind, group);
  char *origin = cbc_dup("");
  if (vm && !o->ipv4) {
    char *addr6 = stack_output("OriginIpv6");
    printf("origin: the instance claims %s at boot, and keeps it across a replacement. Make an AAAA record for it (vm.example.com AAAA %s) and name it once in the group's first app's cbase.conf: CBASE_VM_ORIGIN = vm.example.com\n",
           addr6, addr6);
  } else if (vm) {
    free(origin);
    origin = public_dns_of(group);
  }
  char *state_file = cbc_tmp_path("state-new");
  FILE *sf = fopen(state_file, "w");
  if (!sf) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", state_file);
  fprintf(sf, "cbase-state %s %s %s%s\n", STATE_VERSION_TEXT, kind, group, *origin ? cbc_fmt(" origin=%s addr=dualstack", origin) : "");
  fclose(sf);
  int rc = aws_s3_put(bucket, key, state_file, "*");
  if (rc == 0) printf("state file: s3://%s/%s (no members yet)\n", bucket, key);
  else if (rc == 3) printf("state file: s3://%s/%s (kept as it is)\n", bucket, key);
  else exit(rc);
  char *id = NULL;
  if (aws_group_find(kind, group, &id) != 0) exit(CBC_EXIT_FAIL);
  printf("%s group %s: %s\n", kind, group, id);
}

/* ---- group-destroy ---------------------------------------------------------------------------- */

void cbc_group_destroy(const char *kind, const char *group, const cbc_group_opts *o) {
  char *region = aws_region_required();
  char *stack = group_stack(kind, group);
  stack_use(stack, region, "");
  printf("group-destroy: %s group %s in account %s (%s), stack %s\n", kind, group, aws_account, region, stk_name);
  state_read(kind, group);
  if (state_apps()->n > 0) {
    char *names = cbc_dup("");
    for (size_t i = 0; i < state_apps()->n; i++) {
      char *g = cbc_fmt("%s%s%s", names, i ? " " : "", state_apps()->v[i]);
      free(names);
      names = g;
    }
    fprintf(stderr, "cbase: %s group %s still has %zu member%s: %s. Undeploy each first (./cbase undeploy in its repository); nothing was changed.\n",
            kind, group, state_apps()->n, state_apps()->n == 1 ? "" : "s", names);
    exit(CBC_EXIT_FAIL);
  }
  stack_status();
  char *id = cbc_dup("");
  cbc_strs data = { 0, 0, 0 };
  if (strcmp(stk_status, "NONE") != 0) {
    if (aws_group_find(kind, group, &id) != 0) exit(CBC_EXIT_FAIL);
    if (strcmp(kind, "vm") == 0) {
      /* the data undeploy keeps: the instance's disk goes with it */
      if (!cbc_vm_data_check(id, group, &data)) {
        fprintf(stderr, "cbase: the data check on %s ended %s, so whether the instance holds data is not known: nothing was changed (--delete-data destroys it anyway)\n", id, cbc_vm.ssm_status);
        if (!o->delete_data) exit(CBC_EXIT_FAIL);
      }
    }
  }
  if (data.n > 0 && !o->delete_data) {
    char *names = cbc_dup("");
    for (size_t i = 0; i < data.n; i++) {
      char *g = cbc_fmt("%s%s%s", names, i ? " " : "", data.v[i]);
      free(names);
      names = g;
    }
    fprintf(stderr, "cbase: vm group %s's instance %s holds the data of %s (/srv/cbase/<app>/data, kept by undeploy), which goes with the instance: refused. Copy it off, or run group-destroy --delete-data to destroy it. Nothing was changed.\n",
            group, id, names);
    exit(CBC_EXIT_FAIL);
  }
  bool vm = strcmp(kind, "vm") == 0;
  if (!group_confirm(o->yes, cbc_fmt("Delete %s%s", stk_name, vm ? ", and the instance with its disk" : ""))) {
    fprintf(stderr, "cbase: group-destroy deletes the group's stack only with --yes (or yes typed at a terminal): nothing was changed\n");
    exit(2);
  }
  stack_delete();
  if (strcmp(kind, "static") == 0 && *id) {
    /* Retain kept the bucket and its files; with its tag gone, a group-init of the name makes a new one. */
    if (aws_call(NULL, "s3api", "delete-bucket-tagging", "--bucket", id, NULL) != 0) aws_fail(cbc_fmt("taking the group tag off s3://%s", id));
    printf("bucket: s3://%s is kept with its files, its cbase:static-group tag taken off (delete it with aws s3 rb s3://%s --force)\n", id, id);
  }
  char *bucket = state_bucket();
  char *key = state_key(kind, group);
  if (strcmp(state_etag(), "*") != 0) {
    if (aws_call(NULL, "s3api", "delete-object", "--bucket", bucket, "--key", key, NULL) != 0)
      aws_fail(cbc_fmt("deleting s3://%s/%s", bucket, key));
    printf("state file: s3://%s/%s deleted\n", bucket, key);
  }
  printf("%s group %s is destroyed\n", kind, group);
}
