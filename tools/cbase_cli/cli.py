# cli.py - the deploy tool's shared parts (README.md here, the port of cli.c): the error exit, the usage
# text, the one process runner, the run's temporary directory, the regular expressions and the string
# helpers. Nothing here reads a setting or calls AWS.
from __future__ import annotations

import atexit
import os
import re as _re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Callable, List, Optional, Tuple

EXIT_FAIL = 1
EXIT_USAGE = 2

# Text is str, UTF-8 with surrogateescape everywhere, so bytes that are not UTF-8 pass through unchanged.
sys.stdout.reconfigure(encoding="utf-8", errors="surrogateescape")  # type: ignore[attr-defined]
sys.stderr.reconfigure(encoding="utf-8", errors="surrogateescape")  # type: ignore[attr-defined]

_ENC = "utf-8"
_ERRORS = "surrogateescape"


def out(text: str) -> None:
    """The only way to write stdout."""
    sys.stdout.write(text)


def err(text: str) -> None:
    """The only way to write stderr."""
    sys.stderr.write(text)


def fail(status: int, msg: str) -> None:
    """"cbase: <message>" on stderr, then exit with STATUS (stdout is flushed first, so the order of the
    two streams is the order of the events). Every error the tool stops on goes through here, so the
    prefix and the exit status are one rule."""
    sys.stdout.flush()
    sys.stderr.write("cbase: " + msg + "\n")
    sys.stderr.flush()
    sys.exit(status)


USAGE = (
    "usage: cbase/tools/cbase [-f FILE] COMMAND    (FILE: the app's settings, default ./cbase.conf)\n"
    "\n"
    "  config check       cbase.conf read and printed, defaults filled in (no AWS)\n"
    "  doctor             the config, the AWS account, each group and its members\n"
    "  apps [GROUP]       a group's apps, with each one's port, service and memory\n"
    "                     (default: cbase.conf's groups)\n"
    "  deploy [--dry-run] [--accept-drift] [--allow-downtime] [--no-prune] [--no-aliases|--attach]\n"
    "                     the app: its service on its group's VM, its static files, its\n"
    "                     distribution; DRY_RUN=1, CBASE_ACCEPT_DRIFT=1, CBASE_ALLOW_DOWNTIME=1\n"
    "                     and CBASE_PRUNE=0 are the same as the flags; --no-aliases and --attach\n"
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
    "  state join|leave   the membership step deploy and undeploy run, for repair\n"
)


def usage(to: Callable[[str], None]) -> None:
    """The shell tool's usage(): tools/cbase's header lines from "# usage:" to "#   state join|leave",
    with the "# " taken off each. README.md's "cbase/tools/cbase --help" block is the same text. TO is
    out or err."""
    to(USAGE)


# ---- text helpers --------------------------------------------------------------------------------


def text_of(s: str) -> str:
    """A captured text with its trailing newlines removed, as $(...) keeps it."""
    return s.rstrip("\n")


def first_line(s: str) -> str:
    """The text up to its first newline (all of it when it has none)."""
    return s.split("\n", 1)[0]


def replace_all(s: str, frm: str, to: str) -> str:
    """S with every FROM replaced by TO. FROM must not be empty."""
    return s.replace(frm, to)


# ---- processes -----------------------------------------------------------------------------------


def _decode(b: bytes) -> str:
    return b.decode(_ENC, _ERRORS)


def run(argv: List[str], out: bool = True, err: bool = True) -> Tuple[int, str, str]:
    """The one process runner (cbc_run_capture): ARGV[0] is searched on PATH, stdin is /dev/null, and a
    stream that is not captured (OUT or ERR False) is inherited. Returns (status, stdout text, stderr
    text): the child's exit status, 128 + N when it was killed by signal N, 127 when it cannot start
    (nothing printed). Both pipes are drained together, so neither can fill and block the child."""
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        proc = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE if out else None,
            stderr=subprocess.PIPE if err else None,
        )
    except (OSError, ValueError):
        return 127, "", ""
    ob, eb = proc.communicate()
    rc = proc.returncode
    if rc < 0:
        rc = 128 - rc
    return rc, _decode(ob) if ob is not None else "", _decode(eb) if eb is not None else ""


def on_path(name: str) -> bool:
    """Whether NAME is an executable on PATH (command -v); empty PATH entries are skipped."""
    if "/" in name:
        return os.access(name, os.X_OK)
    path = os.environ.get("PATH")
    if path is None:
        return False
    for d in path.split(":"):
        if d and os.access(d + "/" + name, os.X_OK):
            return True
    return False


# ---- files ---------------------------------------------------------------------------------------


def read_file(path: str) -> Optional[str]:
    """FILE's bytes, whole, as text; None when it cannot be opened or read."""
    try:
        with open(path, "r", encoding=_ENC, errors=_ERRORS, newline="") as f:
            return f.read()
    except OSError:
        return None


