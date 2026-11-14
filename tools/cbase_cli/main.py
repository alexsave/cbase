# main.py - the deploy tool's entry (the port of main.c): option and command dispatch, in the order
# tools/cbase checks them (README.md "./cbase deploy tool", tools/cbase's header). Every usage error is
# checked before any setting is read or any AWS call is made, as the shell does it.
from __future__ import annotations

import os
import sys
import traceback
from typing import List, Tuple

import aws
import cli
import conf as confmod
import deploy
import group as grp
import stack
import state

conf = confmod.conf


def _conf_groups() -> List[Tuple[str, str]]:
    """The groups cbase.conf names, the VM first (conf.sh's conf_groups, tools/cbase)."""
    res: List[Tuple[str, str]] = []
    if conf.vm != "none":
        res.append(("vm", conf.vm))
    if conf.stat != "none":
        res.append(("static", conf.stat))
    return res


def _file_exists_in(d: str, name: str) -> bool:
    """Whether DIR/NAME is a regular file (the shell's [ -f ])."""
    return cli.is_file(d + "/" + name)


def _arg_error(msg: str) -> None:
    """An argument the shell refuses (its "echo ... >&2; exit 2"): the message, no usage."""
    cli.fail(cli.EXIT_USAGE, msg)


def _usage_block_then_exit() -> None:
    cli.usage(cli.err)
    sys.exit(cli.EXIT_USAGE)


