# aws.py - every AWS call the deploy tool makes (the port of aws.c): each one an AWS CLI call with its own
# credential chain, its output read with --query and --output text, its failure classified by the error
# code the CLI prints, and what of its text reaches the person masked. The last call's stderr is kept in
# this module (aws_err in the C), emptied as each attempt starts.
#
# The calls return (status, stdout text) where the C appended to a buffer: a throttled attempt's stdout
# stays in front of the next attempt's, as the C buffer kept it.
from __future__ import annotations

import fnmatch
import os
import re
import sys
from typing import Callable, List, Optional, Sequence, Tuple

import cli
import conf

account = ""   # the twelve-digit account the credentials are in, set by init

_err = ""            # the stderr of the last call (a success leaves it empty)
_mask_values: List[str] = []   # mask_add's values

_ATTEMPTS = 5


def mask_add(value: str) -> None:
    """A value this run holds that a line may name (the origin secret, of letters, digits, _ and -). A line
    holding it is masked by mask."""
    _mask_values.append(value)


def _prec(s: str, n: int) -> str:
    """printf's %.Ns: at most N bytes of S."""
    b = s.encode("utf-8", "surrogateescape")
    if len(b) <= n:
        return s
    return b[:n].decode("utf-8", "surrogateescape")


def init() -> None:
    """The CLI found, and the account asked of STS. Stops the tool on failure."""
    global account
    if not cli.on_path("aws"):
        cli.fail(cli.EXIT_FAIL, "the AWS CLI (aws) is not on PATH: install AWS CLI v2 (https://docs.aws.amazon.com/cli/)")
    os.environ["AWS_PAGER"] = ""
    rc, out = call("sts", "get-caller-identity", "--query", "Account", "--output", "text")
    if rc != 0:
        fail("sts get-caller-identity")
    account = out.rstrip("\n")
    if not cli.re("^[0-9]{12}\\Z", account):
        cli.fail(cli.EXIT_FAIL, "STS named the account \"%s\", which is not twelve digits" % _prec(account, 40))


def _stagger_ms(name: str) -> int:
    """0-99, the name's cksum mod 100. Deterministic."""
    return cli.cksum(name) % 100


def _err_matches(pattern: str, icase: bool) -> bool:
    """Whether any line of the last call's stderr matches PATTERN (grep -q, line by line); ICASE as grep -i."""
    for line in _err.split("\n"):
        if cli.re_icase(pattern, line) if icase else cli.re(pattern, line):
            return True
    return False


def _throttled() -> bool:
    """The last failure was AWS's throttling (case-insensitively)."""
    return _err_matches("Throttling|TooManyRequests|Too Many Requests|RequestLimitExceeded|"
                        "(HTTP|status|code)[^0-9A-Za-z]{0,8}429([^0-9]|\\Z)|\\(429\\)", True)


def exec_(head: Optional[Sequence[str]], tail: Optional[Sequence[str]]) -> Tuple[int, str]:
    """The one core every call goes through: `aws --no-cli-pager`, then the words of HEAD, then the words
    of TAIL, run once per attempt with the retry on throttling (up to five attempts in all). Returns the
    status of the last attempt and the stdout of all attempts, joined; its stderr is held, read by code and
    fail."""
    global _err
    av = ["aws", "--no-cli-pager"] + list(head or []) + list(tail or [])
    outs = ""
    k = 1
    while True:
        _err = ""
        rc, o, e = cli.run(av)
        outs += o
        _err = e
        if rc == 0 or not (k < _ATTEMPTS and _throttled()):
            break
        ms = (1000 << (k - 1)) + _stagger_ms(conf.conf.app)
        cli.sleep_ms(ms)
        k += 1
    return rc, outs


def callv(head: Optional[Sequence[str]], *args: str) -> Tuple[int, str]:
    """exec_ with a HEAD, for the `cloudformation --region R` prefix."""
    return exec_(head, args)


def call(*args: str) -> Tuple[int, str]:
    """Runs `aws --no-cli-pager` with ARGS; returns (status of the last attempt, its stdout)."""
    return exec_(None, args)


def err_contains(text: str) -> bool:
    """Whether the last failed call's stderr holds TEXT (a plain substring)."""
    return text in _err


