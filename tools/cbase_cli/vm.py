# vm.py - the VM side of a deploy and an undeploy (the port of vm.c; tools/cbase.d/vm.sh's header has the
# whole design: the port is the origin, the socket holds the port across a restart, the secrets never pass
# this tool): the paths, the two units (vm_unit.service and vm_unit.socket, filled in), the memory fit, the
# server build and its checks, the SSM steps, the join and leave of the group's state file, and the prune
# of the old server binaries. Every name and every message is the shell's.
from __future__ import annotations

import base64
import os
import re
import sys
from typing import Callable, List, Optional, Tuple

import aws
import cli
import conf as confmod
import deploy
import group
import state
import vm_remote

conf = confmod.conf

# MiB an instance keeps for itself: the kernel, systemd, journald and the SSM agent.
OS_RESERVE = 192
# The server's name in the build's dist/ directory (cbase.mk's serve-binary).
ARTIFACT_NAME = "cbase-serve-linux-arm64"

_INT64_MAX = 2 ** 63 - 1


def file(name: str) -> str:
    """The path of a file under cbase.d/ (tools/cbase.d/NAME)."""
    return "%s/cbase.d/%s" % (cli.tools_dir(), name)


def read(path: str) -> str:
    """The whole of PATH, or the tool stops ("cbase: cannot read PATH")."""
    data = cli.read_file(path)
    if data is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % path)
        return ""
    return data


def lines_of(text: str) -> List[str]:
    """The lines of TEXT as the C's loops see them: cut at each newline, no empty piece after the last."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def _transform_lines(text: str, fn: Callable[[str], Optional[str]]) -> str:
    """TEXT run line by line through FN (which returns the line to keep, or None to drop it). A line keeps
    its newline when the input's did (sed's behaviour)."""
    out: List[str] = []
    p = 0
    n = len(text)
    while p < n:
        nl = text.find("\n", p)
        if nl < 0:
            line = text[p:]
            end = n
            has_nl = False
        else:
            line = text[p:nl]
            end = nl + 1
            has_nl = True
        res = fn(line)
        if res is not None:
            out.append(res)
            if has_nl:
                out.append("\n")
        p = end
    return "".join(out)


def settings() -> str:
    """The SSM path of the app's settings, ending in '/' (vm_settings)."""
    if conf.vm_settings:
        return conf.vm_settings
    if conf.vm_stack:
        return "/%s/%s/" % (conf.vm_stack, conf.app)
    return "/cbase/%s/" % conf.app


def data_dir() -> str:
    """The app's data directory on the VM (vm_data_dir)."""
    if conf.vm_data:
        return conf.vm_data
    return "/srv/cbase/%s/data" % conf.app


def artifacts(account: str, region: str, state_bucket: str) -> Tuple[str, str]:
    """Where the app's servers go, as (bucket, prefix) (the prefix ends in '/'), from cbase.conf, else the
    account's code bucket in an adopted group, else the state bucket (vm_artifacts)."""
    if conf.vm_artifacts:
        a = conf.vm_artifacts
    elif conf.vm_stack:
        a = "s3://cbase-code-%s-%s/vm/%s/" % (account, region, conf.app)
    else:
        a = "s3://%s/artifacts/%s/" % (state_bucket, conf.app)
    if a.startswith("s3://"):
        a = a[5:]
    slash = a.find("/")
    if slash >= 0:
        return a[:slash], a[slash + 1:]
    return a, a


def backup_bucket(state_bucket: str) -> str:
    """Where serve puts the app's backups (vm_backup_bucket)."""
    return conf.backup_bucket if conf.backup_bucket else state_bucket


