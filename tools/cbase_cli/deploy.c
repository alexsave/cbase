/* deploy.c - the deploy and undeploy commands, step for step as tools/cbase's cases do them: the same
 * order, the same messages, the same exit codes. */
#include "deploy.h"

#include "aws.h"
#include "cert.h"
#include "conf.h"
#include "group.h"
#include "stack.h"
#include "state.h"
#include "static.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cbc_strs cbc_dry_problems;

void dry_stop(const char *what, const char *line) {
  if (cbc_flags.dry_run) {
    cbc_strs_push(&cbc_dry_problems, cbc_dup(what));
    printf("dry run: a deploy would stop here, on %s; this dry run goes on\n", what);
    return;
  }
  if (line && *line) fprintf(stderr, "%s\n", line);
  exit(CBC_EXIT_FAIL);
}

void cbc_deploy_scan(const char *dir, cbc_strs *files) {
  cbc_static_scan(dir, files);
  stk_files = cbc_tmp_path("static-files");
  FILE *f = fopen(stk_files, "wb");
  if (!f) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", stk_files);
  for (size_t i = 0; i < files->n; i++) fprintf(f, "%s\n", files->v[i]);
  if (fclose(f) != 0) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", stk_files);
}

/* CF_ZONE_ID: every CloudFront distribution's hosted zone, for a Route 53 alias. */
#define CF_ZONE_ID "Z2FDTNDATAQYW2"

