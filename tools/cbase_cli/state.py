# state.py - the state file (the port of state.c): a group's members, read from S3 with its ETag, changed
# and written back with S3's conditional PUT, retried when another writer won, and the memory fit a vm
# join is checked against.
from __future__ import annotations

import os
import re
from typing import List, Optional, Tuple

import aws
import cli
import conf

STATE_VERSION_TEXT = "1"   # the state file's format version, the second field of its header line

_PORT_MIN = 8700
_PORT_MAX = 8799
_ATTEMPTS = 8

origin_in = ""        # the joining app's CBASE_VM_ORIGIN, or "" (the state command sets it from cbase.conf)
origin_need = False   # true when a join with no origin recorded or named is refused (deploy's vm step sets it)
fit_limit = -1        # the MiB the vm group's instance leaves for apps, or -1 for no fit check (vm sets it)
fit_what = ""         # how the limit came about, for the report
legacy_port = ""      # a grandfathered member's own port, or "" (deploy's vm step sets it)
legacy_data = ""      # a grandfathered member's own data directory, or ""
port = ""             # the port the last vm join holds
joined_new = False    # true when the last join added the app (it was not a member)

# The file as read. Member i is _lines[i], app _apps[i], port _ports[i] (-1 for a static member).
_lines: List[str] = []
_apps: List[str] = []
_ports: List[int] = []
member_origin = ""        # the header's origin, "" when none
member_origin_addr = ""   # ipv6 or dualstack
fit_report = ""           # the last memory fit's report, what the join prints
_etag_cur = ""
no_bucket = False         # a read with it set is taken as no file (a dry run's, when there is no state bucket yet)


def apps() -> List[str]:
    """The app names of the members the last read left."""
    return _apps


def etag() -> str:
    """The last read file's ETag, "*" when there was none."""
    return _etag_cur


def _members_clear() -> None:
    del _lines[:]
    del _apps[:]
    del _ports[:]


def _member_push(line: str, app: str, port_: int) -> None:
    _lines.append(line)
    _apps.append(app)
    _ports.append(port_)


def _member_remove(at_: int) -> None:
    del _lines[at_]
    del _apps[at_]
    del _ports[at_]


def _die(where: str, no: int, sentence: str) -> None:
    """A state file that is not the format."""
    cli.fail(cli.EXIT_FAIL, "state file %s line %d %s. It is written by ./cbase alone; fix or restore it by hand only with care." % (where, no, sentence))


def _match(pattern: str, s: str) -> Optional["re.Match[str]"]:
    try:
        rx = re.compile(pattern, re.A)
    except re.error:
        cli.fail(cli.EXIT_FAIL, "internal: bad pattern " + pattern)
        return None
    return rx.search(s)


_VM_LINE = "^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?) port=([0-9]{4,5}) service=([a-z0-9-]+)\\.service mem=~?([0-9]{1,6})( data=(/[A-Za-z0-9._/-]+) legacy=1)?\\Z"
_STATIC_LINE = "^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?) folder=([a-z0-9-]+)/\\Z"