def unit(port_: str, region: str, backup_bucket_: str) -> str:
    """The app's service unit for PORT (vm_unit): REGION and BACKUP_BUCKET are the group's region and the
    backup bucket."""
    text = read(file("vm_unit.service"))
    app = conf.app
    user = "cbase-%s" % app
    data = data_dir()
    home = "/srv/cbase/%s" % app
    backup_key = conf.backup_key if conf.backup_key else "backups/%s/cbase.sqlite" % app
    # None: the MemoryMax line is deleted (an undeclared app)
    mem_line = None if conf.memory_estimated else "MemoryMax=%sM" % conf.memory

    def line_fn(line: str) -> Optional[str]:
        s = line
        s = s.replace("@APP@", app)
        s = s.replace("@USER@", user)
        s = s.replace("@PORT@", port_)
        s = s.replace("@DATA@", data)
        s = s.replace("@HOME@", home)
        s = s.replace("@REGION@", region)
        s = s.replace("@BACKUP_BUCKET@", backup_bucket_)
        s = s.replace("@BACKUP_KEY@", backup_key)
        if s == "@MEMORY_MAX@":
            return mem_line
        return s

    return _transform_lines(text, line_fn)


def socket(port_: str) -> str:
    """The app's socket unit for PORT (vm_socket)."""
    text = read(file("vm_unit.socket"))
    return _transform_lines(text, lambda line: line.replace("@APP@", conf.app).replace("@PORT@", port_))


def fit(type__: str, mib_: int) -> Tuple[int, str]:
    """The fit check's limit for an instance of TYPE with MIB memory (vm_fit_set): MIB less the OS's
    reserve, never below 0, and the sentence the report names it by."""
    limit = mib_ - OS_RESERVE
    if limit < 0:
        limit = 0
    return limit, "%s: %d MiB less %d for the OS" % (type__, mib_, OS_RESERVE)


def mib_text(type__: str, text: str) -> int:
    """The instance type's memory from what describe-instance-types printed for TYPE (vm_type_mib). Stops
    the tool when AWS named no size."""
    # The shell's $(...) keeps no trailing newline; the text is one value.
    m = text.rstrip("\n \t")
    v = cli.parse_int(m, 0, _INT64_MAX) if cli.re("^[0-9]+\\Z", m) else None
    if v is None:
        cli.fail(cli.EXIT_FAIL, "instance type %s has no memory size AWS names" % type__)
        return 0
    return v


def b64(text: str) -> str:
    """TEXT as one line of standard base64 with '=' padding and no newline, the shell's `base64 | tr -d
    '\\n'` (how a script travels to the instance in an SSM command)."""
    return base64.b64encode(text.encode("utf-8", "surrogateescape")).decode("ascii")


def sha256(path: str) -> Optional[str]:
    """The lower-case hex sha256 of the file at PATH (vm_sha256), or None when it could not be read."""
    rc, out, _e = cli.run(["sha256sum", path], err=False)
    if rc == 127:
        rc, out, _e = cli.run(["shasum", "-a", "256", path], err=False)
    if rc != 0 or not out:
        return None
    n = 0
    while n < len(out) and out[n] != " " and out[n] != "\n":
        n += 1
    return out[:n]


def check_elf(path: str) -> None:
    """Stops the tool with the shell's message unless PATH is a 64-bit little-endian Linux ELF binary for
    aarch64 (vm_build's check): \\177ELF, class 2 (64-bit), data 1 (little-endian), e_machine 0xb7."""
    head = b""
    try:
        with open(path, "rb") as f:
            head = f.read(20)
    except OSError:
        pass
    magic = head.hex()
    if magic.startswith("7f454c460201"):
        if len(magic) < 40 or magic[36:40] != "b700":
            cli.fail(cli.EXIT_FAIL, "%s is not an arm64 (aarch64) Linux binary, and the VM is arm64: build it with make serve-binary" % path)
        return
    cli.fail(cli.EXIT_FAIL, "%s is not a 64-bit Linux (ELF) binary: build it with make serve-binary" % path)