def write_file(path: str, text: str) -> None:
    """Writes TEXT to PATH, created or truncated; stops the tool when that fails."""
    try:
        with open(path, "w", encoding=_ENC, errors=_ERRORS, newline="") as f:
            f.write(text)
    except (OSError, ValueError):
        fail(EXIT_FAIL, "cannot write " + path)


def is_file(path: str) -> bool:
    """PATH exists and is a regular file, following symbolic links."""
    try:
        return stat.S_ISREG(os.stat(path).st_mode)
    except (OSError, ValueError):
        return False


def is_dir(path: str) -> bool:
    """PATH exists and is a directory, following symbolic links."""
    try:
        return stat.S_ISDIR(os.stat(path).st_mode)
    except (OSError, ValueError):
        return False


# ---- splitting -----------------------------------------------------------------------------------
# Each split differs where the shell's read does: split_on is read -a with the separators SEPS (runs of
# them make one break, and empty pieces are dropped); split_tab cuts at the first tab only; split_tabs
# gives N fields at each tab and leaves a missing one None; read_tab_fields is IFS=$'\t' read -r, where
# runs of tabs separate and the last field keeps the rest of the line.


def split_on(text: str, seps: str) -> List[str]:
    res: List[str] = []
    p = 0
    n = len(text)
    while p < n:
        while p < n and text[p] in seps:
            p += 1
        if p >= n:
            break
        b = p
        while p < n and text[p] not in seps:
            p += 1
        res.append(text[b:p])
    return res


def split_tab(text: str) -> Tuple[str, str]:
    """TEXT cut at its first tab; the rest is what follows it ("" when there is no tab)."""
    i = text.find("\t")
    if i < 0:
        return text, ""
    return text[:i], text[i + 1:]


def split_tabs(line: str, n: int) -> List[Optional[str]]:
    """N fields, each cut at the next tab (the last one too); a field the line has none of is None."""
    parts = line.split("\t")[:n]
    res: List[Optional[str]] = list(parts)
    while len(res) < n:
        res.append(None)
    return res


def read_tab_fields(line: str, n: int) -> List[str]:
    """N fields, "" for a missing one; runs of tabs separate, and the last field keeps the rest of the
    line without its trailing tabs."""
    f = [""] * n
    p = 0
    ln = len(line)
    for i in range(n):
        while p < ln and line[p] == "\t":
            p += 1
        if p >= ln:
            break
        if i == n - 1:
            f[i] = line[p:].rstrip("\t")
            break
        b = p
        while p < ln and line[p] != "\t":
            p += 1
        f[i] = line[b:p]
        if p < ln:
            p += 1
    return f


def trim(s: str) -> str:
    """S without the blanks (space and tab) at either end."""
    return s.strip(" \t")


# ---- the run's temporary directory ---------------------------------------------------------------
# THE RUN'S TEMPORARY DIRECTORY. tmp_path makes one directory, cbase.XXXXXX under TMPDIR (/tmp when it is
# unset or empty), the first time a path is asked for. The whole directory is removed (rmtree) at a normal
# exit (atexit) and at SIGINT, SIGTERM or SIGHUP, as the shell tool's EXIT trap did for those three. So
# the C's cbc_tmp_note, which listed the paths a run made one by one, has no job left: callers do not
# record the files and directories they make in it.
#
# The signal handler removes the directory, restores the default action and raises the signal again, so
# the process ends by that signal (status 128 + N). It does not wait for a child: a child left running is
# the terminal's process group's business. A copy of the handler in a child that is not the owner (the
# owner check, getpid against _tmp_owner) does not remove its parent's directory. A signal that was
# ignored when the directory was made (SIG_IGN, a background job's SIGINT) stays ignored.

_tmp_dir: Optional[str] = None
_tmp_owner = 0
_TMP_SIGS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)


def _tmp_remove_all() -> None:
    if _tmp_dir is not None and os.getpid() == _tmp_owner:
        shutil.rmtree(_tmp_dir, ignore_errors=True)


def _tmp_on_signal(sig: int, frame: object) -> None:
    _tmp_remove_all()
    signal.signal(sig, signal.SIG_DFL)
    os.kill(os.getpid(), sig)
    os._exit(128 + sig)


def _tmp_make() -> None:
    global _tmp_dir, _tmp_owner
    base = os.environ.get("TMPDIR")
    if not base:
        base = "/tmp"
    try:
        d = tempfile.mkdtemp(prefix="cbase.", dir=base)
    except OSError:
        fail(EXIT_FAIL, "cannot make a temporary directory in " + base)
        return
    _tmp_dir = d
    _tmp_owner = os.getpid()
    atexit.register(_tmp_remove_all)
    for sig in _TMP_SIGS:
        if signal.getsignal(sig) == signal.SIG_IGN:
            continue
        signal.signal(sig, _tmp_on_signal)


