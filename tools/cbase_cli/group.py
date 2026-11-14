# group.py - a group's own stack: group-init, group-destroy and group-template (the port of group.c; group.sh's
# header says what each command makes and why its refusals are where they are). One stack per group in the
# CLI's region: a VM group's instance (cbase-group-vm-<group>), or a static group's bucket
# (cbase-group-static-<group>), and the account's state bucket and the group's state file. A VM costs money,
# so its change set runs only with --yes or a "yes" typed at a terminal. The messages are group.sh's words.
from __future__ import annotations

import os
import sys
from typing import List, Optional

import aws
import cli
import stack
import state
import vm

GROUP_AMI_PARAM = "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64"
GROUP_PREFIX_LIST = "com.amazonaws.global.ipv6.cloudfront.origin-facing"
GROUP_PREFIX_LIST_IPV4 = "com.amazonaws.global.cloudfront.origin-facing"
GROUP_TYPE_DEFAULT = "t4g.nano"


class Opts:
    """The flags group-init and group-destroy take (group_args). TYPE is "" when none was given."""

    def __init__(self) -> None:
        self.yes = False
        self.type = ""
        self.ipv4 = False
        self.delete_data = False


# ---- small helpers ----------------------------------------------------------------------------


def _stack_name(kind: str, group: str) -> str:
    """The stack's name, cbase-group-<kind>-<group>."""
    return "cbase-group-%s-%s" % (kind, group)


_COST_MONTH = {
    "t4g.nano": 3, "t4g.micro": 6, "t4g.small": 12, "t4g.medium": 25,
    "t4g.large": 49, "t4g.xlarge": 98, "t4g.2xlarge": 196,
}


def _cost(type_: str) -> str:
    """A sentence of what the instance type costs a month (us-east-1 on-demand Linux, 730 h)."""
    if type_ in _COST_MONTH:
        return "a %s runs about $%d/month (us-east-1 on-demand), plus about $0.64/month for its 8 GB disk" % (type_, _COST_MONTH[type_])
    return "a %s: see https://aws.amazon.com/ec2/pricing/on-demand/ for its price" % type_


def _prec(s: str, n: int) -> str:
    """printf's %.Ns: at most N bytes of S."""
    b = s.encode("utf-8", "surrogateescape")
    if len(b) <= n:
        return s
    return b[:n].decode("utf-8", "surrogateescape")


def _read_answer() -> Optional[bytes]:
    """One line from stdin without its newline, or None at the end of the input."""
    try:
        line = sys.stdin.buffer.readline()
    except (OSError, ValueError, AttributeError):
        return None
    if line == b"":
        return None
    if line.endswith(b"\n"):
        line = line[:-1]
    return line


def _confirm(yes: bool, what: str) -> bool:
    """True with --yes, or a "yes" typed at a terminal; else False."""
    if yes:
        return True
    try:
        tty = os.isatty(0)
    except OSError:
        tty = False
    if not tty:
        return False
    cli.out("%s? Type yes to go on: " % what)
    sys.stdout.flush()
    answer = _read_answer()
    return answer is not None and answer == b"yes"


# ---- the arguments ----------------------------------------------------------------------------


def args(cmd: str, kind: str, argv: List[str]) -> Opts:
    """CMD's flags (ARGV) after KIND and GROUP, as group_args reads them. Stops the tool (exit 2) on a flag
    the command does not take, with the shell's message."""
    o = Opts()
    init_ = cmd == "group-init"
    argc = len(argv)
    i = 0
    while i < argc:
        a = argv[i]
        if a == "--yes":
            o.yes = True
        elif a == "--type":
            if not init_:
                cli.fail(cli.EXIT_USAGE, "%s takes no --type" % cmd)
            if i + 1 >= argc:
                cli.fail(cli.EXIT_USAGE, "--type wants an instance type (t4g.small)")
            t = argv[i + 1]
            if not cli.re("^[a-z][a-z0-9-]*\\.[a-z0-9]+\\Z", t):
                cli.fail(cli.EXIT_USAGE, "%s is not an instance type (t4g.small)" % t)
            o.type = t
            i += 1
        elif a == "--ipv4":
            if not init_ or kind != "vm":
                cli.fail(cli.EXIT_USAGE, "--ipv4 is group-init vm's alone")
            o.ipv4 = True
        elif a == "--delete-data":
            if init_:
                cli.fail(cli.EXIT_USAGE, "%s takes no --delete-data" % cmd)
            o.delete_data = True
        else:
            cli.fail(cli.EXIT_USAGE, "%s takes %s, not %s" % (cmd, "--yes, --type TYPE and --ipv4" if init_ else "--yes and --delete-data", a))
        i += 1
    return o


