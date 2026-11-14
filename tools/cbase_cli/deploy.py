# deploy.py - the deploy and undeploy commands (the port of deploy.c), step for step as tools/cbase's cases
# do them: the same order, the same messages, the same exit codes.
from __future__ import annotations

import sys
from typing import List, Optional

import aws
import cert as certmod
import cli
import conf as confmod
import stack
import state
import static
import vm

conf = confmod.conf

# The dry run's state: the problems a dry run lists at its end (tools/cbase's DRY_PROBLEMS).
dry_problems: List[str] = []


def dry_stop(what: str, line: str) -> None:
    """A problem a deploy stops on, said where it is found. A dry run keeps WHAT and goes on; a deploy
    prints LINE (if any, to stderr) and exits 1 there."""
    if cli.flags.dry_run:
        dry_problems.append(what)
        cli.out("dry run: a deploy would stop here, on %s; this dry run goes on\n" % what)
        return
    if line:
        cli.err(line + "\n")
    sys.exit(cli.EXIT_FAIL)


def scan(dir: str) -> List[str]:
    """The build's files (static.scan), and their list written for the router's page list and the app
    shell check (stack.stk_files)."""
    files = static.scan(dir)
    stack.stk_files = cli.tmp_path("static-files")
    cli.write_file(stack.stk_files, "".join(f + "\n" for f in files))
    return files


# CF_ZONE_ID: every CloudFront distribution's hosted zone, for a Route 53 alias.
CF_ZONE_ID = "Z2FDTNDATAQYW2"


def deploy(conf_file: str, args: List[str]) -> None:
    """CONF_FILE is the -f file; ARGS (after "deploy") are already checked by the caller to be deploy's flags."""
    aliases_flag = ""
    for a in args:
        if a == "--dry-run":
            cli.flags.dry_run = cli.flags.dry_banner = True
        elif a == "--accept-drift":
            cli.flags.accept_drift = True
        elif a == "--prune":
            cli.flags.prune = True   # what the default is: still accepted
        elif a == "--no-prune":
            cli.flags.prune = False
        elif a == "--allow-downtime":
            cli.flags.allow_downtime = True
        elif a == "--no-aliases":
            aliases_flag = "off"
        elif a == "--attach":
            aliases_flag = "on"
        else:
            cli.fail(cli.EXIT_USAGE, "deploy takes --dry-run, --accept-drift, --no-prune, --allow-downtime, --no-aliases and --attach, not %s" % a)
    dry = cli.flags.dry_run
    prune = cli.flags.prune

    confmod.load(conf_file)
    if aliases_flag:
        if not conf.domain:
            cli.fail(cli.EXIT_USAGE, "--no-aliases and --attach are for CBASE_DOMAIN's hostnames, and there is none")
        conf.aliases = aliases_flag
    aws.init()
    stack.app_use()
    cli.out("deploy: %s to account %s%s\n" % (conf.app, aws.account, " (dry run: nothing is changed)" if cli.flags.dry_banner else ""))

    has_vm = conf.vm != "none"
    has_static = conf.stat != "none"

    # 1. The groups, found by their tags (a missing one stops with its group-init command).
    if has_vm:
        vm.deploy_find(conf.vm)
    if has_static:
        stack.stk_bucket, stack.stk_bucket_region = static.bucket(conf.static_bucket, conf.stat)
        stack.stk_prefix = conf.static_prefix if conf.static_prefix else conf.app
        if conf.static_bucket:
            cli.out("static bucket s3://%s (%s, adopted: CBASE_STATIC_BUCKET), the app's folder %s/\n" % (stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix))
            if not static.policy_ok(stack.stk_bucket, aws.account):
                dry_stop("s3://%s's policy (above)" % stack.stk_bucket, "cbase: then deploy again; nothing was changed")
        else:
            cli.out("static group %s: s3://%s (%s), the app's folder %s/\n" % (conf.stat, stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix))
    # 2. The files, read and grouped before anything is changed.
    files: List[str] = []
    plan: Optional[static.Plan] = None
    if has_static:
        files = scan(conf.static_dir)
        plan = static.plan_make(conf.static_dir, files, cli.tmp_path("."))
    # 3. The server, built before anything is changed (a dry run builds nothing).
    if has_vm and not dry:
        vm.deploy_build()
    # 4. The certificate (it may stop, status 3, for DNS records added by hand), and the hostnames free.
    cert = certmod.Cert()
    certmod.resolve(cert, dry)
    if cert.hosts:
        stack.status()
        own: List[str] = []
        to_main = ""
        to_api = ""
        if stack.stk_status != "NONE":
            to_main = stack.output("DistributionId")
            to_api = stack.output("ApiDistributionId")
            own.append(to_main)
            own.append(to_api)
        if certmod.alias_check(cert, to_main, to_api, own, dry):
            dry_stop("a hostname another distribution holds (above)", "cbase: nothing was changed")
    stack.stk_cert = cert.arn
    stack.stk_api_cert = cert.api_arn
    # 5. The VM group: the port claimed and the memory fit checked, in one conditional write; the stack's VM
    #    origin is the instance and that port.
    if has_vm:
        vm.deploy_join(conf.vm, dry)
        stack.stk_addr = vm.origin_addr   # the template's "addr" block: the origin's IP address type
        stack.params_clear()
        stack.param_add("VmOrigin", vm.dns)
        stack.param_add("VmPort", vm.port)
        stack.param_add("OriginSecret", vm.origin_secret)
    # 6. The stack's change set, through its gates (drift, the bucket, downtime); a dry run's is deleted there.
    stack.plan()
    if dry:
        if has_static:
            assert plan is not None
            cli.out("the upload (%s/ to s3://%s/%s/), in order:\n" % (conf.static_dir, stack.stk_bucket, stack.stk_prefix))
            static.print_plan(plan)
        if prune and has_static:
            static.prune(stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix, files, True)
        if prune and has_vm:
            vm.prune_artifacts(True)
        cli.out("dry run: change set %s deleted unexecuted; nothing was uploaded, deleted or changed\n" % stack.stk_cs)
        if not dry_problems:
            sys.exit(0)
        n = len(dry_problems)
        cli.err("cbase: dry run: a deploy would stop on %d problem%s:\n" % (n, "" if n == 1 else "s"))
        for p in dry_problems:
            cli.err("  %s\n" % p)
        sys.exit(cli.EXIT_FAIL)
    # 7. The service: the artifact, then SSM installs and restarts it. It goes before the files, so a service
    #    that does not start leaves the site as it was; the change set is then deleted unexecuted.
    if has_vm and not vm.ship(conf.vm):
        if stack.stk_changes:
            stack.discard()
        sys.exit(cli.EXIT_FAIL)
    # 8. The files: the app joins the static group's state file; then hashed, the rest, html last; and the
    #    prune after the stack executes (step 9).
    if has_static:
        assert plan is not None
        if not conf.static_bucket:
            state.change("static", conf.stat, conf.app, "", "join")
        static.print_plan(plan)
        static.upload(plan, stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix)
    # 9. The distribution: the hostnames moved onto it first (a staged attach), then the stack update.
    if cert.moves:
        certmod.attach(cert)
        stack.stk_no_rollback = True
    if stack.stk_changes:
        stack.execute()
    # THE PRUNE, on by default, goes only now, the stack executed (or up to date), the service shipped and the
    # files uploaded: a deploy that stopped earlier never deletes what a rollback or the live site still needs.
    # The static folder's leftovers and the old server binaries are two prunes with their own guards.
    if prune and has_static:
        static.prune(stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix, files, False)
    if prune and has_vm:
        vm.prune_artifacts(False)
    dist_id = stack.output("DistributionId")
    dist_domain = stack.output("DistributionDomain")
    if not dist_id or not dist_domain:
        cli.fail(cli.EXIT_FAIL, "the %s stack has no DistributionId or DistributionDomain output" % stack.stk_name)
    # 10. The edge's copies of the old files dropped; then the hostnames pointed at the distribution.
    if stack.stk_type == "UPDATE":   # a distribution just made has nothing cached
        rc, out = aws.call("cloudfront", "create-invalidation", "--distribution-id", dist_id, "--paths", "/*",
                           "--query", "Invalidation.Id", "--output", "text")
        if rc != 0:
            aws.fail("invalidating %s" % dist_id)
        inv = cli.text_of(out)
        cli.out("invalidation %s of /* on %s\n" % (inv, dist_id))
        sys.stdout.flush()
        rc, _o = aws.call("cloudfront", "wait", "invalidation-completed", "--distribution-id", dist_id, "--id", inv)
        if rc != 0:
            aws.fail("waiting for invalidation %s" % inv)
    url = "https://%s/" % dist_domain
    if cert.arn:
        for d in conf.domains:
            certmod.point(cert, d, dist_domain, CF_ZONE_ID)
        if conf.api_host:
            api_domain = stack.output("ApiDistributionDomain")
            if not api_domain:
                cli.fail(cli.EXIT_FAIL, "the %s stack has no ApiDistributionDomain output" % stack.stk_name)
            certmod.point(cert, conf.api_host, api_domain, CF_ZONE_ID)
        if conf.aliases != "off":
            url = "https://%s/" % conf.domain
    cli.out("deployed %s: %s\n" % (conf.app, url))
    if conf.vm != "none":
        cli.out("service: %s\n" % vm.service())
    for n_ in cert.notes:
        cli.out("%s\n" % n_)


