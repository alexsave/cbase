# static.py - an app's static files into its static group's bucket (the port of static.c; static.sh's header
# says what each rule is for). The per-file headers come from tools/cbase.d/static_rules.tsv, read at run
# time. The scan, the upload order, and the prune that is held to the app's folder.
from __future__ import annotations

import os
import stat
import sys
from typing import List, Optional, Tuple

import aws
import cli
import conf as confmod

conf = confmod.conf

CACHE_IMMUTABLE = "public, max-age=31536000, immutable"
CACHE_NOCACHE = "no-cache"

# ---- fnmatch -------------------------------------------------------------------------------------
# libc's fnmatch(3), as the C called it (flags 0, or FNM_CASEFOLD): `*` crosses '/', a backslash quotes the
# next character, `[!x]` and `[^x]` negate, ranges and [:class:]es work, an unterminated `[` is a literal.
# Python's fnmatch differs (`[^x]` is literal, no backslash), so the match is written out; it was checked
# against libc's on random patterns. Case folding is ASCII only (the C locale).

_CLASSES = {
    "alpha": lambda c: ("a" <= c <= "z") or ("A" <= c <= "Z"),
    "digit": lambda c: "0" <= c <= "9",
    "alnum": lambda c: ("a" <= c <= "z") or ("A" <= c <= "Z") or ("0" <= c <= "9"),
    "upper": lambda c: "A" <= c <= "Z",
    "lower": lambda c: "a" <= c <= "z",
    "space": lambda c: c in " \t\n\v\f\r",
    "blank": lambda c: c in " \t",
    "punct": lambda c: "!" <= c <= "~" and not c.isalnum(),
    "print": lambda c: " " <= c <= "~",
    "graph": lambda c: "!" <= c <= "~",
    "cntrl": lambda c: c < " " or c == "\x7f",
    "xdigit": lambda c: c in "0123456789abcdefABCDEF",
}


def _lo(c: str) -> str:
    return chr(ord(c) + 32) if "A" <= c <= "Z" else c


def _up(c: str) -> str:
    return chr(ord(c) - 32) if "a" <= c <= "z" else c


def _bracket(p: str, pi: int, c: str, fold: bool) -> Tuple[int, bool]:
    """The bracket expression starting at P[PI] == '[' against the character C: (the index after it, whether
    C matched); (-1, False) when it is not terminated (the '[' is then an ordinary character)."""
    i = pi + 1
    n = len(p)
    neg = False
    if i < n and p[i] in "!^":
        neg = True
        i += 1
    cands = [_lo(c)] if fold else [c]   # FNM_CASEFOLD lowers the tested character and the pattern's
    matched = False
    first = True
    while True:
        if i >= n:
            return -1, False
        ch = p[i]
        if ch == "]" and not first:
            i += 1
            break
        first = False
        if ch == "[" and i + 1 < n and p[i + 1] == ":":
            end = p.find(":]", i + 2)
            if end >= 0:
                fn = _CLASSES.get(p[i + 2:end])
                if fn is not None and any(fn(x) for x in cands):
                    matched = True
                i = end + 2
                continue
        lo_c = _lo(ch) if fold else ch   # a backslash is an ordinary character in a bracket (macOS libc)
        i += 1
        if i + 1 < n and p[i] == "-" and p[i + 1] != "]":
            hi_c = _lo(p[i + 1]) if fold else p[i + 1]
            i += 2
            if any(lo_c <= x <= hi_c for x in cands):
                matched = True
        elif any(x == lo_c for x in cands):
            matched = True
    return i, matched != neg


def _single(p: str, pi: int, c: str, fold: bool) -> int:
    """P[PI] (not a '*') against the one character C: the next pattern index, or -1."""
    ch = p[pi]
    if ch == "?":
        return pi + 1
    if ch == "[":
        nxt, ok = _bracket(p, pi, c, fold)
        if nxt >= 0:
            return nxt if ok else -1
        # an unterminated '[': an ordinary character
    elif ch == "\\" and pi + 1 < len(p):
        pi += 1
        ch = p[pi]
    if ch == c or (fold and _lo(ch) == _lo(c)):
        return pi + 1
    return -1