# ---- the state bucket -------------------------------------------------------------------------


def state_bucket_found() -> bool:
    """Whether the account has its state bucket (cbase-state-<account>). Stops the tool on a failed read."""
    bucket = state.bucket()
    rc, _o = aws.call("s3api", "head-bucket", "--bucket", bucket)
    if rc == 0:
        return True
    code = aws.code()
    if code == "404" or code == "NoSuchBucket" or code == "NotFound":
        return False
    aws.fail("reading the state bucket s3://%s" % bucket)
    return False


def _state_bucket_make(region: str) -> None:
    """The state bucket, made in REGION (a name this account holds already is it)."""
    bucket = state.bucket()
    if region == "us-east-1":
        rc, _o = aws.call("s3api", "create-bucket", "--bucket", bucket, "--region", region)
    else:
        rc, _o = aws.call("s3api", "create-bucket", "--bucket", bucket, "--region", region,
                          "--create-bucket-configuration", "LocationConstraint=%s" % region)
    if rc != 0:
        code = aws.code()
        if code == "BucketAlreadyOwnedByYou":
            pass   # made since the read, by this account: it is the one
        elif code == "BucketAlreadyExists":
            cli.err("cbase: the state bucket's name s3://%s is another account's\n" % bucket)
            sys.exit(cli.EXIT_FAIL)
        else:
            aws.fail("making the state bucket s3://%s" % bucket)
    rc, _o = aws.call("s3api", "put-public-access-block", "--bucket", bucket, "--region", region,
                      "--public-access-block-configuration", "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true")
    if rc != 0:
        aws.fail("blocking public access to s3://%s" % bucket)
    rc, _o = aws.call("s3api", "put-bucket-versioning", "--bucket", bucket, "--region", region,
                      "--versioning-configuration", "Status=Enabled")
    if rc != 0:
        aws.fail("turning on versioning of s3://%s" % bucket)
    cli.out("state bucket: s3://%s made (%s; versioned, public access blocked)\n" % (bucket, region))


def _state_bucket_here(region: str) -> None:
    """The state bucket, made when the account has none."""
    bucket = state.bucket()
    if state_bucket_found():
        cli.out("state bucket: s3://%s\n" % bucket)
        return
    _state_bucket_make(region)


def state_bucket_adopted(region: str, group: str, dry: bool, app: str, vm_port: str, vm_data: str) -> bool:
    """deploy's, for an adopted VM group (CBASE_VM_STACK), which no group-init made: the state bucket made
    when the account has none (private, versioned, public access blocked). With DRY, makes nothing and says
    what a deploy would make. Returns True when the state bucket does not exist (a dry run: the caller reads
    no state file, as STATE_NO_BUCKET does)."""
    bucket = state.bucket()
    if state_bucket_found():
        return False
    if dry:
        grand = " (grandfathered: port %s, data %s)" % (vm_port, vm_data) if vm_port else ""
        cli.out("dry run: would create state bucket s3://%s (%s; versioned, public access blocked) and seed %s with %s%s\n"
                % (bucket, region, state.key("vm", group), app, grand))
        return True
    _state_bucket_make(region)
    return False


# ---- the template -----------------------------------------------------------------------------


def _boot_text(src: str) -> str:
    """vm_boot.sh as the template takes it: its comments and blank lines left out (the shebang stays), each ${
    written ${! (Fn::Sub's literal), the origin's address the one Fn::Sub names, and every line indented 12
    spaces. No trailing newline (a command substitution's)."""
    out: List[str] = []
    no = 0
    for line in vm.lines_of(src):
        no += 1
        q = line.lstrip(" \t")
        if no == 1 or (not q.startswith("#") and len(line) > 0):
            t = line.replace("${", "${!")
            if t == "want='@ORIGIN_IPV6@'":
                t = "want='${OriginIpv6}'"
            out.append("            %s\n" % t)
    return "".join(out).rstrip("\n")