def _parse(kind: str, group: str, file: str, where: str) -> None:
    """The members and header of FILE (WHERE names it in a message)."""
    global member_origin, member_origin_addr
    _members_clear()
    member_origin = ""
    member_origin_addr = ""
    data = cli.read_file(file)
    if data is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % file)
        return
    lines = data.split("\n")
    if lines[-1] == "":
        lines.pop()   # the file's last newline ends its last line; it starts none
    no = 0
    for line in lines:
        no += 1
        if no == 1:
            if kind == "vm":
                m = _match("^cbase-state %s vm %s origin=([a-z0-9.-]+) addr=(ipv6|dualstack)\\Z" % (STATE_VERSION_TEXT, group), line)
                if m:
                    member_origin = m.group(1)
                    member_origin_addr = m.group(2)
                    continue
            want = "cbase-state %s %s %s" % (STATE_VERSION_TEXT, kind, group)
            if line != want:
                _die(where, no, "is not \"%s\" (the format's version, the kind and the group; a vm group's may add \" origin=<host> addr=<ipv6|dualstack>\")" % want)
            continue
        if kind == "vm":
            m = _match(_VM_LINE, line)
            if not m:
                _die(where, no, "is not \"app=<name> port=<port> service=cbase-<name>.service mem=<MiB>\" (mem=~<MiB> when estimated; \" data=<dir> legacy=1\" after it for a grandfathered member)")
                return
            app = m.group(1)
            pv = cli.parse_int(m.group(3), 0, 99999)
            port_ = pv if pv is not None else 0
            svc = m.group(4)
            if svc != "cbase-%s" % app:
                cli.fail(cli.EXIT_FAIL, "state file %s line %d names service %s.service for app %s (it is cbase-%s.service). It is written by ./cbase alone; fix or restore it by hand only with care." % (where, no, svc, app, app))
            if m.group(6) is not None:
                if not (port_ >= 1024 and port_ <= 65535 and (port_ < _PORT_MIN or port_ > _PORT_MAX)):
                    _die(where, no, "holds the grandfathered port %d, which is not 1024-65535 outside %d-%d" % (port_, _PORT_MIN, _PORT_MAX))
            elif not (port_ >= _PORT_MIN and port_ <= _PORT_MAX):
                _die(where, no, "holds port %d, outside %d-%d" % (port_, _PORT_MIN, _PORT_MAX))
            for j in range(len(_apps)):
                if _ports[j] == port_:
                    _die(where, no, "gives port %d to %s, and %s holds it" % (port_, app, _apps[j]))
        else:
            m = _match(_STATIC_LINE, line)
            if not m:
                _die(where, no, "is not \"app=<name> folder=<name>/\"")
                return
            app = m.group(1)
            folder = m.group(3)
            if folder != app:
                _die(where, no, "gives app %s the folder %s/ (it is %s/)" % (app, folder, app))
            port_ = -1
        if app in _apps:
            _die(where, no, "lists %s again" % app)
        _member_push(line, app, port_)
    if no < 1:
        _die(where, 1, "is empty (it starts \"cbase-state %s %s %s\")" % (STATE_VERSION_TEXT, kind, group))


def _mem(line: str) -> Tuple[int, bool]:
    """A vm member line's declared MiB (or its estimate) and whether it is the estimate. The number is
    what follows the last " mem=", up to a blank (a grandfathered member's data follows it)."""
    i = line.rfind(" mem=")
    m = line[i + 5:] if i >= 0 else line
    est = False
    if m.startswith("~"):
        est = True
        m = m[1:]
    digits = ""
    k = 0
    while k < len(m) and m[k] != " " and k + 1 < 16:   # char digits[16]
        digits += m[k]
        k += 1
    v = cli.parse_int(digits, 0, 999999)
    return (v if v is not None else 0), est


def at(app: str) -> int:
    """The index of APP among the members as read left them, or -1."""
    for i in range(len(_apps)):
        if _apps[i] == app:
            return i
    return -1


def fit(app: str, new_line: str) -> bool:
    """Whether the members, with APP's line made NEW_LINE, fit in fit_limit MiB. Fills fit_report, the
    members by name; false when they do not fit."""
    global fit_report
    lines = list(_lines)
    pos = at(app)
    if pos >= 0:
        lines[pos] = new_line
    else:
        lines.append(new_line)
    total = 0
    notes = ""
    members = ""
    for line in cli.sort_bytes(lines):
        a = line[len("app="):]
        sp = a.find(" ")
        name = a if sp < 0 else a[:sp]
        mib, est = _mem(line)
        total += mib
        if est:
            notes += "  %s: memory not declared, counted as %dM\n" % (name, mib)
            members += "%s%s ~%d" % (", " if members else "", name, mib)
        else:
            members += "%s%s %d" % (", " if members else "", name, mib)
    fit_report = "%s  memory: %s = %d MiB of the %d MiB for apps (%s)" % (notes, members, total, fit_limit, fit_what)
    return total <= fit_limit