def undeploy(conf_file: str, args: List[str]) -> None:
    """ARGS are the ones after "undeploy", already checked."""
    del_files = False
    del_data = False
    for a in args:
        if a == "--delete-files":
            del_files = True
        elif a == "--delete-data":
            del_data = True
        else:
            cli.fail(cli.EXIT_USAGE, "undeploy takes --delete-files and --delete-data, not %s" % a)
    confmod.load(conf_file)
    aws.init()
    cli.out("undeploy: %s from account %s\n" % (conf.app, aws.account))
    # 1. The distribution first, so nothing routes to what goes next.
    stack.app_use()
    stack.delete()
    # 2. The service, then its membership (its port released); its data kept unless --delete-data.
    if conf.vm != "none":
        vm.undeploy(conf.vm, del_data)
    elif del_data:
        cli.out("undeploy: --delete-data: %s has no VM, so it has no data folder\n" % conf.app)
    # 3. The files: kept unless --delete-files (the folder alone, never another app's key).
    if conf.stat != "none":
        stack.stk_bucket, stack.stk_bucket_region = static.bucket(conf.static_bucket, conf.stat)
        stack.stk_prefix = conf.static_prefix if conf.static_prefix else conf.app
        if del_files:
            static.prune(stack.stk_bucket, stack.stk_bucket_region, stack.stk_prefix, [], False)
        else:
            cli.out("undeploy: kept s3://%s/%s/ (./cbase undeploy --delete-files deletes it)\n" % (stack.stk_bucket, stack.stk_prefix))
        if not conf.static_bucket:
            state.change("static", conf.stat, conf.app, "", "leave")
    if conf.domain:
        cli.out("undeploy: the DNS records of ")
        cli.out(" ".join(conf.domains))
        if conf.api_host:
            cli.out(" %s" % conf.api_host)
        cli.out(" still point at the deleted distribution: remove them where the domain's DNS is served\n")
    cli.out("undeployed %s\n" % conf.app)