def build() -> Tuple[str, str]:
    """The linux-arm64 server, built by `make serve-binary` in . or server/, or named by CBASE_SERVE_BIN:
    (its path, its sha256) (vm_build). Stops the tool on any failure, as the shell does."""
    env = os.environ.get("CBASE_SERVE_BIN")
    if env:
        path = env
        if not cli.is_file(path):
            cli.fail(cli.EXIT_FAIL, "CBASE_SERVE_BIN names %s, which is not a file" % path)
        cli.out("server: %s (CBASE_SERVE_BIN)\n" % path)
    else:
        dir_ = None
        if cli.is_file("./Makefile"):
            dir_ = "."
        elif cli.is_file("server/Makefile"):
            dir_ = "server"
        if dir_ is None:
            cli.fail(cli.EXIT_FAIL, "no Makefile at . or server/, so there is no server to build: the server's Makefile includes cbase.mk, whose make serve-binary builds it (or set CBASE_SERVE_BIN to a linux-arm64 binary). Nothing was changed.")
            return "", ""
        cli.out("server: make -C %s serve-binary (linux arm64, static, in cbase's Alpine container)\n" % dir_)
        sys.stdout.flush()
        rc, _o, _e = cli.run(["make", "-C", dir_, "serve-binary"], out=False, err=False)
        if rc != 0:
            cli.fail(cli.EXIT_FAIL, "make -C %s serve-binary failed, so there is no linux-arm64 server to ship. It is cbase.mk's target for this build (ops/build_static.sh serve), and it needs Docker; run it until it passes, or set CBASE_SERVE_BIN to a linux-arm64 serve binary, then deploy again. Nothing was changed." % dir_)
        path = "%s/dist/%s" % (dir_, ARTIFACT_NAME)
        if not cli.is_file(path):
            cli.fail(cli.EXIT_FAIL, "make -C %s serve-binary made no %s (cbase.mk's serve-binary writes dist/%s)" % (dir_, path, ARTIFACT_NAME))
    check_elf(path)
    sha_ = sha256(path)
    if sha_ is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s to hash it" % path)
        return path, ""
    cli.out("server: %s, sha256 %s\n" % (path, sha_))
    return path, sha_


# ---- the AWS and SSM steps: aws.py's calls, state.py's join and leave ----------------------------------

# The VM this run deploys to or undeploys from (vm.sh's VM_* variables).
id_ = ""                # VM_ID: the instance
dns = ""                # VM_DNS: its public DNS name, or the group's origin
origin_addr = ""        # VM_ORIGIN_ADDR: ipv6 or dualstack, "" for none
type_ = ""              # VM_TYPE
mib = 0                 # VM_MIB
bin_ = ""               # VM_BIN: the server file
sha = ""                # VM_SHA
key = ""                # VM_KEY: the artifact's key in its bucket
port = ""               # VM_PORT: the port the app holds
new_member = False      # VM_NEW: the join added the app
origin_secret = ""      # VM_ORIGIN_SECRET: never printed
ssm_status = ""         # VM_SSM_STATUS: Success, Failed, ...
ssm_code = ""           # VM_SSM_CODE


def _stack_instance(stack_: str) -> str:
    """The running instance an adopted stack holds: its AWS::EC2::Instance, or the in-service instance of
    its auto-scaling group of one (vm_stack_instance)."""
    rc, out = aws.call("cloudformation", "describe-stack-resources", "--stack-name", stack_,
                       "--query", "StackResources[?ResourceType=='AWS::EC2::Instance' || ResourceType=='AWS::AutoScaling::AutoScalingGroup'].[ResourceType,PhysicalResourceId]",
                       "--output", "text")
    if rc != 0:
        aws.fail("reading the stack %s (CBASE_VM_STACK)" % stack_)
    rtype, rest = cli.split_tab(cli.first_line(out))
    # read -r type id: the first line's two words; a tab or a space between them.
    rid = rest.lstrip(" ")
    if rtype == "AWS::EC2::Instance":
        return rid
    if rtype == "AWS::AutoScaling::AutoScalingGroup":
        rc, ag = aws.call("autoscaling", "describe-auto-scaling-groups", "--auto-scaling-group-names", rid,
                          "--query", "AutoScalingGroups[0].Instances[?LifecycleState=='InService'].InstanceId",
                          "--output", "text")
        if rc != 0:
            aws.fail("reading the auto-scaling group %s of %s" % (rid, stack_))
        inst = cli.split_on(ag, " \t\n")
        if len(inst) != 1 or inst[0] == "None":
            cli.fail(cli.EXIT_FAIL, "the auto-scaling group %s of %s (CBASE_VM_STACK) has %d instances in service, not one: a group is one VM"
                     % (rid, stack_, len(inst)))
        return inst[0]
    cli.fail(cli.EXIT_FAIL, "the stack %s (CBASE_VM_STACK) holds no EC2 instance or auto-scaling group" % stack_)
    return ""