def tmp_path(name: str) -> str:
    """A path in the run's temporary directory, named NAME; removed when the tool exits, or when it is
    stopped by SIGINT, SIGTERM or SIGHUP."""
    if _tmp_dir is None:
        _tmp_make()
    assert _tmp_dir is not None
    return _tmp_dir + "/" + name


def signals_at_start() -> None:
    """First thing in main: SIGPIPE has its default action (a closed reader ends the tool, as it ended
    the C one; Python ignores it), and SIGINT too unless it was ignored at the start."""
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    if signal.getsignal(signal.SIGINT) is signal.default_int_handler:
        signal.signal(signal.SIGINT, signal.SIG_DFL)


def sleep_ms(ms: int) -> None:
    time.sleep(ms / 1000.0)


# ---- numbers -------------------------------------------------------------------------------------


def cksum(s: str) -> int:
    """POSIX cksum: the CRC-32 of the bytes, then of the length (least significant byte first, as the
    standard has it), complemented."""
    table = _cksum_table()
    crc = 0
    data = s.encode(_ENC, _ERRORS)
    for b in data:
        crc = ((crc << 8) ^ table[((crc >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    n = len(data)
    while n:
        crc = ((crc << 8) ^ table[((crc >> 24) ^ (n & 0xFF)) & 0xFF]) & 0xFFFFFFFF
        n >>= 8
    return ~crc & 0xFFFFFFFF


_CKSUM_TABLE: List[int] = []


def _cksum_table() -> List[int]:
    if not _CKSUM_TABLE:
        for i in range(256):
            r = i << 24
            for _ in range(8):
                r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
            _CKSUM_TABLE.append(r)
    return _CKSUM_TABLE


_INT_RE = _re.compile(r"-?[0-9]+\Z", _re.A)


def parse_int(s: str, lo: int, hi: int) -> Optional[int]:
    """lib/num_parse.h's rule: -?[0-9]+ (ASCII digits, no blanks, no '+'), the '-' only when lo < 0, the
    value in lo..hi; None otherwise."""
    if not _INT_RE.match(s):
        return None
    if s[0] == "-" and lo >= 0:
        return None
    v = int(s)
    if v < lo or v > hi:
        return None
    return v


def sort_bytes(items: List[str]) -> List[str]:
    """Sorted by the UTF-8 bytes, as strcmp under LC_ALL=C sorts."""
    return sorted(items, key=lambda x: x.encode(_ENC, _ERRORS))


# ---- regular expressions -------------------------------------------------------------------------


def _re_flags(pattern: str, s: str, flags: int) -> bool:
    try:
        rx = _re.compile(pattern, flags | _re.A)
    except _re.error:
        fail(EXIT_FAIL, "internal: bad pattern " + pattern)
        return False
    return rx.search(s) is not None


def re(pattern: str, s: str) -> bool:
    """Whether PATTERN (a Python translation of the C's ERE: anchor with ^ and \\Z, ASCII classes spelled
    out) matches anywhere in S. A bad pattern stops the tool."""
    return _re_flags(pattern, s, 0)


def re_icase(pattern: str, s: str) -> bool:
    """re, ignoring ASCII case (grep -i)."""
    return _re_flags(pattern, s, _re.I)


# ---- the deploy flags ----------------------------------------------------------------------------


class Flags:
    """The deploy flags, read once: tools/cbase takes each as an environment variable too (DRY_RUN=1 is
    --dry-run), and the command line sets the same fields. A flag is on only when its variable is exactly
    "1", except prune, which is on unless CBASE_PRUNE is exactly "0"; dry_banner is the one other rule:
    any non-empty DRY_RUN names the dry run in the deploy banner, whether or not it is "1"."""

    def __init__(self) -> None:
        self.dry_run = False
        self.dry_banner = False
        self.accept_drift = False
        self.prune = False   # ON by default: a successful deploy deletes the leftovers
        self.allow_downtime = False


flags = Flags()


def flags_from_env() -> None:
    """Fills flags from DRY_RUN, CBASE_ACCEPT_DRIFT, CBASE_PRUNE (on unless "0") and
    CBASE_ALLOW_DOWNTIME. Called once, first thing in main."""
    dry = os.environ.get("DRY_RUN")
    flags.dry_run = dry == "1"
    flags.dry_banner = bool(dry)
    flags.accept_drift = os.environ.get("CBASE_ACCEPT_DRIFT") == "1"
    flags.prune = os.environ.get("CBASE_PRUNE") != "0"
    flags.allow_downtime = os.environ.get("CBASE_ALLOW_DOWNTIME") == "1"