_CODE_RE = re.compile(r".*An error occurred \(([A-Za-z0-9.]*)\).*", re.A)


def code() -> str:
    """The error code of the last failed call ("NoSuchKey"), or ""."""
    for line in _err.split("\n"):
        m = _CODE_RE.search(line)
        if m:
            return m.group(1)[:255]
    return ""


_MASKED = "(a line naming an origin header or a parameter value, not shown)"


def mask(line: str) -> str:
    """LINE as the person may see it: a line naming an origin's headers or a parameter's value, or holding
    one of mask_add's values, is replaced by a sentence saying so."""
    if cli.re_icase("OriginCustomHeaders|HeaderValue|ParameterValue", line):
        return _MASKED
    for v in _mask_values:
        if v in line:
            return _MASKED
    return line


def show(lines: int) -> None:
    """The first LINES lines of the last failed call's stderr, masked and indented by two spaces, on stderr."""
    p = 0
    shown = 0
    n = len(_err)
    while p < n and shown < lines:
        nl = _err.find("\n", p)
        line = _err[p:] if nl < 0 else _err[p:nl]
        cli.err("  %s\n" % mask(line))
        shown += 1
        if nl < 0:
            break
        p = nl + 1


def _print_failure(what: str) -> None:
    c = code()
    if _err_matches("Unable to locate credentials|Token has expired|token.*expired|expired.*token|ExpiredToken|The SSO session|InvalidClientTokenId|refresh failed|no valid credential", False):
        cli.err("cbase: %s: no usable AWS credentials. Run aws login (or aws sso login), or set AWS_PROFILE.\n" % what)
    elif _err_matches("Unknown options: --if-", False):
        cli.err("cbase: %s: this AWS CLI is too old for S3 conditional writes (put-object --if-match): update AWS CLI v2.\n" % what)
    elif c in ("AccessDenied", "AccessDeniedException", "UnauthorizedOperation"):
        profile = os.environ.get("AWS_PROFILE")
        if profile:
            cli.err("cbase: %s: AWS refused it (AccessDenied) for profile %s. The deploy role needs the permission.\n" % (what, profile))
        else:
            cli.err("cbase: %s: AWS refused it (AccessDenied) for these credentials. The deploy role needs the permission.\n" % what)
    else:
        cli.err("cbase: %s failed:\n" % what)
        show(5)


def _region_get() -> str:
    r = os.environ.get("AWS_REGION")
    if r:
        return r
    r = os.environ.get("AWS_DEFAULT_REGION")
    if r:
        return r
    _rc, out, _e = cli.run(["aws", "configure", "get", "region"])   # stderr is not shown
    return out.rstrip("\n")


def region() -> str:
    """The region the CLI calls, or "the configured region" when none is set (for messages)."""
    r = _region_get()
    return r if r else "the configured region"


def region_required() -> str:
    """The region the CLI calls; stops the tool when none is set."""
    r = _region_get()
    if not cli.re("^[a-z]{2}(-[a-z]+)+-[0-9]\\Z", r):
        cli.fail(cli.EXIT_FAIL, "no AWS region is set: set AWS_REGION (us-east-1), or the profile's region (aws configure set region us-east-1)")
    return r


def group_find(kind: str, name: str) -> Optional[str]:
    """A group's resource, found by its tag: the instance id of a vm group, the bucket name of a static
    one. On a failure it prints the message and returns None without exiting (the caller decides whether
    to stop)."""
    tag = "cbase:vm-group" if kind == "vm" else "cbase:static-group"
    rtype = "ec2:instance" if kind == "vm" else "s3"
    flt = "Key=%s,Values=%s" % (tag, name)
    what = "finding %s group %s (tag:GetResources)" % (kind, name)
    rc, out = call("resourcegroupstaggingapi", "get-resources", "--tag-filters", flt,
                   "--resource-type-filters", rtype, "--query", "ResourceTagMappingList[].ResourceARN",
                   "--output", "text")
    if rc != 0:
        _print_failure(what)
        return None
    arns = cli.split_on(out, " \t\n")
    if len(arns) == 0:
        cli.err("cbase: there is no %s group named %s (nothing tagged %s=%s in %s). Create it with: ./cbase group-init %s %s\n"
                % (kind, name, tag, name, region(), kind, name))
        return None
    if len(arns) > 1:
        cli.err("cbase: %d resources are tagged %s=%s, and a %s group is one %s: %s\n"
                % (len(arns), tag, name, kind, "instance" if kind == "vm" else "bucket", " ".join(arns)))
        return None
    arn = arns[0]
    kind_arn = "%s:%s" % (kind, arn)
    if kind == "vm" and fnmatch.fnmatchcase(kind_arn, "vm:arn:aws*:ec2:*:instance/i-*"):
        return arn[arn.rfind("/") + 1:]
    if kind == "static" and fnmatch.fnmatchcase(kind_arn, "static:arn:aws*:s3:::*"):
        last = -1
        i = arn.find(":::")
        while i >= 0:
            last = i
            i = arn.find(":::", i + 3)
        return arn[last + 3:]
    cli.err("cbase: %s=%s is on %s, which is not a %s\n" % (tag, name, arn, rtype))
    return None