def origin(group: str) -> bool:
    """The group's origin, from the file or from the joining app's CBASE_VM_ORIGIN (ipv6) when it holds
    none. One VM has one origin. False when neither names one."""
    global member_origin, member_origin_addr
    if origin_in:
        if not member_origin:
            member_origin = origin_in
            member_origin_addr = "ipv6"
        elif member_origin != origin_in:
            cli.fail(cli.EXIT_FAIL, "%s's CBASE_VM_ORIGIN is %s, and vm group %s's VM is reached at %s (its state file): one VM can't have two origins. Drop CBASE_VM_ORIGIN, or set it to %s. Nothing was changed."
                     % (conf.conf.app, origin_in, group, member_origin, member_origin))
    return member_origin != ""


def _render(kind: str, group: str, file: str) -> None:
    """The header and the members sorted by app, written to FILE."""
    if member_origin:
        text = "cbase-state %s %s %s origin=%s addr=%s\n" % (STATE_VERSION_TEXT, kind, group, member_origin, member_origin_addr)
    else:
        text = "cbase-state %s %s %s\n" % (STATE_VERSION_TEXT, kind, group)
    for line in cli.sort_bytes(_lines):
        text += line + "\n"
    cli.write_file(file, text)


def port_of(group: str, legacy: str, at_: int) -> str:
    """The port a join of the member at AT_ (-1: a new member) holds: a grandfathered member's own
    (LEGACY, when set), the one it holds, or the lowest free one in 8700-8799. Stops the tool when every
    one is held."""
    if legacy:
        return legacy
    if at_ >= 0:
        if not _lines[at_].endswith(" legacy=1"):
            return "%d" % _ports[at_]
    for p in range(_PORT_MIN, _PORT_MAX + 1):
        if p not in _ports:
            return "%d" % p
    cli.fail(cli.EXIT_FAIL, "vm group %s has no free port: all of %d-%d are held" % (group, _PORT_MIN, _PORT_MAX))
    return ""


def bucket() -> str:
    """The state bucket's name, cbase-state-<account>."""
    return "cbase-state-%s" % aws.account


def key(kind: str, group: str) -> str:
    """The state file's key in the bucket, <kind>/<group>.txt."""
    return "%s/%s.txt" % (kind, group)


def read(kind: str, group: str) -> None:
    """The members and the ETag of the group's file ("*" when there is none yet, and then there are no
    members). Stops the tool on a corrupt file or a failed read."""
    global _etag_cur, member_origin, member_origin_addr
    bucket_s = bucket()
    key_s = key(kind, group)
    cur = cli.tmp_path("cur")
    try:
        os.unlink(cur)
    except OSError:
        pass
    # no_bucket reads no file at all: the same as a missing one.
    tag = None if no_bucket else aws.s3_get(bucket_s, key_s, cur)
    if tag is None:
        _etag_cur = "*"
        _members_clear()
        member_origin = ""
        member_origin_addr = ""
        return
    _etag_cur = tag
    _parse(kind, group, cur, "s3://%s/%s" % (bucket_s, key_s))


def _backoff(attempt: int, app: str) -> None:
    ms = 100 * (1 << (attempt - 1))
    if ms > 2000:
        ms = 2000
    ms += cli.cksum(app) % 100
    cli.sleep_ms(ms)


def vm_line(app: str, port_: str, mem: str, legacy_data_: str) -> str:
    """A vm member's line, "app=<a> port=<p> service=cbase-<a>.service mem=<m>", with " data=<dir> legacy=1"
    after it when LEGACY_DATA_ is set."""
    line = "app=%s port=%s service=cbase-%s.service mem=%s" % (app, port_, app, mem)
    if legacy_data_:
        line += " data=" + legacy_data_ + " legacy=1"
    return line