def _type_mib(type__: str) -> int:
    """The instance type's memory in MiB (describe-instance-types)."""
    rc, out = aws.call("ec2", "describe-instance-types", "--instance-types", type__,
                       "--query", "InstanceTypes[0].MemoryInfo.SizeInMiB", "--output", "text")
    if rc != 0:
        aws.fail("reading instance type %s" % type__)
    return mib_text(type__, out.rstrip("\n"))


def find(group_: str) -> None:
    """VM_ID, VM_DNS, VM_TYPE and VM_MIB for the group's running arm64 instance (vm_find)."""
    global id_, dns, type_, mib
    if conf.vm_stack:
        id_ = _stack_instance(conf.vm_stack)
    else:
        # aws.group_find prints its own message; the shell's exit inside its $(...) stops here.
        found = aws.group_find("vm", group_)
        if found is None:
            sys.exit(cli.EXIT_FAIL)
        id_ = found
    rc, out = aws.call("ec2", "describe-instances", "--instance-ids", id_,
                       "--query", "Reservations[0].Instances[0].[State.Name,PublicDnsName,InstanceType,Architecture]",
                       "--output", "text")
    if rc != 0:
        aws.fail("reading instance %s" % id_)
    # IFS=$'\t' read -r state VM_DNS VM_TYPE arch
    text = cli.first_line(out)
    st, text = cli.split_tab(text)
    dns_, text = cli.split_tab(text)
    type__, text = cli.split_tab(text)
    arch = text
    dns = "" if dns_ == "None" else dns_
    type_ = type__
    if st != "running":
        cli.fail(cli.EXIT_FAIL, "vm group %s's instance %s is %s, not running: start it (aws ec2 start-instances --instance-ids %s) and run this again"
                 % (group_, id_, st, id_))
    if arch != "arm64":
        cli.fail(cli.EXIT_FAIL, "vm group %s's instance %s is %s, and cbase builds the server for linux arm64 alone (make serve-binary)"
                 % (group_, id_, arch))
    mib = _type_mib(type_)
    cli.out("vm group %s: %s (%s, %d MiB), %s" % (group_, id_, type_, mib, dns if dns else "no public DNS name"))
    if conf.vm_stack:
        cli.out(", adopted from the stack %s" % conf.vm_stack)
    cli.out("\n")


def fit_set() -> None:
    """The fit check's limit for the join (state.fit_limit) and its sentence (vm_fit_set)."""
    limit, what = fit(type_, mib)
    state.fit_limit = limit
    state.fit_what = what


# The origin secret: SSM /<settings>CBASE_ORIGIN_SECRET (a SecureString). Its value is never printed: it is
# masked wherever it appears, and a bad one is refused without it.
_DRY_SECRET = "cbase-dry-run-stand-in-for-the-missing-origin-secret"


def origin_secret_read() -> None:
    """VM_ORIGIN_SECRET from SSM, masked (vm_origin_secret)."""
    global origin_secret
    name = "%sCBASE_ORIGIN_SECRET" % settings()
    rc, out = aws.call("ssm", "get-parameter", "--name", name, "--with-decryption",
                       "--query", "Parameter.Value", "--output", "text")
    if rc != 0:
        origin_secret = ""
        if aws.code() == "ParameterNotFound":
            cli.err("cbase: there is no SSM parameter %s, the origin secret CloudFront sends the app as X-Cbase-Origin (and serve checks). Make it once, then deploy again; nothing was changed:\n" % name)
            cli.err("  aws ssm put-parameter --type SecureString --name %s --value \"$(openssl rand -hex 32)\"\n" % name)
            deploy.dry_stop("no SSM parameter %s (the origin secret)" % name, "")
            origin_secret = _DRY_SECRET
            return
        aws.fail("reading the SSM parameter %s" % name)
    value = out.rstrip("\n")
    if not cli.re("^[A-Za-z0-9_-]{32,128}\\Z", value):
        origin_secret = ""
        cli.err("cbase: the SSM parameter %s is not 32 to 128 letters, digits, _ and - (its value is not shown): openssl rand -hex 32 makes one\n" % name)
        deploy.dry_stop("the SSM parameter %s is not an origin secret" % name, "")
        origin_secret = _DRY_SECRET
        return
    origin_secret = value
    aws.mask_add(value)
    cli.out("origin secret: %s (X-Cbase-Origin; its value is not shown)\n" % name)