void cbc_deploy(const char *conf_file, int argc, char **argv) {
  const char *aliases_flag = "";
  for (int k = 0; k < argc; k++) {
    const char *a = argv[k];
    if (strcmp(a, "--dry-run") == 0) cbc_flags.dry_run = cbc_flags.dry_banner = true;
    else if (strcmp(a, "--accept-drift") == 0) cbc_flags.accept_drift = true;
    else if (strcmp(a, "--prune") == 0) cbc_flags.prune = true;
    else if (strcmp(a, "--allow-downtime") == 0) cbc_flags.allow_downtime = true;
    else if (strcmp(a, "--no-aliases") == 0) aliases_flag = "off";
    else if (strcmp(a, "--attach") == 0) aliases_flag = "on";
    else
      cbc_fail(CBC_EXIT_USAGE, "deploy takes --dry-run, --accept-drift, --prune, --allow-downtime, --no-aliases and --attach, not %s", a);
  }
  const bool dry = cbc_flags.dry_run;
  const bool prune = cbc_flags.prune;

  cbc_conf_load(conf_file);
  if (*aliases_flag) {
    if (!*cbc_conf.domain) cbc_fail(CBC_EXIT_USAGE, "--no-aliases and --attach are for CBASE_DOMAIN's hostnames, and there is none");
    cbc_conf.aliases = cbc_dup(aliases_flag);
  }
  aws_init();
  stack_app_use();
  printf("deploy: %s to account %s%s\n", cbc_conf.app, aws_account, cbc_flags.dry_banner ? " (dry run: nothing is changed)" : "");

  const bool has_vm = strcmp(cbc_conf.vm, "none") != 0;
  const bool has_static = strcmp(cbc_conf.stat, "none") != 0;

  /* 1. The groups, found by their tags (a missing one stops with its group-init command). */
  if (has_vm) cbc_vm_deploy_find(cbc_conf.vm);
  if (has_static) {
    char *bucket = NULL, *region = NULL;
    cbc_static_bucket(cbc_conf.static_bucket, cbc_conf.stat, &bucket, &region);
    stk_bucket = bucket;
    stk_bucket_region = region;
    stk_prefix = *cbc_conf.static_prefix ? cbc_conf.static_prefix : cbc_conf.app;
    if (*cbc_conf.static_bucket) {
      printf("static bucket s3://%s (%s, adopted: CBASE_STATIC_BUCKET), the app's folder %s/\n", stk_bucket, stk_bucket_region, stk_prefix);
      if (!cbc_static_policy_ok(stk_bucket, aws_account)) {
        char *what = cbc_dup("");
        cbc_asprintf(&what, "s3://%s's policy (above)", stk_bucket);
        dry_stop(what, "cbase: then deploy again; nothing was changed");
      }
    } else {
      printf("static group %s: s3://%s (%s), the app's folder %s/\n", cbc_conf.stat, stk_bucket, stk_bucket_region, stk_prefix);
    }
  }
  /* 2. The files, read and grouped before anything is changed. */
  cbc_strs files = { 0, 0, 0 };
  cbc_static_plan plan = { 0 };
  if (has_static) {
    cbc_deploy_scan(cbc_conf.static_dir, &files);
    cbc_static_plan_make(&plan, cbc_conf.static_dir, &files, cbc_tmp_path("."));
  }
  /* 3. The server, built before anything is changed (a dry run builds nothing). */
  if (has_vm && !dry) cbc_vm_deploy_build();
  /* 4. The certificate (it may stop, status 3, for DNS records added by hand), and the hostnames free. */
  cbc_cert_t cert = { 0 };
  cbc_cert_resolve(&cert, dry);
  if (cert.hosts.n > 0) {
    stack_status();
    cbc_strs own = { 0, 0, 0 };
    const char *to_main = "", *to_api = "";
    if (strcmp(stk_status, "NONE") != 0) {
      to_main = stack_output("DistributionId");
      to_api = stack_output("ApiDistributionId");
      cbc_strs_push(&own, cbc_dup(to_main));
      cbc_strs_push(&own, cbc_dup(to_api));
    }
    if (cbc_cert_alias_check(&cert, to_main, to_api, &own, dry))
      dry_stop("a hostname another distribution holds (above)", "cbase: nothing was changed");
  }
  stk_cert = cert.arn;
  stk_api_cert = cert.api_arn;
  /* 5. The VM group: the port claimed and the memory fit checked, in one conditional write; the stack's VM
   *    origin is the instance and that port. */
  if (has_vm) {
    cbc_vm_deploy_join(cbc_conf.vm, dry);
    stk_addr = cbc_vm.origin_addr;   /* the template's "addr" block: the origin's IP address type */
    stack_params_clear();
    stack_param_add("VmOrigin", cbc_vm.dns);
    stack_param_add("VmPort", cbc_vm.port);
    stack_param_add("OriginSecret", cbc_vm.origin_secret);
  }
  /* 6. The stack's change set, through its gates (drift, the bucket, downtime); a dry run's is deleted there. */
  stack_plan();
  if (dry) {
    if (has_static) {
      printf("the upload (%s/ to s3://%s/%s/), in order:\n", cbc_conf.static_dir, stk_bucket, stk_prefix);
      cbc_static_print_plan(&plan);
    }
    printf("dry run: change set %s deleted unexecuted; nothing was uploaded, deleted or changed\n", stk_cs);
    if (cbc_dry_problems.n == 0) exit(0);
    fprintf(stderr, "cbase: dry run: a deploy would stop on %zu problem%s:\n", cbc_dry_problems.n, cbc_dry_problems.n == 1 ? "" : "s");
    for (size_t i = 0; i < cbc_dry_problems.n; i++) fprintf(stderr, "  %s\n", cbc_dry_problems.v[i]);
    exit(CBC_EXIT_FAIL);
  }
  /* 7. The service: the artifact, then SSM installs and restarts it. It goes before the files, so a service
   *    that does not start leaves the site as it was; the change set is then deleted unexecuted. */
  if (has_vm && !cbc_vm_ship(cbc_conf.vm)) {
    if (*stk_changes) stack_discard();
    exit(CBC_EXIT_FAIL);
  }
  /* 8. The files: the app joins the static group's state file; then hashed, the rest, html last; then the prune. */
  if (has_static) {
    if (!*cbc_conf.static_bucket) state_change("static", cbc_conf.stat, cbc_conf.app, "", "join");
    cbc_static_print_plan(&plan);
    cbc_static_upload(&plan, stk_bucket, stk_bucket_region, stk_prefix);
    if (prune) cbc_static_prune(stk_bucket, stk_bucket_region, stk_prefix, &files);
  }
  /* 9. The distribution: the hostnames moved onto it first (a staged attach), then the stack update. */
  if (cert.moves.n > 0) {
    cbc_cert_attach(&cert);
    stk_no_rollback = true;
  }
  if (*stk_changes) stack_execute();
  char *dist_id = stack_output("DistributionId");
  char *dist_domain = stack_output("DistributionDomain");
  if (!*dist_id || !*dist_domain)
    cbc_fail(CBC_EXIT_FAIL, "the %s stack has no DistributionId or DistributionDomain output", stk_name);
  /* 10. The edge's copies of the old files dropped; then the hostnames pointed at the distribution. */
  if (strcmp(stk_type, "UPDATE") == 0) {   /* a distribution just made has nothing cached */
    cbc_buf out = { 0 };
    if (aws_call(&out, "cloudfront", "create-invalidation", "--distribution-id", dist_id, "--paths", "/*", "--query",
                 "Invalidation.Id", "--output", "text", NULL) != 0) {
      char *what = NULL;
      cbc_asprintf(&what, "invalidating %s", dist_id);
      aws_fail(what);
    }
    char *inv = cbc_buf_text(&out);
    printf("invalidation %s of /* on %s\n", inv, dist_id);
    fflush(stdout);
    if (aws_call(NULL, "cloudfront", "wait", "invalidation-completed", "--distribution-id", dist_id, "--id", inv, NULL) != 0) {
      char *what = NULL;
      cbc_asprintf(&what, "waiting for invalidation %s", inv);
      aws_fail(what);
    }
  }
  char *url = NULL;
  cbc_asprintf(&url, "https://%s/", dist_domain);
  if (*cert.arn) {
    for (size_t i = 0; i < cbc_conf.domains.n; i++) cbc_cert_point(&cert, cbc_conf.domains.v[i], dist_domain, CF_ZONE_ID);
    if (*cbc_conf.api_host) {
      char *api_domain = stack_output("ApiDistributionDomain");
      if (!*api_domain) cbc_fail(CBC_EXIT_FAIL, "the %s stack has no ApiDistributionDomain output", stk_name);
      cbc_cert_point(&cert, cbc_conf.api_host, api_domain, CF_ZONE_ID);
    }
    if (strcmp(cbc_conf.aliases, "off") != 0) {
      cbc_asprintf(&url, "https://%s/", cbc_conf.domain);
    }
  }
  printf("deployed %s: %s\n", cbc_conf.app, url);
  if (strcmp(cbc_conf.vm, "none") != 0) printf("service: %s\n", cbc_vm_service());
  for (size_t i = 0; i < cert.notes.n; i++) printf("%s\n", cert.notes.v[i]);
}