def no_state_bucket(bucket: str) -> None:
    """Stops the tool: the account has no state bucket BUCKET yet."""
    cli.fail(cli.EXIT_FAIL, "there is no state bucket s3://%s in this account: ./cbase group-init makes it with the first group" % bucket)


def s3_get(bucket: str, key: str, file: str) -> Optional[str]:
    """The object at KEY in BUCKET into FILE. Returns its ETag, or None when there is no such key (the C's
    status 3). A missing state bucket or another failure stops the tool."""
    rc, out = call("s3api", "get-object", "--bucket", bucket, "--key", key, file,
                   "--query", "ETag", "--output", "text")
    if rc == 0:
        return out.rstrip("\n")
    c = code()
    if c == "NoSuchKey":
        return None
    if c == "NoSuchBucket":
        no_state_bucket(bucket)
    fail("reading s3://%s/%s" % (bucket, key))
    return None


def s3_put(bucket: str, key: str, file: str, cond: str) -> int:
    """Writes FILE under KEY if COND holds: "*" for no object yet, otherwise the ETag the object must still
    have. Returns 0 when written, 3 when the precondition failed (someone else wrote first). Other failures
    stop the tool."""
    if cond == "*":
        rc, _o = call("s3api", "put-object", "--bucket", bucket, "--key", key, "--body", file,
                      "--content-type", "text/plain; charset=utf-8", "--if-none-match", "*",
                      "--query", "ETag", "--output", "text")
    else:
        rc, _o = call("s3api", "put-object", "--bucket", bucket, "--key", key, "--body", file,
                      "--content-type", "text/plain; charset=utf-8", "--if-match", cond,
                      "--query", "ETag", "--output", "text")
    if rc == 0:
        return 0
    c = code()
    if c in ("PreconditionFailed", "ConditionalRequestConflict", "NoSuchKey"):
        return 3
    if c == "NoSuchBucket":
        no_state_bucket(bucket)
    fail("writing s3://%s/%s" % (bucket, key))
    return 3


_DELETE_BATCH = 1000   # S3's limit of keys in one delete-objects call

_SAFE_KEY = re.compile(r"[A-Za-z0-9/._~-]+\Z", re.A)


def _delete_key_safe(key: str) -> bool:
    """Whether KEY can sit in the shorthand `Objects=[{Key=KEY},...]` as it is: only these characters, none
    of which the shorthand parser, the shell-free argv or the XML body can change."""
    return _SAFE_KEY.match(key) is not None


def _delete_batch(bucket: str, region_: str, keys: List[str]) -> None:
    """One delete-objects call for KEYS, Quiet, so a clean answer holds no Errors."""
    n = len(keys)
    arg = "Objects=[" + ",".join("{Key=%s}" % k for k in keys) + "],Quiet=true"
    rc, out = call("s3api", "delete-objects", "--bucket", bucket, "--delete", arg, "--region", region_,
                   "--query", "Errors[].[Key,Code,Message]", "--output", "text")
    if rc != 0:
        fail("deleting %d keys of s3://%s" % (n, bucket))
    text = cli.text_of(out)
    # A clean batch prints nothing ("None" is the CLI's text for an absent list); a line is an error: key, code, message.
    if text and text != "None":
        errors = text.count("\n") + 1   # the lines: one more than the newlines
        first = text.split("\n", 1)[0]
        parts = first.split("\t", 2)
        key = parts[0]
        c = parts[1] if len(parts) > 1 else None
        msg = parts[2] if len(parts) > 2 else None
        cli.fail(cli.EXIT_FAIL, "deleting s3://%s/%s failed: %s: %s (%d of the %d keys in the batch were not deleted)"
                 % (bucket, key, c if c is not None else "?", msg if msg is not None else "", errors, n))