def join(group_: str) -> None:
    """The app joins the group's state file, the port claimed and the memory fit checked in the one
    conditional write (vm_join)."""
    global port, new_member
    fit_set()
    state.legacy_port = conf.vm_port
    state.legacy_data = conf.vm_data
    state.origin_in = conf.vm_origin
    state.origin_need = True
    state.change("vm", group_, conf.app, confmod.mem(), "join")
    port = state.port
    new_member = state.joined_new


def _atol(s: str) -> int:
    """C's atol: blanks, a sign, digits, then whatever follows is ignored; 0 when there are none."""
    m = re.match(r"[ \t\n\v\f\r]*([+-]?[0-9]+)", s, re.A)
    return int(m.group(1)) if m else 0


def _polls() -> int:
    env = os.environ.get("CBASE_VM_POLLS")
    return _atol(env) if env else 180   # get-command-invocation reads before the wait gives up


_ssm_command_id = ""
_last_lines: List[str] = []   # the masked CBASE lines of the last ssm_run


def _ssm_read() -> bool:
    """One read of the command; True once it has ended (vm_ssm_read)."""
    global ssm_status, ssm_code
    rc, out = aws.call("ssm", "get-command-invocation", "--command-id", _ssm_command_id, "--instance-id", id_,
                       "--query", "[Status,ResponseCode]", "--output", "text")
    if rc == 0:
        ssm_status, ssm_code = cli.split_tab(cli.first_line(out))
        if ssm_status != "Pending" and ssm_status != "InProgress" and ssm_status != "Delayed":
            return True
    elif aws.code() != "InvocationDoesNotExist":
        aws.fail("reading SSM command %s" % _ssm_command_id)
    ssm_status = ""
    return False


def _ssm_run(script: str, what: str) -> None:
    """SCRIPT (the text) run as root on VM through SSM's AWS-RunShellScript, waited for with a bounded poll.
    Prints only its "CBASE: " lines (masked), as "vm: " lines. Sets ssm_status and ssm_code. The script
    travels base64, decoded to a file on the instance and run (vm_ssm_run)."""
    global _ssm_command_id, _last_lines
    cmd = "f=$(mktemp) && echo %s | base64 -d > $f && bash $f; rc=$?; rm -f $f; exit $rc" % b64(script)
    param = "commands=%s" % cmd
    rc, out = aws.call("ssm", "send-command", "--instance-ids", id_, "--document-name", "AWS-RunShellScript",
                       "--comment", "cbase %s" % what, "--timeout-seconds", "600", "--parameters", param,
                       "--query", "Command.CommandId", "--output", "text")
    if rc != 0:
        if aws.code() == "InvalidInstanceId":
            cli.fail(cli.EXIT_FAIL, "%s is not an SSM managed instance yet (a new instance takes a few minutes; its role must hold AmazonSSMManagedInstanceCore, as group-init's does)" % id_)
        aws.fail("sending the %s to %s (ssm send-command)" % (what, id_))
    cid = cli.first_line(out)
    cli.out("vm: the %s runs on %s (SSM command %s)\n" % (what, id_, cid))
    sys.stdout.flush()
    _ssm_command_id = cid
    if not aws.poll(_polls() + 1, _ssm_read):
        cli.fail(cli.EXIT_FAIL, "the %s on %s (SSM command %s) did not finish in time; its output may name the app's settings, so cbase does not print it (aws ssm get-command-invocation --command-id %s --instance-id %s)"
                 % (what, id_, cid, cid, id_))
    rc, res = aws.call("ssm", "get-command-invocation", "--command-id", cid, "--instance-id", id_,
                       "--query", "StandardOutputContent", "--output", "text")
    if rc != 0:
        aws.fail("reading SSM command %s's output" % cid)
    # grep '^CBASE: ', then aws.mask: the lines kept, parsed once; the sed that makes them "vm: " lines.
    _last_lines = []
    for line in res.split("\n"):
        if len(line) >= 8 and line.startswith("CBASE: "):
            _last_lines.append(aws.mask(line))   # the line, or the sentence that stands for it
    for line in _last_lines:
        if line.startswith("CBASE: "):
            cli.out("vm: %s\n" % line[7:])
        else:
            cli.out("%s\n" % line)