def render(kind: str, group: str, ipv4: bool) -> str:
    """The group's template for KIND (group.sh's group_render): its T_ values set, the boot script for a VM
    group; IPV4 is group-init --ipv4's flag. Returns the template text."""
    stack.template_set("GROUP", group)
    stack.template_set("AMIPARAM", GROUP_AMI_PARAM)
    stack.template_set("PREFIXLIST", GROUP_PREFIX_LIST)
    stack.template_set("PREFIXLISTIPV4", GROUP_PREFIX_LIST_IPV4)
    boot = ""
    if kind == "vm":
        boot = _boot_text(vm.read(vm.file("vm_boot.sh")))
    stack.template_set("BOOT", boot)
    return stack.template_render("group-%s.yaml" % kind, "ipv4" if ipv4 else "")


def template(kind: str, group: str, ipv4: bool) -> None:
    """A group's template as group-init would render it, to stdout (no AWS)."""
    cli.out(render(kind, group, ipv4))


def _prefix_list(name: str) -> str:
    """The id of the managed prefix list NAME (CloudFront's origin-facing lists)."""
    rc, out = aws.call("ec2", "describe-managed-prefix-lists", "--filters", "Name=prefix-list-name,Values=%s" % name,
                       "--query", "PrefixLists[0].PrefixListId", "--output", "text")
    if rc != 0:
        aws.fail("finding %s" % name)
    pid = cli.text_of(out)
    if not cli.re("^pl-[0-9a-f]+\\Z", pid):
        cli.err("cbase: there is no %s prefix list in %s\n" % (name, stack.stk_region))
        sys.exit(cli.EXIT_FAIL)
    return pid


# ---- group-init -------------------------------------------------------------------------------


def _vm_instance(o: Opts, region: str) -> None:
    """The group's instance type, its image and its zone, as group-init sets them up (a new group, or the
    stack's own values kept)."""
    type_ = o.type
    if not type_ and stack.stk_status == "NONE":
        type_ = GROUP_TYPE_DEFAULT
    if type_:
        rc, out = aws.call("ec2", "describe-instance-types", "--instance-types", type_, "--query",
                           "InstanceTypes[0].[MemoryInfo.SizeInMiB,join(`,`,ProcessorInfo.SupportedArchitectures)]", "--output", "text")
        if rc != 0:
            if aws.code() == "InvalidInstanceType":
                cli.err("cbase: %s is not an EC2 instance type in %s\n" % (type_, region))
                sys.exit(cli.EXIT_FAIL)
            aws.fail("reading instance type %s" % type_)
        text = cli.text_of(out)
        # the architectures are the text's tab-separated second field
        tab = text.find("\t")
        archs = text[tab + 1:] if tab >= 0 else ""
        if ",arm64," not in ",%s," % archs:
            cli.err("cbase: %s is %s, and cbase builds the server for linux arm64 alone (make serve-binary): pick an arm64 (Graviton) type, t4g.nano to t4g.2xlarge\n" % (type_, archs))
            sys.exit(cli.EXIT_FAIL)
        mib_text = text[:tab] if tab >= 0 else text
        mib = vm.mib_text(type_, mib_text)
        cli.out("instance: %s; %d MiB, of which %d for apps (%d kept for the OS)\n" % (_cost(type_), mib, mib - vm.OS_RESERVE, vm.OS_RESERVE))
        stack.param_add("InstanceType", type_)
    else:
        stack.param_previous("InstanceType")
    if stack.stk_status == "NONE":
        rc, out = aws.call("ssm", "get-parameter", "--name", GROUP_AMI_PARAM, "--query", "Parameter.Value", "--output", "text")
        if rc != 0:
            aws.fail("reading the Amazon Linux 2023 image (%s)" % GROUP_AMI_PARAM)
        image = cli.text_of(out)
        if not cli.re("^ami-[0-9a-f]+\\Z", image):
            cli.err("cbase: %s names \"%s\", not an image\n" % (GROUP_AMI_PARAM, _prec(image, 40)))
            sys.exit(cli.EXIT_FAIL)
        cli.out("image: %s (Amazon Linux 2023 arm64, kept for the group's life)\n" % image)
        stack.param_add("ImageId", image)
    else:
        stack.param_previous("ImageId")
    if stack.stk_status == "NONE":
        # the subnet's zone, one that offers the type, picked once: a new zone is a new subnet and instance
        rc, out = aws.call("ec2", "describe-instance-type-offerings", "--location-type", "availability-zone", "--filters",
                           "Name=instance-type,Values=%s" % type_,
                           "--query", "sort(InstanceTypeOfferings[].Location)[0]", "--output", "text")
        if rc != 0:
            aws.fail("finding a zone that offers %s" % type_)
        zone = cli.text_of(out)
        if not cli.re("^[a-z0-9-]+[a-z]\\Z", zone):
            cli.err("cbase: no zone of %s offers %s\n" % (region, type_))
            sys.exit(cli.EXIT_FAIL)
        cli.out("zone: %s (offers %s; kept for the group's life)\n" % (zone, type_))
        stack.param_add("AvailabilityZone", zone)
    else:
        stack.param_previous("AvailabilityZone")


