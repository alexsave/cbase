# conf.py - cbase.conf, read and checked (the port of conf.c): the port of the shell tool's conf.sh, rule
# for rule and in the same order, so the first error a file gets is the one the shell gives. The format,
# the keys and the secret scan are conf.sh's header; the messages are conf.sh's words. Nothing here calls
# AWS.
from __future__ import annotations

import os
import re as _re
from typing import List, Optional

import cli

# The memory an undeclared VM app is counted at, MiB (the shell's CONF_MEMORY_ESTIMATE).
MEMORY_ESTIMATE = 128

CONF_KEYS = ("CBASE_APP CBASE_VM CBASE_VM_STACK CBASE_VM_ORIGIN CBASE_VM_PORT CBASE_VM_DATA CBASE_VM_SETTINGS "
             "CBASE_VM_ARTIFACTS CBASE_VM_REPLACES CBASE_BACKUP_KEY CBASE_BACKUP_BUCKET CBASE_STATIC "
             "CBASE_STATIC_BUCKET CBASE_STATIC_PREFIX CBASE_MEMORY CBASE_ROUTE CBASE_DOMAIN CBASE_CERT_ARN "
             "CBASE_API_CERT_ARN CBASE_ALIASES CBASE_ZONE CBASE_API_HOST CBASE_API_ROUTE CBASE_STATIC_DIR "
             "CBASE_HASHED CBASE_SHELL")


class Route:
    """One normalized route: "<pattern> <origin> <cache> <methods>", and the line it is on."""

    def __init__(self, pattern: str, origin: str, cache: str, methods: str, line: int) -> None:
        self.pattern = pattern
        self.origin = origin
        self.cache = cache
        self.methods = methods
        self.line = line


class Conf:
    """The settings as load leaves them. An unset string is "", as the shell's CONF_ variables are."""

    def __init__(self) -> None:
        self.file = ""
        self.app = ""
        self.vm = ""             # CONF_VM: the VM group, or "none"
        self.stat = ""           # CONF_STATIC: the static group, or "none"
        self.vm_stack = ""
        self.vm_origin = ""
        self.vm_port = ""        # decimal, as printed
        self.vm_data = ""
        self.vm_settings = ""
        self.vm_artifacts = ""
        self.vm_replaces = ""
        self.backup_key = ""
        self.backup_bucket = ""
        self.static_bucket = ""
        self.static_prefix = ""
        self.memory = ""         # the declared MiB in decimal, or ""
        self.memory_estimated = False
        self.domain = ""         # the first CBASE_DOMAIN hostname
        self.domains: List[str] = []
        self.cert_arn = ""
        self.api_cert_arn = ""
        self.zone = ""
        self.aliases = "on"      # "on" or "off"
        self.api_host = ""
        self.static_dir = ""     # no trailing '/'; "" for a VM-only app
        self.static_dir_default = False
        self.hashed: List[str] = []
        self.shell = ""
        self.routes: List[Route] = []
        self.api_routes: List[Route] = []


conf = Conf()

# The keys the file set, in the file's order: (key, line). conf_at reads the last of some keys.
_seen: List[tuple] = []


# ---- errors --------------------------------------------------------------------------------------


def _die_line(no: int, msg: str) -> None:
    """The file's line and why, and the tool stops (conf_die)."""
    cli.fail(cli.EXIT_FAIL, "%s line %d: %s" % (conf.file, no, msg))


def _die_file(what: str) -> None:
    """The file as a whole is wrong (conf_die_file)."""
    cli.fail(cli.EXIT_FAIL, "%s %s" % (conf.file, what))


# ---- the keys the file set -----------------------------------------------------------------------


def _conf_at(keys: str) -> int:
    """The line the last of KEYS (a blank-separated list) is on, or 0 when none is set."""
    wanted = keys.split(" ")
    at = 0
    for k, line in _seen:
        if k in wanted:
            at = line
    return at