def _ssm_code_shown() -> str:
    """The exit status shown in the messages: the command's, or "?" when it has none."""
    return ssm_code if ssm_code else "?"


# VM_SERVICE, from the last "CBASE: cbase-<app>.service ..." line (the last one that says unchanged: not
# restarted, restarted or started) of the remote script's masked lines.
_service_text = ""


def service() -> str:
    """VM_SERVICE, what the deploy's summary says of the service, from the remote script's own line
    (vm_service_said): restarted, started, or unchanged and not restarted. Set by ship once the deploy
    script succeeded."""
    return _service_text


def _service_said() -> None:
    global _service_text
    u = "cbase-%s.service" % conf.app
    found = None
    for line in _last_lines:
        if not line.startswith("CBASE: ") or not line[7:].startswith(u):
            continue
        r = line[7 + len(u):]
        if r.startswith(" unchanged: not restarted") or r.startswith(" restarted ") or r.startswith(" started "):
            found = line
    _service_text = found[7:] if found is not None else "%s: the deploy did not say whether it restarted" % u


def ship(group_: str) -> bool:
    """The artifact uploaded (its sha256's key) and the service (re)started on VM. False when the service
    was not started (said why): a port taken on the VM releases the claim this deploy made (vm_ship)."""
    global key
    bucket, prefix = artifacts(aws.account, aws.region_required(), state.bucket())
    key = "%s%s" % (prefix, sha)
    region = aws.region_required()
    rc, _o = aws.call("s3api", "head-object", "--bucket", bucket, "--key", key,
                      "--query", "ETag", "--output", "text")
    if rc == 0:
        cli.out("artifact: s3://%s/%s is already this server\n" % (bucket, key))
    else:
        code = aws.code()
        if code != "404" and code != "NoSuchKey" and code != "NotFound":
            aws.fail("reading s3://%s/%s" % (bucket, key))
        rc, _o = aws.call("s3api", "put-object", "--bucket", bucket, "--key", key, "--body", bin_,
                          "--content-type", "application/octet-stream", "--query", "ETag", "--output", "text")
        if rc != 0:
            aws.fail("uploading s3://%s/%s" % (bucket, key))
        cli.out("artifact: s3://%s/%s\n" % (bucket, key))
    script = vm_remote.deploy(port, bucket, key, sha, region, state.bucket())
    cli.write_file(cli.tmp_path("vm-deploy.sh"), script)
    _ssm_run(script, "deploy of %s" % conf.app)
    if ssm_status == "Success":
        _service_said()
        return True
    if ssm_code == "3":
        if new_member:
            state.change("vm", group_, conf.app, "", "leave")
            cli.err("cbase: port %s is taken on %s by another process, so %s was not started, and its claim on the port is released. Free the port (or stop what holds it), then deploy again.\n"
                    % (port, id_, conf.app))
        else:
            cli.err("cbase: port %s is taken on %s by another process, so %s was not started; it keeps its port (it was a member before this deploy). Free the port, then deploy again.\n"
                    % (port, id_, conf.app))
        return False
    cli.err("cbase: the deploy of %s on %s ended %s (exit %s), above; the service's own log is on the instance: journalctl -u cbase-%s (aws ssm start-session --target %s)\n"
            % (conf.app, id_, ssm_status, _ssm_code_shown(), conf.app, id_))
    return False


def undeploy(group_: str, delete_data: bool) -> None:
    """The app's service stopped and removed, then its membership left (vm_undeploy)."""
    find(group_)
    script = vm_remote.undeploy(delete_data)
    cli.write_file(cli.tmp_path("vm-undeploy.sh"), script)
    _ssm_run(script, "undeploy of %s" % conf.app)
    if ssm_status != "Success":
        cli.fail(cli.EXIT_FAIL, "the undeploy of %s on %s ended %s (exit %s); it still holds its port, and nothing else was changed"
                 % (conf.app, id_, ssm_status, _ssm_code_shown()))
    state.change("vm", group_, conf.app, "", "leave")