def fnmatch(pat: str, s: str, fold: bool = False) -> bool:
    """fnmatch(pat, s, 0) or, with FOLD, FNM_CASEFOLD."""
    pi = 0
    si = 0
    star = -1
    mark = 0
    while si < len(s):
        if pi < len(pat):
            if pat[pi] == "*":
                star = pi
                mark = si
                pi += 1
                continue
            r = _single(pat, pi, s[si], fold)
            if r >= 0:
                pi = r
                si += 1
                continue
        if star >= 0:
            mark += 1
            si = mark
            pi = star + 1
            continue
        return False
    while pi < len(pat) and pat[pi] == "*":
        pi += 1
    return pi == len(pat)


# ---- small helpers -------------------------------------------------------------------------------


def _make_dir(d: str) -> None:
    """DIR made, or already there."""
    try:
        os.mkdir(d, 0o777)
    except FileExistsError:
        pass
    except (OSError, ValueError):
        cli.fail(cli.EXIT_FAIL, "cannot make %s" % d)


def _mkdir_p(path: str) -> None:
    for i in range(1, len(path)):
        if path[i] == "/":
            _make_dir(path[:i])
    _make_dir(path)


def _copy_file(src: str, dst: str) -> None:
    """SRC's bytes and mode into DST (cp -p's content, for a file a link cannot reach)."""
    try:
        with open(src, "rb") as fi:
            mode = os.fstat(fi.fileno()).st_mode & 0o777
            data = fi.read()
    except OSError:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % src)
        return
    try:
        fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
        with os.fdopen(fd, "wb") as fo:
            fo.write(data)
    except OSError:
        cli.fail(cli.EXIT_FAIL, "cannot write %s" % dst)


# ---- the scan ------------------------------------------------------------------------------------


def _walk(root: str, rel: str, out: List[str]) -> None:
    """Every regular file under ROOT/REL, named relative to the static dir (find -type f: a symbolic link
    is neither followed nor named). A directory that cannot be read stops the tool, as find's failure did."""
    dirpath = root + "/" + rel if rel else root
    try:
        names = os.listdir(dirpath)
    except OSError:
        cli.fail(cli.EXIT_FAIL, "could not list every file under %s; nothing was uploaded" % root)
        return
    for name in names:
        child = rel + "/" + name if rel else name
        full = dirpath + "/" + name
        try:
            st = os.lstat(full)
        except OSError:
            cli.fail(cli.EXIT_FAIL, "could not list every file under %s; nothing was uploaded" % root)
            return
        if stat.S_ISDIR(st.st_mode):
            _walk(root, child, out)
        elif stat.S_ISREG(st.st_mode):
            out.append(child)


def _hidden(f: str) -> bool:
    """find's dot rule as the shell wrote it, on "/" + the path: a part that starts with '.' is left out,
    .well-known/ excepted (static.sh, WHAT IS UPLOADED)."""
    p = "/" + f
    return "/.well-known/" not in p and "/." in p


def scan(dir: str) -> List[str]:
    """DIR's files to upload, relative paths, sorted by bytes. Stops the tool on a directory that is not a
    build (no index.html), a file name with a control character, or a listing that fails; nothing is
    changed by a scan."""
    if not cli.is_dir(dir):
        cli.fail(cli.EXIT_FAIL, "the static dir %s does not exist: build the app first, or set CBASE_STATIC_DIR in cbase.conf (the directory with index.html)" % dir)
    if not cli.is_file(dir + "/index.html"):
        cli.fail(cli.EXIT_FAIL, "%s has no index.html, so it is not a built site: nothing was uploaded or deleted (CBASE_STATIC_DIR names the directory with index.html)" % dir)
    allf: List[str] = []
    _walk(dir, "", allf)
    files: List[str] = []
    for f in allf:
        if _hidden(f):
            continue
        for c in f.encode("utf-8", "surrogateescape"):
            if c < 32 or c == 127:   # iscntrl
                cli.fail(cli.EXIT_FAIL, "%s holds a file whose name has a control character (a tab, a newline): rename it; nothing was uploaded" % dir)
        files.append(f)
    return cli.sort_bytes(files)


# ---- the headers ---------------------------------------------------------------------------------


def _hashed(path: str) -> bool:
    """PATH is under one of CBASE_HASHED's globs (a case-sensitive match, as the shell's)."""
    for g in conf.hashed:
        if fnmatch(g, path):
            return True
    return False


_rules: List[Tuple[str, str]] = []