def _public_dns_of(group: str) -> str:
    """The origin the state file records for a dual-stack VM (its public DNS name), or ""."""
    gid = aws.group_find("vm", group)
    if gid is None:
        sys.exit(cli.EXIT_FAIL)
    rc, out = aws.call("ec2", "describe-instances", "--instance-ids", gid, "--query",
                       "Reservations[0].Instances[0].PublicDnsName", "--output", "text")
    if rc != 0:
        aws.fail("reading vm group %s's instance" % group)
    dns = cli.text_of(out)
    if dns == "None":
        return ""
    return dns


def init(kind: str, group: str, o: Opts) -> None:
    """Makes or updates the group's stack (a VM's instance, or a static group's bucket), the state bucket when
    the account has none, and the group's empty state file. Stops the tool on a refusal, having changed
    nothing when the refusal is before the change set runs."""
    region = aws.region_required()
    stack.use(_stack_name(kind, group), region, "Key=cbase:group,Value=%s/%s" % (kind, group))
    vm_ = kind == "vm"
    cli.out("group-init: %s group %s in account %s (%s), stack %s\n" % (kind, group, aws.account, region, stack.stk_name))
    stack.status()
    if vm_:
        _vm_instance(o, region)
        stack.param_add("CloudFrontPrefixList", _prefix_list(GROUP_PREFIX_LIST))
        if o.ipv4:
            stack.param_add("CloudFrontPrefixListIpv4", _prefix_list(GROUP_PREFIX_LIST_IPV4))
            cli.out("address: dual-stack (--ipv4): a public IPv4 is about $3.60 a month, an Elastic IP\n")
        else:
            cli.out("address: IPv6-only, no public IPv4 (--ipv4 adds one, about $3.60 a month)\n")
        stack.param_add("ArtifactBucket", state.bucket())
        stack.stk_caps = "CAPABILITY_IAM"
    else:
        cli.out("bucket: S3 bills what it stores and serves (cents a month for a site)\n")
    stack.stk_template = cli.tmp_path("stack.yaml")
    cli.write_file(stack.stk_template, render(kind, group, o.ipv4))
    stack.changeset()
    if stack.stk_changes and vm_:
        if not _confirm(o.yes, "Run this change set on %s" % stack.stk_name):
            stack.discard()
            cli.err("cbase: a VM costs money, so group-init vm runs its change set only with --yes (or yes typed at a terminal): the change set is deleted, and nothing was made or changed\n")
            sys.exit(2)
    # The state bucket first: the instance's role names it, and the state file goes in it.
    _state_bucket_here(region)
    if stack.stk_changes:
        stack.execute()
    # The group's state file, empty, so apps and doctor see the group; one that exists is kept. A dual-stack
    # group's records its origin: the instance's public DNS name, dualstack. An IPv6-only instance has no
    # public DNS name of AWS's: CloudFront reaches it at an AAAA record of its fixed address, which the
    # group's first app names (CBASE_VM_ORIGIN) and its join records (addr=ipv6, state.sh).
    bucket = state.bucket()
    key = state.key(kind, group)
    origin = ""
    if vm_ and not o.ipv4:
        addr6 = stack.output("OriginIpv6")
        cli.out("origin: the instance claims %s at boot, and keeps it across a replacement. Make an AAAA record for it (vm.example.com AAAA %s) and name it once in the group's first app's cbase.conf: CBASE_VM_ORIGIN = vm.example.com\n"
                % (addr6, addr6))
    elif vm_:
        origin = _public_dns_of(group)
    state_file = cli.tmp_path("state-new")
    cli.write_file(state_file, "cbase-state %s %s %s%s\n" % (state.STATE_VERSION_TEXT, kind, group,
                                                              " origin=%s addr=dualstack" % origin if origin else ""))
    rc = aws.s3_put(bucket, key, state_file, "*")
    if rc == 0:
        cli.out("state file: s3://%s/%s (no members yet)\n" % (bucket, key))
    elif rc == 3:
        cli.out("state file: s3://%s/%s (kept as it is)\n" % (bucket, key))
    else:
        sys.exit(rc)
    gid = aws.group_find(kind, group)
    if gid is None:
        sys.exit(cli.EXIT_FAIL)
    cli.out("%s group %s: %s\n" % (kind, group, gid))