# ---- vm_origin, vm_plan_dry and the entry points deploy, undeploy and group-destroy call ---------------


def origin(group_: str) -> None:
    """The group's origin (the state file's, or this app's CBASE_VM_ORIGIN recorded now), read before
    anything is changed (vm_origin). A dry run with neither lists the problem and goes on with the
    placeholder."""
    global dns, origin_addr
    state.origin_in = conf.vm_origin
    if conf.vm_stack and not group.state_bucket_found():
        state.no_bucket = True
        state.read("vm", group_)
        state.no_bucket = False
    else:
        state.read("vm", group_)
    held = state.member_origin
    if state.origin(group_):
        dns = state.member_origin
        origin_addr = state.member_origin_addr
        if held:
            cli.out("vm origin: %s (%s), the group's (its state file)\n" % (dns, origin_addr))
        else:
            cli.out("vm origin: %s (%s), the group's: CBASE_VM_ORIGIN, recorded in its state file by this deploy\n" % (dns, origin_addr))
        return
    what = "vm group %s has no origin recorded and %s names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once" % (group_, conf.app)
    deploy.dry_stop(what, "cbase: %s, in this app's cbase.conf. Nothing was changed." % what)
    dns = dns if dns else "unrecorded-origin.invalid"
    origin_addr = ""


def plan_dry(group_: str) -> None:
    """What a deploy would claim on the group, from the state file, writing nothing (vm_plan_dry). Sets
    port. Exits 1 when the app would not fit."""
    global port
    fit_set()
    state.read("vm", group_)
    at = state.at(conf.app)
    state.port = state.port_of(group_, conf.vm_port, at)
    line = state.vm_line(conf.app, state.port, confmod.mem(), conf.vm_data)
    if not state.fit(conf.app, line):
        cli.err("%s\n" % state.fit_report)
        cli.err("cbase: %s would not fit on vm group %s (%s)\n" % (conf.app, group_, state.fit_what))
        sys.exit(cli.EXIT_FAIL)
    cli.out("vm: would join vm group %s as %s\n" % (group_, line))
    cli.out("%s\n" % state.fit_report)
    port = state.port


# ---- the entry points ----
# tools/cbase's deploy calls, in this order: deploy_find (step 1), deploy_build (step 3, not on a dry run),
# deploy_join (step 5, with the stack's VmOrigin = dns, VmPort = port and OriginSecret = origin_secret),
# then after the stack executes ship (step 7; on False it calls stack.discard and exits 1). undeploy calls
# undeploy. group-destroy calls data_check on the instance it found.


def deploy_find(group_: str) -> None:
    """Deploy step 1 for a VM group (vm_find, the paths line, vm_origin, the adopted group's state bucket,
    vm_origin_secret)."""
    find(group_)
    bucket, prefix = artifacts(aws.account, aws.region_required(), state.bucket())
    cli.out("vm paths: settings SSM %s, servers s3://%s/%s\n" % (settings(), bucket, prefix))
    origin(group_)
    if conf.vm_stack and group.state_bucket_adopted(aws.region_required(), group_, cli.flags.dry_run, conf.app, conf.vm_port, conf.vm_data):
        state.no_bucket = True   # a dry run: the account has no state bucket, so no file is read (STATE_NO_BUCKET=1)
    origin_secret_read()


def deploy_build() -> None:
    """Deploy step 3 (vm_build): sets bin_ and sha. Not on a dry run."""
    global bin_, sha
    bin_, sha = build()


def deploy_join(group_: str, dry: bool) -> None:
    """Deploy step 5 (vm_join, or vm_plan_dry when DRY is true)."""
    if dry:
        plan_dry(group_)
    else:
        join(group_)