def _rules_load() -> None:
    """The table, parsed as static_rules_load reads it: a line is a glob and a Content-Type, TAB-separated;
    blank and '#' lines are skipped; the last rule must be the fallback "*". The message names the file by
    its repository path (the shell names it by its install path: the only difference)."""
    if _rules:
        return
    f = "tools/cbase.d/static_rules.tsv"
    text = cli.read_file(cli.tools_dir() + "/cbase.d/static_rules.tsv")
    if text is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % f)
        return
    for line in text.split("\n"):
        if line and line[0] != "#":
            g = line
            t: Optional[str] = None
            extra: Optional[str] = None
            tab = line.find("\t")
            if tab >= 0:
                g = line[:tab]
                t = line[tab + 1:]
                tab2 = t.find("\t")
                if tab2 >= 0:
                    extra = t[tab2 + 1:]
                    t = t[:tab2]
            if not t or extra:
                cli.err("cbase: %s: the rule for '%s' is not two TAB-separated fields\n" % (f, g))
                sys.exit(cli.EXIT_FAIL)
            _rules.append((g, t))
    if not _rules or _rules[-1][0] != "*":
        cli.err("cbase: %s does not end with the fallback rule \"*\"\n" % f)
        sys.exit(cli.EXIT_FAIL)


def _classify(path: str) -> Tuple[str, str]:
    """The file's Content-Type (the first rule whose glob matches, ignoring case) and its Cache-Control:
    html is always no-cache, a content-hashed file is a year, the rest are no-cache."""
    _rules_load()
    typ = ""
    for g, t in _rules:
        if fnmatch(g, path, True):
            typ = t
            break
    cache = CACHE_NOCACHE
    if not typ.startswith("text/html") and _hashed(path):
        cache = CACHE_IMMUTABLE
    return typ, cache


# ---- the plan ------------------------------------------------------------------------------------


class Group:
    """One pair of headers (and phase): a scratch tree of links to its files, so one `aws s3 cp --recursive`
    carries exactly its pair (static.sh static_plan). PHASE: 1 content-hashed, 2 other than html, 3 html."""

    def __init__(self, phase: int, typ: str, cache: str, tree: str) -> None:
        self.phase = phase
        self.type = typ
        self.cache = cache
        self.count = 0
        self.tree = tree


class Plan:
    """The static site's upload plan: its groups, and the scratch tree's directory (TMP/static-tree)."""

    def __init__(self, root: str) -> None:
        self.groups: List[Group] = []
        self.root = root


def plan_make(dir: str, files: List[str], tmp: str) -> Plan:
    """The upload groups for FILES under DIR, their links made in TMP."""
    p = Plan(tmp + "/" + "static-tree")
    _mkdir_p(p.root)
    for f in files:
        typ, cache = _classify(f)
        phase = 1 if cache == CACHE_IMMUTABLE else (3 if typ.startswith("text/html") else 2)
        g: Optional[Group] = None
        for k in p.groups:
            if k.phase == phase and k.type == typ and k.cache == cache:
                g = k
                break
        if g is None:
            g = Group(phase, typ, cache, "%s/g%d" % (p.root, len(p.groups)))
            p.groups.append(g)
        g.count += 1
        # A link (ln) in the group's tree, its directories made first; a copy where no link can be made.
        src = dir + "/" + f
        dst = g.tree + "/" + f
        _mkdir_p(dst[:dst.rindex("/")])
        try:
            os.link(src, dst)
        except OSError:
            _copy_file(src, dst)
    return p


def print_plan(p: Plan) -> None:
    """The groups in upload order, one line each."""
    for phase in (1, 2, 3):
        for g in p.groups:
            if g.phase != phase:
                continue
            cli.out("  %4d file%s  %s  %s\n" % (g.count, " " if g.count == 1 else "s", g.type, g.cache))


def upload(p: Plan, bucket: str, region: str, app: str) -> None:
    """Each group, phase by phase, into s3://BUCKET/APP/ (html last)."""
    n = 0
    dest = "s3://%s/%s/" % (bucket, app)
    for phase in (1, 2, 3):
        for g in p.groups:
            if g.phase != phase:
                continue
            rc, _o = aws.call("s3", "cp", g.tree, dest, "--recursive", "--only-show-errors", "--region", region,
                              "--content-type", g.type, "--cache-control", g.cache)
            if rc != 0:
                aws.fail("uploading to %s" % dest)
            n += g.count
    cli.out("uploaded %d file%s to %s (html last)\n" % (n, "" if n == 1 else "s", dest))