def s3_delete_keys(bucket: str, region_: str, keys: Sequence[str]) -> None:
    """Deletes the KEYS of BUCKET (REGION) for a prune, and prints one "pruned s3://BUCKET/KEY" line per key
    after the call that deleted it succeeded. A key made only of letters, digits and "/ . _ - ~" goes in one
    `s3api delete-objects` call with up to 1000 keys, in the CLI's shorthand syntax; any other key
    (a space, comma, bracket, quote, equals, non-ASCII: shorthand cannot carry it unmangled) goes alone
    through `s3api delete-object`. An error entry in a batch's answer, or a failed call, stops the tool. The
    caller has already held every key to its folder: nothing is checked here."""
    batch: List[str] = []
    for key in keys:
        if not _delete_key_safe(key):
            rc, _o = call("s3api", "delete-object", "--bucket", bucket, "--key", key, "--region", region_)
            if rc != 0:
                fail("deleting s3://%s/%s" % (bucket, key))
            cli.out("pruned s3://%s/%s\n" % (bucket, key))
            continue
        batch.append(key)
        if len(batch) == _DELETE_BATCH:
            _delete_batch(bucket, region_, batch)
            for k in batch:
                cli.out("pruned s3://%s/%s\n" % (bucket, k))
            batch = []
    if batch:   # the last, partial batch
        _delete_batch(bucket, region_, batch)
        for k in batch:
            cli.out("pruned s3://%s/%s\n" % (bucket, k))


def fail(what: str) -> None:
    """Stops the tool (exit 1) on a failed call the caller does not handle, after printing what the person
    can do about it (credentials, an old CLI, AccessDenied, or the masked error text). WHAT names the call."""
    _print_failure(what)
    sys.exit(cli.EXIT_FAIL)


def _atol(s: str) -> int:
    """C's atol: blanks, a sign, digits, then whatever follows is ignored; 0 when there are none."""
    m = re.match(r"[ \t\n\v\f\r]*([+-]?[0-9]+)", s, re.A)
    return int(m.group(1)) if m else 0


def poll(tries: int, fn: Callable[[], bool]) -> bool:
    """THE BOUNDED WAIT. Calls FN up to TRIES times, POLL_SECONDS apart (CBASE_POLL_SECONDS, 5 by default),
    until FN returns true. Returns true when FN did, false when it never did. FN may stop the tool itself."""
    env = os.environ.get("CBASE_POLL_SECONDS")
    secs = _atol(env) if env else 5
    n = 1
    while True:
        if fn():
            return True
        if n >= tries:
            return False
        if secs > 0:
            cli.sleep_ms(secs * 1000)
        n += 1


def set_err_text(text: str) -> None:
    """Replaces the held stderr of the last call with TEXT, exactly (the change set's status reason or the
    events are written there before show)."""
    global _err
    _err = text


# The plain parameters, whose values may be printed.
_PLAIN_PARAMS = ("VmOrigin", "VmPort", "InstanceType", "ImageId", "CloudFrontPrefixList", "ArtifactBucket")


def secret_filter(path: str, before: str, after: str, cause: Optional[str]) -> str:
    """The JMESPath filter that holds a detail (a change set's Target, a drift's property difference)
    secret: its path or either value names an origin's custom headers, or, with CAUSE (a change set), a
    parameter caused it that is not plain."""
    f = ""
    for w in ("OriginCustomHeaders", "HeaderValue"):
        t = "contains(%s || '', '%s') || contains(%s || '', '%s') || contains(%s || '', '%s')" % (path, w, before, w, after, w)
        if f:
            f += " || "
        f += t
    if cause:
        f += " || (ChangeSource == 'ParameterReference'"
        for q in _PLAIN_PARAMS:
            f += " && CausingEntity != '%s'" % q
        f += ")"
    return f