def change(kind: str, group: str, app: str, mem: str, op: str) -> None:
    """One join or leave (OP "join" or "leave") of APP with MEM as its declared memory, read, changed and
    conditionally written, retried on a lost race. Prints what it did. Stops the tool on a refusal, having
    changed nothing."""
    global port, joined_new
    bucket_s = bucket()
    key_s = key(kind, group)
    leaving = op == "leave"
    for attempt in range(1, _ATTEMPTS + 1):
        read(kind, group)
        pos = at(app)
        old_origin = ""
        if leaving:
            if pos < 0:
                cli.out("%s is not in %s group %s: nothing to leave\n" % (app, kind, group))
                return
            line = _lines[pos]
            _member_remove(pos)
        else:
            port_s = ""
            if kind == "vm":
                old_origin = member_origin
                if not origin(group) and origin_need:
                    cli.fail(cli.EXIT_FAIL, "vm group %s has no origin recorded and %s names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once, in this app's cbase.conf. Nothing was changed." % (group, app))
                port_s = port_of(group, legacy_port, pos)
                port = port_s
                if legacy_port:
                    for i in range(len(_ports)):
                        if i == pos:
                            continue
                        if "%d" % _ports[i] == port_s:
                            cli.err("cbase: %s's port %s (CBASE_VM_PORT) is %s's in vm group %s: nothing was changed\n" % (app, port_s, _apps[i], group))
                            raise SystemExit(cli.EXIT_FAIL)
                line = vm_line(app, port_s, mem, legacy_data)
                if fit_limit >= 0 and not fit(app, line):
                    cli.err("%s\n" % fit_report)
                    cli.fail(cli.EXIT_FAIL, "%s does not fit on vm group %s: its members would need more than the %d MiB its instance leaves for apps (%s). Declare less memory (CBASE_MEMORY), put the app in another group, or give the group a bigger instance (./cbase group-init vm %s --type <type>). Nothing was changed."
                             % (app, group, fit_limit, fit_what, group))
            else:
                line = "app=%s folder=%s/" % (app, app)
            joined_new = pos < 0
            if pos >= 0 and _lines[pos] == line and old_origin == member_origin:
                cli.out("%s is in %s group %s: %s\n" % (app, kind, group, line))
                if fit_limit >= 0 and kind == "vm":
                    cli.out("%s\n" % fit_report)
                return
            if pos >= 0:
                _lines[pos] = line
            else:
                port_n = -1
                if kind == "vm":
                    pv = cli.parse_int(port_s, 0, 99999)
                    port_n = pv if pv is not None else 0
                _member_push(line, app, port_n)
        new_path = cli.tmp_path("new")
        _render(kind, group, new_path)
        rc = aws.s3_put(bucket_s, key_s, new_path, _etag_cur)
        if rc == 0:
            if leaving:
                cli.out("%s left %s group %s (it held: %s)\n" % (app, kind, group, line))
            else:
                cli.out("%s joined %s group %s: %s\n" % (app, kind, group, line))
                if fit_limit >= 0 and kind == "vm":
                    cli.out("%s\n" % fit_report)
            return
        cli.err("cbase: s3://%s/%s changed while %s's %s was being written (attempt %d of %d): reading it again\n"
                % (bucket_s, key_s, app, op, attempt, _ATTEMPTS))
        if attempt != _ATTEMPTS:
            _backoff(attempt, app)
    cli.fail(cli.EXIT_FAIL, "s3://%s/%s kept changing: %s's %s lost %d conditional writes in a row; run it again" % (bucket_s, key_s, app, op, _ATTEMPTS))


def list_(kind: str, group: str) -> int:
    """The group's members as a table, on stdout. Returns 3 when the group has no state file yet (nothing
    printed for it), 0 otherwise. Stops the tool on a corrupt file or a failed read."""
    read(kind, group)
    if _etag_cur == "*":
        return 3
    n = len(_lines)
    cli.out("%s group %s (s3://%s/%s): %d app%s\n" % (kind, group, bucket(), key(kind, group), n, "" if n == 1 else "s"))
    if n == 0:
        return 0
    if kind == "vm":
        cli.out("  %-24s %-5s %-34s %s\n" % ("app", "port", "service", "mem"))
    else:
        cli.out("  %-24s %s\n" % ("app", "folder"))
    for line in cli.sort_bytes(_lines):
        f = cli.split_on(line, " ")
        if kind == "vm":
            mib, est = _mem(line)
            m = "~%d counted" % mib if est else "%dM" % mib
            cli.out("  %-24s %-5s %-34s %s\n" % (f[0][4:], f[1][5:], f[2][8:], m))
        else:
            cli.out("  %-24s %s\n" % (f[0][4:], f[1][7:]))
    return 0