def prune(bucket: str, region: str, app: str, files: List[str], dry: bool) -> None:
    """Deletes the keys under APP/ that FILES does not name. A key outside APP/ is never deleted, whatever
    the listing says."""
    prefix = app + "/"
    rc, out = aws.call("s3api", "list-objects-v2", "--bucket", bucket, "--prefix", prefix, "--region", region,
                       "--query", "Contents[].Key", "--output", "text")
    if rc != 0:
        aws.fail("listing s3://%s/%s" % (bucket, prefix))
    have = set(files)
    gone: List[str] = []   # the keys to delete: each held to the folder BEFORE it joins (and a batch)
    n = 0
    # The listing: a page of keys a line, tab-separated (the CLI's text output; no key holds a tab).
    for line in cli.text_of(out).split("\n"):
        for k in line.split("\t"):
            if k and k != "None":
                # THE FOLDER GUARD: whatever the listing holds, only a key under "<app>/" is ever deleted.
                pl = len(prefix)
                if not k.startswith(prefix) or len(k) == pl:
                    cli.err("cbase: prune: s3://%s/%s is outside %s/, and is left alone\n" % (bucket, k, app))
                elif k[pl:] not in have:
                    if dry:
                        cli.out("would prune s3://%s/%s\n" % (bucket, k))
                    else:
                        gone.append(k)
                    n += 1
    if not dry:
        aws.s3_delete_keys(bucket, region, gone)   # batched; prints one "pruned" line a key
    cli.out("%s %d file%s the build no longer has, under s3://%s/%s/ alone\n"
            % ("would prune" if dry else "pruned", n, "" if n == 1 else "s", bucket, app))


def bucket(static_bucket: str, group: str) -> Tuple[str, str]:
    """The static group's bucket for CONF_STATIC (or CBASE_STATIC_BUCKET, when set), and its region; the
    caller takes the app's folder from CBASE_STATIC_PREFIX or the app's name. Stops the tool when a call
    fails."""
    if static_bucket:
        b = static_bucket
    else:
        gid = aws.group_find("static", group)
        if gid is None:
            sys.exit(cli.EXIT_FAIL)
        b = gid
    rc, out = aws.call("s3api", "get-bucket-location", "--bucket", b, "--query", "LocationConstraint",
                       "--output", "text")
    if rc != 0:
        aws.fail("reading s3://%s's region" % b)
    loc = cli.text_of(out)
    if loc == "None" or loc == "":
        region = "us-east-1"
    elif loc == "EU":
        region = "eu-west-1"
    else:
        region = loc
    return b, region


def policy_ok(bucket: str, account: str) -> bool:
    """True when BUCKET's policy lets ACCOUNT's distributions read it. Otherwise it prints the statement to
    add (stderr) and returns False; the caller decides whether that stops the deploy (a dry run goes on).
    Stops the tool on a failed read."""
    rc, out = aws.call("s3api", "get-bucket-policy", "--bucket", bucket, "--query", "Policy", "--output", "text")
    if rc == 0:
        pol = cli.text_of(out)
        if "cloudfront.amazonaws.com" in pol and "aws:SourceAccount" in pol and account in pol:
            cli.out("static bucket s3://%s: its policy lets the account's distributions read it\n" % bucket)
            return True
    elif aws.code() != "NoSuchBucketPolicy":
        aws.fail("reading s3://%s's policy" % bucket)
    cli.err("cbase: s3://%(b)s's policy does not let this account's distributions read it, so the app's new distribution would answer 403.\n"
            "  Add this statement to the policy's Statement list, keeping every statement it has (S3 console: the bucket, Permissions, Bucket policy, Edit):\n"
            "    {\"Sid\": \"TheAccountsDistributionsRead\", \"Effect\": \"Allow\", \"Principal\": {\"Service\": \"cloudfront.amazonaws.com\"},\n"
            "     \"Action\": [\"s3:GetObject\", \"s3:ListBucket\"], \"Resource\": [\"arn:aws:s3:::%(b)s\", \"arn:aws:s3:::%(b)s/*\"],\n"
            "     \"Condition\": {\"StringEquals\": {\"aws:SourceAccount\": \"%(a)s\"}}}\n"
            "  It lets any CloudFront distribution of account %(a)s read the bucket (a cbase static group's own statement), and removes nothing.\n"
            % {"b": bucket, "a": account})
    return False