# ---- group-destroy ----------------------------------------------------------------------------


def destroy(kind: str, group: str, o: Opts) -> None:
    """Deletes an empty group's stack (refused while it has members, or a VM holds data unless
    DELETE_DATA), and its state file."""
    region = aws.region_required()
    stack.use(_stack_name(kind, group), region, "")
    cli.out("group-destroy: %s group %s in account %s (%s), stack %s\n" % (kind, group, aws.account, region, stack.stk_name))
    state.read(kind, group)
    members = state.apps()
    if len(members) > 0:
        n = len(members)
        cli.err("cbase: %s group %s still has %d member%s: %s. Undeploy each first (./cbase undeploy in its repository); nothing was changed.\n"
                % (kind, group, n, "" if n == 1 else "s", " ".join(members)))
        sys.exit(cli.EXIT_FAIL)
    stack.status()
    gid = ""
    data: List[str] = []
    if stack.stk_status != "NONE":
        found = aws.group_find(kind, group)
        if found is None:
            sys.exit(cli.EXIT_FAIL)
        gid = found
        if kind == "vm":
            # the data undeploy keeps: the instance's disk goes with it
            if not vm.data_check(gid, group, data):
                cli.err("cbase: the data check on %s ended %s, so whether the instance holds data is not known: nothing was changed (--delete-data destroys it anyway)\n"
                        % (gid, vm.ssm_status))
                if not o.delete_data:
                    sys.exit(cli.EXIT_FAIL)
    if len(data) > 0 and not o.delete_data:
        cli.err("cbase: vm group %s's instance %s holds the data of %s (/srv/cbase/<app>/data, kept by undeploy), which goes with the instance: refused. Copy it off, or run group-destroy --delete-data to destroy it. Nothing was changed.\n"
                % (group, gid, " ".join(data)))
        sys.exit(cli.EXIT_FAIL)
    vm_ = kind == "vm"
    if not _confirm(o.yes, "Delete %s%s" % (stack.stk_name, ", and the instance with its disk" if vm_ else "")):
        cli.err("cbase: group-destroy deletes the group's stack only with --yes (or yes typed at a terminal): nothing was changed\n")
        sys.exit(2)
    stack.delete()
    if kind == "static" and gid:
        # Retain kept the bucket and its files; with its tag gone, a group-init of the name makes a new one.
        rc, _o = aws.call("s3api", "delete-bucket-tagging", "--bucket", gid)
        if rc != 0:
            aws.fail("taking the group tag off s3://%s" % gid)
        cli.out("bucket: s3://%s is kept with its files, its cbase:static-group tag taken off (delete it with aws s3 rb s3://%s --force)\n" % (gid, gid))
    bucket = state.bucket()
    key = state.key(kind, group)
    if state.etag() != "*":
        rc, _o = aws.call("s3api", "delete-object", "--bucket", bucket, "--key", key)
        if rc != 0:
            aws.fail("deleting s3://%s/%s" % (bucket, key))
        cli.out("state file: s3://%s/%s deleted\n" % (bucket, key))
    cli.out("%s group %s is destroyed\n" % (kind, group))