void cbc_undeploy(const char *conf_file, int argc, char **argv) {
  bool del_files = false, del_data = false;
  for (int k = 0; k < argc; k++) {
    if (strcmp(argv[k], "--delete-files") == 0) del_files = true;
    else if (strcmp(argv[k], "--delete-data") == 0) del_data = true;
    else cbc_fail(CBC_EXIT_USAGE, "undeploy takes --delete-files and --delete-data, not %s", argv[k]);
  }
  cbc_conf_load(conf_file);
  aws_init();
  printf("undeploy: %s from account %s\n", cbc_conf.app, aws_account);
  /* 1. The distribution first, so nothing routes to what goes next. */
  stack_app_use();
  stack_delete();
  /* 2. The service, then its membership (its port released); its data kept unless --delete-data. */
  if (strcmp(cbc_conf.vm, "none") != 0) cbc_vm_undeploy(cbc_conf.vm, del_data);
  else if (del_data) printf("undeploy: --delete-data: %s has no VM, so it has no data folder\n", cbc_conf.app);
  /* 3. The files: kept unless --delete-files (the folder alone, never another app's key). */
  if (strcmp(cbc_conf.stat, "none") != 0) {
    char *bucket = NULL, *region = NULL;
    cbc_static_bucket(cbc_conf.static_bucket, cbc_conf.stat, &bucket, &region);
    stk_bucket = bucket;
    stk_bucket_region = region;
    stk_prefix = *cbc_conf.static_prefix ? cbc_conf.static_prefix : cbc_conf.app;
    if (del_files) {
      cbc_strs none = { 0, 0, 0 };
      cbc_static_prune(stk_bucket, stk_bucket_region, stk_prefix, &none);
    } else {
      printf("undeploy: kept s3://%s/%s/ (./cbase undeploy --delete-files deletes it)\n", stk_bucket, stk_prefix);
    }
    if (!*cbc_conf.static_bucket) state_change("static", cbc_conf.stat, cbc_conf.app, "", "leave");
  }
  if (*cbc_conf.domain) {
    printf("undeploy: the DNS records of ");
    for (size_t i = 0; i < cbc_conf.domains.n; i++) printf("%s%s", i ? " " : "", cbc_conf.domains.v[i]);
    if (*cbc_conf.api_host) printf(" %s", cbc_conf.api_host);
    printf(" still point at the deleted distribution: remove them where the domain's DNS is served\n");
  }
  printf("undeployed %s\n", cbc_conf.app);
}