def _key_set(keys: str) -> bool:
    return _conf_at(keys) != 0


def _conf_refuses(keys: str, sentence: str) -> None:
    """One of KEYS set stops the tool at the last one's line."""
    at = _conf_at(keys)
    if at:
        _die_line(at, sentence)


def _conf_requires(key: str, other: str, sentence: str) -> None:
    """KEY set without OTHER stops the tool at KEY's line."""
    at = _conf_at(key)
    if at and not _key_set(other):
        _die_line(at, sentence)


# ---- the value rules -----------------------------------------------------------------------------


def name_ok(s: str) -> bool:
    """A group name or CBASE_APP: a lower-case letter, then lower-case letters, digits and -, at most 32
    characters, not ending in -."""
    return cli.re(r"^[a-z]([a-z0-9-]{0,30}[a-z0-9])?\Z", s)


def _host_ok(h: str) -> bool:
    """A lower-case hostname. The shell splits it on '.' with IFS=. and checks each label; a final empty
    field is not one (bash drops it), an empty field in the middle is (and fails)."""
    if len(h.encode("utf-8", "surrogateescape")) > 253:
        return False
    if not cli.re(r"^[a-z0-9.-]+\Z", h):
        return False
    if "." not in h:
        return False
    start = 0
    while True:
        dot = h.find(".", start)
        end = dot if dot >= 0 else len(h)
        if dot < 0 and end == start:
            break   # a final empty field is dropped
        if not cli.re(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\Z", h[start:end]):
            return False
        if dot < 0:
            break
        start = dot + 1
    return True


def _printable_key(k: str) -> bool:
    """A key spelled as a name (upper-case, digits and _), which can be printed."""
    return cli.re(r"^[A-Z][A-Z0-9_]{0,63}\Z", k)


_KNOWN_METHODS = ["GET", "HEAD", "OPTIONS", "PUT", "POST", "PATCH", "DELETE"]


def _conf_methods(no: int, lst: str) -> str:
    """ALL, or the list's methods once each in a fixed order."""
    known_text = "GET HEAD OPTIONS PUT POST PATCH DELETE"
    if lst == "ALL":
        return "ALL"
    if not cli.re(r"^[A-Z]+(,[A-Z]+)*\Z", lst):
        _die_line(no, "CBASE_ROUTE's methods are ALL or a comma list of %s (upper case, no blanks)" % known_text)
    for m in lst.split(","):
        if m not in _KNOWN_METHODS:
            _die_line(no, "%s is not a method CloudFront passes (%s)" % (m, known_text))
    padded = "," + lst + ","
    out = ",".join(k for k in _KNOWN_METHODS if "," + k + "," in padded)
    if out == "GET,HEAD,OPTIONS,PUT,POST,PATCH,DELETE":
        return "ALL"
    return out


def _conf_route(no: int, value: str, api: bool) -> None:
    """One CBASE_ROUTE (or CBASE_API_ROUTE, when api), normalized and appended."""
    key = "CBASE_API_ROUTE" if api else "CBASE_ROUTE"
    rest = cli.split_on(value, " \t")
    pattern = rest[0] if len(rest) > 0 else ""
    origin = rest[1] if len(rest) > 1 else ""
    if not origin:
        _die_line(no, "%s is \"<pattern> <origin> [cache|nocache] [<methods>]\", and this one has no origin (vm, static or ws)" % key)
    if pattern != "*" and not cli.re(r"^/[A-Za-z0-9_.~*/-]{0,254}\Z", pattern):
        _die_line(no, "%s's pattern is \"*\" or a path starting with '/' of letters, digits and _ . ~ * / - (CloudFront's path patterns)" % key)
    if api:
        if pattern == "*":
            _die_line(no, "CBASE_API_ROUTE's pattern is a path: the API host's default is the VM already")
        if origin != "vm" and origin != "ws":
            _die_line(no, "CBASE_API_ROUTE's origin is vm or ws (the API host has no static files)")
    if origin == "vm" or origin == "static":
        pass   # a vm or static route takes any of the words below
    elif origin == "ws":
        if len(rest) != 2:
            _die_line(no, "a ws route takes no cache or methods: it is the VM, uncached, every method, with long timeouts")
    else:
        _die_line(no, "%s's origin is vm, static or ws" % key)
    cache = ""
    methods = ""
    for f in rest[2:]:
        if f == "cache" or f == "nocache" or f == "origin":
            if cache:
                _die_line(no, "%s names its cache policy twice" % key)
            if f == "origin" and origin != "vm":
                _die_line(no, "the cache mode origin (the origin's Cache-Control) is for a vm route")
            cache = f
        else:
            if methods:
                _die_line(no, "%s names its methods twice (one comma list, no blanks)" % key)
            methods = _conf_methods(no, f)
    if origin == "static":
        if not cache:
            cache = "cache"
        if not methods:
            methods = "GET,HEAD"
        allowed = ["GET", "HEAD", "OPTIONS", "GET,HEAD", "GET,OPTIONS", "HEAD,OPTIONS", "GET,HEAD,OPTIONS"]
        if methods not in allowed:
            _die_line(no, "a static route takes only GET, HEAD and OPTIONS (a bucket answers nothing else)")
    elif origin == "ws":
        cache = "nocache"
        methods = "ALL"
    else:
        if not cache:
            cache = "nocache"
        if not methods:
            methods = "ALL"
    taken = conf.api_routes if api else conf.routes
    for r in taken:
        if r.pattern == pattern:
            _die_line(no, "%s %s is routed twice" % (key, pattern))
    taken.append(Route(pattern, origin, cache, methods, no))


# conf_secret_scan: stops the tool when LINE looks like it holds a secret. The key's name, a PEM marker,
# an AWS access key id, or a long run of random-looking characters: the same tests the shell makes, in the
# same order, and the line is never printed.

_RUN_RES = [
    _re.compile(r"[A-Za-z0-9+/]+", _re.A),
    _re.compile(r"[0-9A-Fa-f]+", _re.A),
    _re.compile(r"[A-Za-z0-9]+", _re.A),
]
_UPPER = _re.compile(r"[A-Z]", _re.A)
_LOWER = _re.compile(r"[a-z]", _re.A)
_DIGIT = _re.compile(r"[0-9]", _re.A)


def _run_matches(text: str, which: int, minimum: int, need_mixed: bool) -> bool:
    """Whether TEXT holds a maximal run of WHICH characters at least MINIMUM long (that has an upper-case
    letter, a lower-case letter and a digit, when NEED_MIXED)."""
    for m in _RUN_RES[which].finditer(text):
        run = m.group(0)
        if len(run) < minimum:
            continue
        if not need_mixed:
            return True
        if _UPPER.search(run) and _LOWER.search(run) and _DIGIT.search(run):
            return True
    return False


def _ascii_upper(s: str) -> str:
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


def _secret_scan(no: int, text: str) -> None:
    key = ""
    eq = text.find("=")
    if eq >= 0:
        key = cli.trim(text[:eq])
    upper = _ascii_upper(key)
    for word in ("SECRET", "TOKEN", "PASSWORD", "PASSWD", "PRIVATE", "CREDENTIAL"):
        if word in upper:
            if _printable_key(key):
                _die_line(no, "%s is named like a secret. cbase.conf is committed and public, so a secret never goes in it: keep it in the app's environment on the VM (or SSM), and out of the repository" % key)
            _die_line(no, "the key is named like a secret. cbase.conf is committed and public, so a secret never goes in it")
    what: Optional[str] = None
    if "-----BEGIN" in text or "-----END" in text:
        what = "a PEM block (a key or a certificate's private part)"
    elif cli.re(r"(AKIA|ASIA)[A-Z0-9]{16}", text):
        what = "an AWS access key id"
    elif _run_matches(text, 0, 24, True):
        what = "a long random-looking string (an access key, a token)"
    elif _run_matches(text, 1, 32, False):
        what = "a long hex string (a token, a key)"
    elif _run_matches(text, 2, 40, False):
        what = "a long random-looking string (an access key, a token)"
    if what is None:
        return
    if _printable_key(key):
        _die_line(no, "the value of %s looks like a secret: %s. cbase.conf is committed and public, so a secret never goes in it" % (key, what))
    _die_line(no, "this line looks like a secret: %s. cbase.conf is committed and public, so a secret never goes in it" % what)


# ---- the load ------------------------------------------------------------------------------------

_ACM_RE = (r"^arn:aws:acm:([a-z0-9-]+):[0-9]{12}:certificate/"
           r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
_OBJ_KEY_RE = r"^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*\Z"
_BUCKET_RE = r"^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]\Z"


def load(file: str) -> None:
    """Reads FILE, checks it, and resolves the defaults into conf. Exits on error."""
    fresh = Conf()
    conf.__dict__.update(fresh.__dict__)
    del _seen[:]
    conf.file = file
    if not cli.is_file(file):
        _die_file("does not exist (an app's settings are cbase.conf at its repository's root, or -f FILE)")
    if not os.access(file, os.R_OK):
        _die_file("cannot be read")
    data = cli.read_file(file)
    if data is None:
        _die_file("cannot be read")
        return
    if "\0" in data:
        _die_file("holds a NUL byte")

    # The lines: split on '\n'; a last piece with no newline counts if it is not empty; one '\r' at the
    # end of each line is not part of it.
    lines = data.split("\n")
    if lines[-1] == "":
        lines.pop()
    lines = [ln[:-1] if ln.endswith("\r") else ln for ln in lines]

    # Every line is held to the secret scan first, before anything is parsed or printed.
    for i, ln in enumerate(lines):
        _secret_scan(i + 1, ln)

    vm_set = ""
    static_set = ""
    memory_set = False
    dir_set = ""
    memory = 0

    for i, line in enumerate(lines):
        no = i + 1
        # a blank or comment line is skipped
        first = line.lstrip(" \t")
        if not first or first[0] == "#":
            continue
        eq = line.find("=")
        if eq < 0:
            _die_line(no, "is not KEY = value (no '=')")
        key = cli.trim(line[:eq])
        value = cli.trim(line[eq + 1:])
        if not cli.re(r"^[A-Z][A-Z0-9_]*\Z", key):
            _die_line(no, "is not KEY = value (a key is upper-case letters, digits and _, and starts the line)")
        if key not in CONF_KEYS.split(" "):
            _die_line(no, "%s is not a cbase.conf key (the keys: %s)" % (key, CONF_KEYS))
        if "#" in value:
            _die_line(no, "has a '#' in its value: a comment goes on a line of its own")
        if '"' in value or "'" in value:
            _die_line(no, "has a quote in its value: values are never quoted (everything after the '=' is the value)")
        if not value:
            _die_line(no, "%s has no value (leave the line out for the default)" % key)
        is_route = key == "CBASE_ROUTE" or key == "CBASE_API_ROUTE"
        if not is_route:
            at = _conf_at(key)
            if at:
                _die_line(no, "sets %s again (line %d set it)" % (key, at))
        _seen.append((key, no))

        if key == "CBASE_APP":
            if not name_ok(value) or value == "none":
                _die_line(no, "CBASE_APP is a name: a lower-case letter, then lower-case letters, digits and '-', at most 32 characters, not ending in '-' (and not none)")
            conf.app = value
        elif key == "CBASE_VM" or key == "CBASE_STATIC":
            if not (value == "none" or name_ok(value)):
                _die_line(no, "%s is a group name (a lower-case letter, then lower-case letters, digits and '-', at most 32 characters) or none" % key)
            if key == "CBASE_VM":
                vm_set = value
            else:
                static_set = value
        elif key == "CBASE_VM_STACK":
            if not cli.re(r"^[A-Za-z][A-Za-z0-9-]{0,127}\Z", value):
                _die_line(no, "CBASE_VM_STACK is a CloudFormation stack's name (a letter, then letters, digits and -)")
            conf.vm_stack = value
        elif key == "CBASE_VM_ORIGIN":
            if not _host_ok(value):
                _die_line(no, "CBASE_VM_ORIGIN is a lower-case hostname (vm-origin.example.com)")
            conf.vm_origin = value
        elif key == "CBASE_VM_PORT":
            port = cli.parse_int(value, 0, 99999)
            if not cli.re(r"^[0-9]{4,5}\Z", value) or port is None or port < 1024 or port > 65535:
                _die_line(no, "CBASE_VM_PORT is a port, 1024 to 65535")
            assert port is not None
            if 8700 <= port <= 8799:
                _die_line(no, "CBASE_VM_PORT is a grandfathered member's own port, outside the 8700-8799 cbase claims")
            conf.vm_port = str(port)
        elif key == "CBASE_VM_DATA":
            slashed = value + "/"
            shape = cli.re(r"^(/[A-Za-z0-9_][A-Za-z0-9._-]*)+\Z", value)
            if not shape or "/../" in slashed or slashed.startswith("/srv/cbase/"):
                _die_line(no, "CBASE_VM_DATA is an absolute directory on the VM (/var/lib/app), not under /srv/cbase/")
            conf.vm_data = value
        elif key == "CBASE_VM_REPLACES":
            if not cli.re(r"^[A-Za-z0-9_.@-]+\.service\Z", value):
                _die_line(no, "CBASE_VM_REPLACES is a systemd unit's name (name.service)")
            conf.vm_replaces = value
        elif key == "CBASE_BACKUP_KEY":
            if not cli.re(_OBJ_KEY_RE, value):
                _die_line(no, "CBASE_BACKUP_KEY is an object key (cbase/latest.sqlite): letters, digits and . _ - /, no leading /")
            conf.backup_key = value
        elif key == "CBASE_STATIC_BUCKET":
            if not cli.re(_BUCKET_RE, value):
                _die_line(no, "CBASE_STATIC_BUCKET is an S3 bucket's name")
            conf.static_bucket = value
        elif key == "CBASE_STATIC_PREFIX":
            if not cli.re(_OBJ_KEY_RE, value):
                _die_line(no, "CBASE_STATIC_PREFIX is a folder in the bucket (site, sites/one): letters, digits and . _ - /, no leading or trailing /")
            conf.static_prefix = value
        elif key == "CBASE_VM_SETTINGS":
            if not cli.re(r"^(/[A-Za-z0-9_.-]+)+/\Z", value):
                _die_line(no, "CBASE_VM_SETTINGS is an SSM path, starting and ending with / (/app/env/)")
            conf.vm_settings = value
        elif key == "CBASE_VM_ARTIFACTS":
            if not cli.re(r"^s3://[a-z0-9][a-z0-9.-]{1,61}[a-z0-9](/[A-Za-z0-9_.-]+)*/\Z", value):
                _die_line(no, "CBASE_VM_ARTIFACTS is s3://<bucket>/<prefix>/, ending with /")
            conf.vm_artifacts = value
        elif key == "CBASE_BACKUP_BUCKET":
            if not cli.re(_BUCKET_RE, value):
                _die_line(no, "CBASE_BACKUP_BUCKET is an S3 bucket's name")
            conf.backup_bucket = value
        elif key == "CBASE_MEMORY":
            if not cli.re(r"^[0-9]{1,6}[MG]\Z", value):
                _die_line(no, "CBASE_MEMORY is <n>M or <n>G (256M, 1G)")
            n = cli.parse_int(value[:-1], 0, 999999)
            memory = n if n is not None else 0
            if value[-1] == "G":
                memory *= 1024
            if memory < 16 or memory > 65536:
                _die_line(no, "CBASE_MEMORY is from 16M to 64G")
            memory_set = True
        elif key == "CBASE_ROUTE":
            _conf_route(no, value, False)
        elif key == "CBASE_API_ROUTE":
            _conf_route(no, value, True)
        elif key == "CBASE_DOMAIN":
            names = cli.split_on(value, " \t")
            for g, nm in enumerate(names):
                if not _host_ok(nm):
                    _die_line(no, "CBASE_DOMAIN is lower-case hostnames, blank-separated (example.com www.example.com)")
                for k in range(g):
                    if names[k] == nm:
                        _die_line(no, "CBASE_DOMAIN names %s twice" % nm)
            conf.domains = names
            conf.domain = names[0]
        elif key == "CBASE_API_HOST":
            if not _host_ok(value):
                _die_line(no, "CBASE_API_HOST is one lower-case hostname (api.example.com)")
            conf.api_host = value
        elif key == "CBASE_CERT_ARN" or key == "CBASE_API_CERT_ARN":
            if not cli.re(_ACM_RE, value):
                _die_line(no, "%s is an ACM certificate's ARN: arn:aws:acm:us-east-1:<account>:certificate/<uuid>" % key)
            # the region is the second colon-field after "arn:aws:acm:"
            region = value[len("arn:aws:acm:"):]
            region = region[:region.index(":")]
            if region != "us-east-1":
                _die_line(no, "%s is in %s, and CloudFront takes a certificate from us-east-1 alone" % (key, region))
            if key == "CBASE_CERT_ARN":
                conf.cert_arn = value
            else:
                conf.api_cert_arn = value
        elif key == "CBASE_ALIASES":
            if value != "on" and value != "off":
                _die_line(no, "CBASE_ALIASES is on or off")
            conf.aliases = value
        elif key == "CBASE_ZONE":
            if not cli.re(r"^Z[A-Z0-9]{1,31}\Z", value):
                _die_line(no, "CBASE_ZONE is a Route 53 hosted zone id (Z and upper-case letters and digits)")
            conf.zone = value
        elif key == "CBASE_STATIC_DIR":
            if value.endswith("/"):
                value = value[:-1]
            slashed = "/" + value + "/"
            if not cli.re(_OBJ_KEY_RE, value) or "/../" in slashed:
                _die_line(no, "CBASE_STATIC_DIR is a directory inside the repository, relative to its root (build, ui/out): letters, digits and . _ - /, no leading '/', no '..'")
            dir_set = value
        elif key == "CBASE_HASHED":
            globs = cli.split_on(value, " \t")
            for gl in globs:
                if not cli.re(r"^[A-Za-z0-9._/*?-]+\Z", gl) or gl[0] == "/":
                    _die_line(no, "CBASE_HASHED is blank-separated globs of letters, digits and . _ - / * ? (assets/*), each a path in the static dir with no leading '/'")
            conf.hashed = globs
        elif key == "CBASE_SHELL":
            if not cli.re(r"^/[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*\.html\Z", value):
                _die_line(no, "CBASE_SHELL is a page of the build, an absolute path ending in .html (/_shell.html): letters, digits and . _ - /, each part starting with a letter, a digit, _ or -")
            conf.shell = value

    if not _conf_at("CBASE_APP"):
        _die_file("has no CBASE_APP line (the app's name: CBASE_APP = myapp)")
    conf.vm = vm_set if vm_set else conf.app
    conf.stat = static_set if static_set else conf.app
    if conf.vm == "none" and conf.stat == "none":
        _die_line(_conf_at("CBASE_VM CBASE_STATIC"), "CBASE_VM and CBASE_STATIC are both none: an app has a VM, static files, or both")
    vmkeys = ("CBASE_VM_STACK CBASE_VM_ORIGIN CBASE_VM_PORT CBASE_VM_DATA CBASE_VM_REPLACES CBASE_BACKUP_KEY "
              "CBASE_VM_SETTINGS CBASE_VM_ARTIFACTS CBASE_BACKUP_BUCKET")
    if (conf.vm_port or conf.vm_data) and (not conf.vm_port or not conf.vm_data):
        _die_line(_conf_at(vmkeys), "CBASE_VM_PORT and CBASE_VM_DATA go together: a grandfathered member's port and its data directory")
    if conf.vm_replaces and not conf.vm_port:
        _die_line(_conf_at(vmkeys), "CBASE_VM_REPLACES is for a grandfathered member (CBASE_VM_PORT and CBASE_VM_DATA): the unit that served it before")
    if conf.vm == "none":
        _conf_refuses(vmkeys, "the CBASE_VM_* and CBASE_BACKUP_BUCKET keys are for an app with a VM, and CBASE_VM is none")
        _conf_refuses("CBASE_MEMORY", "CBASE_MEMORY is for an app with a VM, and CBASE_VM is none")
        _conf_refuses("CBASE_API_HOST", "CBASE_API_HOST sends every request to the VM, and CBASE_VM is none")
    else:
        conf.memory = str(memory) if memory_set else ""
        conf.memory_estimated = not _conf_at("CBASE_MEMORY")
    for r in conf.routes:
        if r.origin == "static" and conf.stat == "none":
            _die_line(r.line, "routes to static, and CBASE_STATIC is none")
        if r.origin != "static" and conf.vm == "none":
            _die_line(r.line, "routes to the VM (%s), and CBASE_VM is none" % r.origin)
    _conf_requires("CBASE_API_HOST", "CBASE_DOMAIN", "CBASE_API_HOST is a second hostname, and there is no CBASE_DOMAIN (the first)")
    _conf_requires("CBASE_API_ROUTE", "CBASE_API_HOST", "CBASE_API_ROUTE is a route of CBASE_API_HOST's, and there is none")
    if conf.api_host:
        for d in conf.domains:
            if d == conf.api_host:
                _die_line(_conf_at("CBASE_API_HOST"), "CBASE_API_HOST %s is one of CBASE_DOMAIN's hostnames: a hostname is on one distribution" % d)
    _conf_requires("CBASE_CERT_ARN", "CBASE_DOMAIN", "CBASE_CERT_ARN is for CBASE_DOMAIN, and there is none (without a domain the app is at its cloudfront.net address)")
    _conf_requires("CBASE_API_CERT_ARN", "CBASE_API_HOST", "CBASE_API_CERT_ARN is for CBASE_API_HOST, and there is none")
    _conf_requires("CBASE_ALIASES", "CBASE_DOMAIN", "CBASE_ALIASES is for CBASE_DOMAIN's hostnames, and there is none")
    _conf_requires("CBASE_ZONE", "CBASE_DOMAIN", "CBASE_ZONE is where CBASE_DOMAIN's record goes, and there is no CBASE_DOMAIN")
    if conf.stat == "none":
        _conf_refuses("CBASE_STATIC_DIR", "CBASE_STATIC_DIR is the static files' directory, and CBASE_STATIC is none")
        _conf_refuses("CBASE_HASHED", "CBASE_HASHED names static files, and CBASE_STATIC is none")
        _conf_refuses("CBASE_SHELL", "CBASE_SHELL is a page of the static files, and CBASE_STATIC is none")
        _conf_refuses("CBASE_STATIC_BUCKET CBASE_STATIC_PREFIX", "CBASE_STATIC_BUCKET and CBASE_STATIC_PREFIX are for static files, and CBASE_STATIC is none")
        conf.static_dir = ""
        conf.static_dir_default = False
    elif dir_set:
        conf.static_dir = dir_set
        conf.static_dir_default = False
    else:
        conf.static_dir = "build" if conf.vm == "none" else "ui/out"
        conf.static_dir_default = True
    # No route written: the defaults for the app's shape.
    if not conf.routes and conf.vm != "none" and conf.stat != "none":
        conf.routes.append(Route("/api/*", "vm", "nocache", "ALL", 0))
    has_default = any(r.pattern == "*" for r in conf.routes)
    if not has_default:
        if conf.stat != "none":
            conf.routes.append(Route("*", "static", "cache", "GET,HEAD", 0))
        else:
            conf.routes.append(Route("*", "vm", "nocache", "ALL", 0))
    else:
        # CloudFront's default behaviour goes last, wherever it was written.
        ordered = [r for r in conf.routes if r.pattern != "*"]
        default = [r for r in conf.routes if r.pattern == "*"][-1]
        ordered.append(default)
        conf.routes = ordered


# ---- the printed form ----------------------------------------------------------------------------


def _row(label: str, val: str) -> None:
    cli.out("%-10s %s\n" % (label, val))


def print_conf() -> None:
    """The settings as read, defaults filled in, to stdout (config check, doctor)."""
    c = conf
    _row("app", c.app)
    if c.vm == "none":
        _row("vm", "none")
    elif c.memory_estimated:
        _row("vm", "%s (%s: memory not declared, counted as %dM)" % (c.vm, c.app, MEMORY_ESTIMATE))
    else:
        _row("vm", "%s (memory %s MiB)" % (c.vm, c.memory))
    if c.vm_stack:
        _row("vm-stack", "%s (adopted: found by its stack, no group-init)" % c.vm_stack)
    if c.vm_origin:
        _row("vm-origin", "%s (IPv6)" % c.vm_origin)
    if c.vm_port:
        _row("legacy", "port %s, data %s (grandfathered: kept as it is)" % (c.vm_port, c.vm_data))
    if c.vm_settings:
        _row("settings", "SSM %s" % c.vm_settings)
    if c.vm_artifacts:
        _row("artifacts", c.vm_artifacts)
    if c.vm_replaces:
        _row("replaces", "%s (stopped and disabled by the first deploy)" % c.vm_replaces)
    if c.backup_bucket or c.backup_key:
        default_key = "backups/%s/cbase.sqlite" % c.app
        _row("backups", "s3://%s/%s" % (c.backup_bucket if c.backup_bucket else "(the state bucket)",
                                       c.backup_key if c.backup_key else default_key))
    if c.static_bucket:
        _row("static", "s3://%s/%s/ (adopted bucket)" % (c.static_bucket, c.static_prefix if c.static_prefix else c.app))
    else:
        _row("static", c.stat)
    if not c.static_bucket and c.static_prefix:
        _row("folder", "%s/" % c.static_prefix)
    if c.static_dir:
        if c.static_dir_default:
            _row("files", "%s/ (the default)" % c.static_dir)
        else:
            _row("files", "%s/" % c.static_dir)
    if c.hashed:
        _row("hashed", " ".join(c.hashed))
    if c.shell:
        _row("shell", c.shell)
    if c.domain:
        _row("domain", " ".join(c.domains))
    if c.api_host:
        _row("api-host", c.api_host)
    if c.cert_arn:
        _row("cert", c.cert_arn)
    elif c.domain:
        _row("cert", "(found or requested by deploy)")
    else:
        _row("address", "(the distribution's cloudfront.net name)")
    if c.api_cert_arn:
        _row("api-cert", c.api_cert_arn)
    if c.zone:
        _row("zone", c.zone)
    if c.aliases != "on":
        _row("aliases", "off (the distributions carry no hostname: a staged attach)")
    for r in c.routes:
        _row("route", "%s %s %s %s" % (r.pattern, r.origin, r.cache, r.methods))
    for r in c.api_routes:
        _row("api-route", "%s %s %s %s" % (r.pattern, r.origin, r.cache, r.methods))


def mem() -> str:
    """conf.sh's conf_mem: the app's mem field in a state file, the declared MiB or "~128" when it is
    the estimate; "" for an app with no VM."""
    if conf.memory_estimated:
        return "~%d" % MEMORY_ESTIMATE
    return conf.memory