def data_check(instance: str, group_: str, apps: List[str]) -> bool:
    """group-destroy's check of which apps' data is on INSTANCE: fills APPS and returns whether the SSM
    command ended Success (ssm_status)."""
    global id_
    id_ = instance
    script = vm_remote.data()
    cli.write_file(cli.tmp_path("vm-data.sh"), script)
    _ssm_run(script, "data check of vm group %s" % group_)
    for line in _last_lines:
        if len(line) >= 13 and line.startswith("CBASE: data "):
            apps.append(line[12:])
    return ssm_status == "Success"


# ---- the prune of the app's old server binaries ----


class _Binary:
    def __init__(self, key_: str, when: str) -> None:
        self.key = key_
        self.when = when   # the object's LastModified (ISO 8601: its text sorts chronologically)


_SHA256_RE = re.compile(r"[0-9a-f]{64}\Z", re.A)


def _is_sha256(s: str) -> bool:
    """Exactly 64 lowercase hex characters, and nothing else."""
    return _SHA256_RE.match(s) is not None


def _bytes(s: str) -> bytes:
    return s.encode("utf-8", "surrogateescape")


def _newer(a: _Binary, b: _Binary) -> bool:
    """Whether A is newer than B (LastModified, then the key, so a tie is still one answer)."""
    wa, wb = _bytes(a.when), _bytes(b.when)
    if wa != wb:
        return wa > wb
    return _bytes(a.key) > _bytes(b.key)


def prune_artifacts(dry: bool) -> None:
    """deploy --prune's second half, after the stack is executed: deletes the app's old server binaries, the
    keys under its artifacts prefix that are exactly a sha256 (64 lowercase hex), and keeps the one this
    deploy shipped (key) and the most recently modified other (the rollback target). Nothing else is a
    candidate; a listed key outside the prefix is named and left alone. A DRY run (no build, so no current
    key) only prints what it would delete, keeping the newest as the previous."""
    bucket, prefix = artifacts(aws.account, aws.region_required(), state.bucket())
    region = aws.region_required()
    current = key if key else None   # None in a dry run: nothing is built, so the sha is not known
    rc, text = aws.call("s3api", "list-objects-v2", "--bucket", bucket, "--prefix", prefix, "--region", region,
                        "--query", "Contents[].[Key,LastModified]", "--output", "text")
    if rc != 0:
        if dry:   # a first deploy: the bucket may not exist yet (the real deploy makes or finds it), so nothing is old
            cli.out("would prune 0 old server binaries: s3://%s/%s cannot be listed yet\n" % (bucket, prefix))
            return
        aws.fail("listing s3://%s/%s" % (bucket, prefix))
    body = cli.text_of(text)
    v: List[_Binary] = []
    pl = len(prefix)
    # The listing: a line a key, the key and its time tab-separated (no key holds a tab).
    for line in body.split("\n"):
        tab = line.find("\t")
        if tab >= 0 and line:
            k = line[:tab]
            when = line[tab + 1:]
            # THE FOLDER GUARD: whatever the listing holds, only a key under the prefix is ever a candidate.
            if not k.startswith(prefix):
                cli.err("cbase: prune: s3://%s/%s is outside %s, and is left alone\n" % (bucket, k, prefix))
            elif _is_sha256(k[pl:]):   # a binary: nothing else (a sidecar, a script, a folder) is
                v.append(_Binary(k, when))
    # Kept: the binary this deploy ships, and the newest other one (the rollback target).
    prev: Optional[_Binary] = None
    for b in v:
        if current is not None and b.key == current:
            continue
        if prev is None or _newer(b, prev):
            prev = b
    gone = 0
    del_: List[str] = []
    for b in v:
        if b is prev or (current is not None and b.key == current):
            continue
        if dry:
            cli.out("would prune s3://%s/%s\n" % (bucket, b.key))
        else:
            del_.append(b.key)
        gone += 1
    if not dry:
        aws.s3_delete_keys(bucket, region, del_)   # batched; prints one "pruned" line a key
    cur = current if current is not None else "the build's own binary, not made in a dry run,"
    kept = "%s and %s" % (cur, prev.key) if prev is not None else cur
    cli.out("%s %d old server binar%s under s3://%s/%s (%s %s)\n"
            % ("would prune" if dry else "pruned", gone, "y" if gone == 1 else "ies", bucket, prefix,
               "keeping" if dry else "kept", kept))