def _run(argv: List[str]) -> int:
    cli.flags_from_env()
    conf_file = "cbase.conf"
    argc = len(argv)
    i = 0
    while i < argc:
        a = argv[i]
        if a == "-f":
            if i + 1 >= argc:
                cli.fail(cli.EXIT_USAGE, "-f wants a file")
            conf_file = argv[i + 1]
            i += 2
        elif a == "-h" or a == "--help" or a == "help":
            cli.usage(cli.out)
            return 0
        elif a[:1] == "-":
            cli.err("cbase: unknown option %s\n" % a)
            _usage_block_then_exit()
        else:
            break
    if i >= argc:
        _usage_block_then_exit()

    cmd = argv[i]
    args = argv[i + 1:]
    nargs = len(args)

    if cmd == "config":
        if nargs != 1 or args[0] != "check":
            _arg_error("config check is the one config command")
        confmod.load(conf_file)
        cli.out("%s: ok\n" % conf_file)
        confmod.print_conf()
        return 0
    if cmd == "doctor":
        if nargs != 0:
            _arg_error("doctor takes no arguments")
        confmod.load(conf_file)
        cli.out("%s: ok\n" % conf_file)
        confmod.print_conf()
        aws.init()
        cli.out("account   %s (%s)\n" % (aws.account, aws.region()))
        rc = 0
        for kind, name in _conf_groups():
            gid = aws.group_find(kind, name)
            if gid is None:
                rc = 1
                continue
            cli.out("%s group %s: %s\n" % (kind, name, gid))
            lrc = state.list_(kind, name)
            if lrc == 3:
                cli.out("%s group %s: no state file yet (no app has joined)\n" % (kind, name))
        return rc
    if cmd == "apps":
        if nargs > 1:
            _arg_error("apps takes one group name at most")
        if nargs == 1 and not confmod.name_ok(args[0]):
            cli.fail(cli.EXIT_USAGE, "%s is not a group name" % args[0])
        if nargs == 1:
            groups = [("vm", args[0]), ("static", args[0])]
        else:
            confmod.load(conf_file)
            groups = _conf_groups()
        aws.init()
        found = False
        for kind, name in groups:
            rc = state.list_(kind, name)
            if rc == 0:
                found = True
            elif rc != 3:
                sys.exit(rc)
        if not found:
            who = ", ".join("%s %s group" % (kind, name) for kind, name in groups)
            cli.fail(cli.EXIT_FAIL, "no state file for %s: no app has joined it yet" % who)
        return 0
    if cmd == "state":
        op = args[0] if nargs > 0 else ""
        if op != "join" and op != "leave":
            _arg_error("state join or state leave")
        if nargs != 1:
            cli.fail(cli.EXIT_USAGE, "state %s takes no more arguments" % op)
        confmod.load(conf_file)
        aws.init()
        for kind, name in _conf_groups():
            gid = aws.group_find(kind, name)
            if gid is None:
                sys.exit(cli.EXIT_FAIL)
            state.origin_in = conf.vm_origin
            state.change(kind, name, conf.app, confmod.mem(), op)
        return 0
    if cmd == "template":
        if nargs != 0:
            _arg_error("template takes no arguments")
        confmod.load(conf_file)
        # Placeholders where deploy puts what it finds: the static group's bucket and region, and the
        # certificate (found or requested).
        bucket = conf.static_bucket if conf.static_bucket else conf.stat + "-bucket"
        prefix = conf.static_prefix if conf.static_prefix else conf.app
        stack.stk_bucket = bucket
        stack.stk_prefix = prefix
        stack.stk_bucket_region = "us-east-1"
        if conf.stat != "none" and _file_exists_in(conf.static_dir, "index.html"):
            # The router's page list is the build's, when there is one to read.
            deploy.scan(conf.static_dir)
        if conf.domain:
            stack.stk_cert = conf.cert_arn if conf.cert_arn else \
                "arn:aws:acm:us-east-1:111122223333:certificate/00000000-0000-0000-0000-000000000000"
            stack.stk_api_cert = conf.api_cert_arn if conf.api_cert_arn else stack.stk_cert
        path = cli.tmp_path("stack.yaml")
        stack.render(path)
        cli.out("# PLACEHOLDERS: the bucket (%s) is static group %s's%s.\n"
                % (bucket, conf.stat, ", the certificate is the one deploy finds or requests" if conf.domain else ""))
        text = cli.read_file(path)
        if text is None:
            cli.fail(cli.EXIT_FAIL, "cannot read %s" % path)
            return 1
        cli.out(text)
        return 0
    if cmd == "group-template":
        kind = args[0] if nargs > 0 else ""
        group = args[1] if nargs > 1 else ""
        if kind != "vm" and kind != "static":
            _arg_error("group-template vm GROUP or group-template static GROUP")
        n = nargs
        if kind == "vm" and n == 3 and args[2] == "--ipv4":
            n = 2
        if not confmod.name_ok(group) or n != 2:
            cli.fail(cli.EXIT_USAGE, "group-template %s wants one group name%s"
                     % (kind, ", and --ipv4 or nothing" if kind == "vm" else ""))
        grp.template(kind, group, nargs == 3)
        return 0
    if cmd == "deploy":
        for a in args:
            if a not in ("--dry-run", "--accept-drift", "--prune", "--no-prune", "--allow-downtime",
                         "--no-aliases", "--attach"):
                cli.fail(cli.EXIT_USAGE, "deploy takes --dry-run, --accept-drift, --no-prune, --allow-downtime, --no-aliases and --attach, not %s" % a)
        deploy.deploy(conf_file, args)
        return 0
    if cmd == "undeploy":
        for a in args:
            if a != "--delete-files" and a != "--delete-data":
                cli.fail(cli.EXIT_USAGE, "undeploy takes --delete-files and --delete-data, not %s" % a)
        deploy.undeploy(conf_file, args)
        return 0
    if cmd == "group-init" or cmd == "group-destroy":
        kind = args[0] if nargs > 0 else ""
        group = args[1] if nargs > 1 else ""
        if kind != "vm" and kind != "static":
            cli.fail(cli.EXIT_USAGE, "%s vm GROUP or %s static GROUP" % (cmd, cmd))
        if not confmod.name_ok(group):
            # char shown[41]: the first 40 bytes
            shown = group.encode("utf-8", "surrogateescape")[:40].decode("utf-8", "surrogateescape")
            cli.fail(cli.EXIT_USAGE, "%s %s wants a group name (a lower-case letter, then lower-case letters, digits and '-', at most 32 characters), not \"%s\""
                     % (cmd, kind, shown))
        o = grp.args(cmd, kind, args[2:])
        aws.init()
        if cmd == "group-init":
            grp.init(kind, group, o)
        else:
            grp.destroy(kind, group, o)
        return 0
    cli.err("cbase: unknown command %s\n" % cmd)
    _usage_block_then_exit()
    return cli.EXIT_USAGE


def main() -> None:
    cli.signals_at_start()
    try:
        rc = _run(sys.argv[1:])
    except SystemExit:
        raise
    except Exception as e:   # the one place an unexpected exception is caught: no traceback reaches a user
        frames = traceback.extract_tb(e.__traceback__)
        where = ""
        if frames:
            last = frames[-1]
            where = " (%s:%d)" % (os.path.basename(last.filename), last.lineno or 0)
        cli.fail(cli.EXIT_FAIL, "internal error: %s: %s%s" % (type(e).__name__, e, where))
        return
    sys.exit(rc)
