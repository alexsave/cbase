# stack.py - the tool's CloudFormation stacks (the port of stack.c, stack.sh's rules): the templates
# rendered for an app, the route behaviours, the router's size check, the stack's status and outputs, and
# the change-set flow every stack goes through (drift, the guards, execute, discard, delete).
#
# The shell's STACK_* variables are stk_* here; stack_status is both a function (status) and STACK_STATUS's
# value (stk_status). The templates are read at run time from tools/cbase.d/templates/.
from __future__ import annotations

import re
import sys
from typing import Dict, List, Optional, Tuple

import aws
import cli
import conf as confmod

conf = confmod.conf

# The stack the next change set or delete is on: its name, region and tag ("Key=K,Value=V"), and its
# capabilities ("" or CAPABILITY_IAM). use sets them; params_clear and param_add manage the parameters.
stk_name = ""
stk_region = ""
stk_tag = ""
stk_caps = ""
stk_app = False           # an app's own stack, whose redeploys the downtime guard holds
stk_template = ""         # the rendered template's file
stk_status = ""           # before this run: NONE, or the stack's status (status sets it)
stk_type = ""             # CREATE or UPDATE
stk_cs = "cbase-deploy"   # the change set's name
stk_changes = ""          # "1" when the change set holds changes
stk_cert = ""             # the distribution's certificate ARN, or ""
stk_api_cert = ""         # CBASE_API_HOST's distribution's certificate ARN, or ""
stk_bucket = ""           # the app's bucket, its region and its folder (static.sh)
stk_bucket_region = ""
stk_prefix = ""
stk_no_rollback = False   # an attach's update is not rolled back
stk_files = ""            # ST_FILES: the static dir's file list (static.scan), "" when none
stk_addr = ""             # the VM origin's IP address type, ipv6 or dualstack, or ""; adds the "addr" block

# The CloudFront policies and the router's size limit (stack.sh's constants).
CF_CACHING_OPTIMIZED = "658327ea-f89d-4fab-a63d-7e88639e58f6"
CF_CACHING_DISABLED = "4135ea2d-6df8-44a3-9df3-4b5a84be39ad"
CF_ALL_VIEWER_EXCEPT_HOST = "b689b0a8-53d0-40ab-baf2-68738e2966ac"
CF_USE_ORIGIN_CC_QS = "4cc15a8a-d715-48a4-82b8-cc0b614638fe"
STACK_FUNCTION_MAX = 10240

# ---- the stack's identity ------------------------------------------------------------------------

_params: List[str] = []   # "ParameterKey=K,ParameterValue=V" each


def use(name: str, region: str, tag: str) -> None:
    """The stack the next change set or delete is on, with no parameters yet."""
    global stk_name, stk_region, stk_tag, stk_caps, stk_app, stk_template
    stk_name = name
    stk_region = region
    stk_tag = tag
    stk_caps = ""
    del _params[:]
    stk_app = False
    stk_template = cli.tmp_path("stack.yaml")


def app_use() -> None:
    """The app's own stack, cbase-app-<CONF_APP> in us-east-1."""
    global stk_app
    use("cbase-app-%s" % conf.app, "us-east-1", "Key=cbase:app,Value=%s" % conf.app)
    stk_app = True


def params_clear() -> None:
    del _params[:]


def param_add(key: str, value: str) -> None:
    _params.append("ParameterKey=%s,ParameterValue=%s" % (key, value))


def plan() -> None:
    """The app's template rendered into stk_template, then its change set."""
    render(stk_template)
    changeset()


def param_previous(key: str) -> None:
    """The parameter KEY keeps the value the stack holds (UsePreviousValue=true)."""
    _params.append("ParameterKey=%s,UsePreviousValue=true" % key)


def _cfn(*args: str) -> Tuple[int, str]:
    """One CloudFormation call in the stack's region."""
    return aws.callv(["cloudformation", "--region", stk_region], *args)


def status() -> None:
    """Reads the stack's status into stk_status (NONE when there is no stack). Stops on a failed read other
    than a missing stack."""
    global stk_status
    rc, out = _cfn("describe-stacks", "--stack-name", stk_name, "--query", "Stacks[0].StackStatus",
                   "--output", "text")
    if rc == 0:
        stk_status = out.rstrip("\n")
        return
    if aws.code() == "ValidationError" and aws.err_contains("does not exist"):
        stk_status = "NONE"
        return
    aws.fail("reading the %s stack" % stk_name)


def output(key: str) -> str:
    """One of the stack's outputs, "" when none. Stops the tool on a failed read."""
    query = "Stacks[0].Outputs[?OutputKey=='%s'].OutputValue" % key
    rc, out = _cfn("describe-stacks", "--stack-name", stk_name, "--query", query, "--output", "text")
    if rc != 0:
        aws.fail("reading %s's outputs" % stk_name)
    out = out.rstrip("\n")
    if out == "None":
        return ""
    return out


# ---- templates -----------------------------------------------------------------------------------

_tvars: Dict[str, str] = {}


def template_clear() -> None:
    """The values template_render replaces each __NAME__ with (the shell's T_NAME variables)."""
    _tvars.clear()


def template_set(name: str, value: str) -> None:
    _tvars[name] = value


def _template_value(name: str) -> str:
    return _tvars.get(name, "")   # a name not set is replaced by ""


def _template_bytes(file: str) -> str:
    if file not in ("app.yaml", "group-vm.yaml", "group-static.yaml"):
        cli.fail(cli.EXIT_FAIL, "internal: no template %s" % file)
    path = cli.tools_dir() + "/cbase.d/templates/" + file
    text = cli.read_file(path)
    if text is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % path)
        return ""
    return text


def _flag_in(flag: str, flags: str) -> bool:
    """Whether FLAG is one of the blank-separated FLAGS."""
    return flag in [w for w in flags.split(" ") if w]


def _text_lines(text: str) -> List[str]:
    """The lines of TEXT as the C's loops see them: cut at each newline, no empty piece after the last."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


_NAME_RE = re.compile(r"__([A-Z0-9]+)__", re.A)


def template_render(file: str, flags: str) -> str:
    """templates/FILE with each "#if FLAG" block kept only when FLAG is one of the blank-separated FLAGS (a
    "#if !FLAG" is the opposite) and each __NAME__ replaced by the value of that name. Returns the text (the
    C wrote it to a stream)."""
    text = _template_bytes(file)
    out: List[str] = []
    skip = 0
    depth = 0
    for line in _text_lines(text):
        if line.startswith("#if "):
            depth += 1
            f = line[4:]
            if skip == 0:
                if f[:1] == "!":
                    if _flag_in(f[1:], flags):
                        skip = depth
                elif not _flag_in(f, flags):
                    skip = depth
            continue
        if line == "#end":
            if skip == depth:
                skip = 0
            depth -= 1
            continue
        if skip:
            continue
        # each __NAME__ replaced by its value; a value is never read again
        out.append(_NAME_RE.sub(lambda m: _template_value(m.group(1)), line))
        out.append("\n")
    return "".join(out)


# ---- the routes ----------------------------------------------------------------------------------


def _methods(methods: str) -> str:
    padded = "," + methods + ","
    for m in (",ALL,", ",PUT,", ",POST,", ",PATCH,", ",DELETE,"):
        if m in padded:
            return "[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE]"
    if ",OPTIONS," in padded:
        return "[GET, HEAD, OPTIONS]"
    return "[GET, HEAD]"


def _behaviour(r: confmod.Route, rhp: Optional[str]) -> str:
    """One route's behaviour. RHP is the response headers policy's name, or None."""
    f: List[str] = []
    pad = "          "
    if r.pattern != "*":
        f.append('%s- PathPattern: "%s"\n' % (pad, r.pattern))
        pad = pad + "  "
    f.append("%sTargetOriginId: %s\n" % (pad, r.origin))
    f.append("%sViewerProtocolPolicy: redirect-to-https\n" % pad)
    f.append("%sAllowedMethods: %s\n" % (pad, _methods(r.methods)))
    f.append("%sCachedMethods: [GET, HEAD]\n" % pad)
    if r.origin == "ws":
        f.append("%sCompress: false\n" % pad)
    else:
        f.append("%sCompress: true\n" % pad)
    if r.cache == "cache":
        policy = CF_CACHING_OPTIMIZED + "   # Managed-CachingOptimized"
    elif r.cache == "origin":
        policy = CF_USE_ORIGIN_CC_QS + "   # Managed-UseOriginCacheControlHeaders-QueryStrings"
    else:
        policy = CF_CACHING_DISABLED + "   # Managed-CachingDisabled"
    f.append("%sCachePolicyId: %s\n" % (pad, policy))
    if r.origin != "static":
        f.append("%sOriginRequestPolicyId: %s   # Managed-AllViewerExceptHostHeader\n" % (pad, CF_ALL_VIEWER_EXCEPT_HOST))
        if rhp:
            f.append("%sResponseHeadersPolicyId: !Ref %s\n" % (pad, rhp))
    else:
        f.append("%sFunctionAssociations:\n" % pad)
        f.append("%s  - EventType: viewer-request\n" % pad)
        f.append("%s    FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN\n" % pad)
    return "".join(f)


def _routes_text(default: confmod.Route, routes: List[confmod.Route], rhp: Optional[str]) -> str:
    """A distribution's behaviours: DEFAULT, then ROUTES in order; trailing newlines removed ($(...))."""
    f = ["        DefaultCacheBehavior:\n", _behaviour(default, rhp)]
    if routes:
        f.append("        CacheBehaviors:\n")
    for r in routes:
        f.append(_behaviour(r, rhp))
    return "".join(f).rstrip("\n")


_PAGE_RE = r"^[A-Za-z0-9_.~/-]+\.html\Z"


def _pages() -> str:
    """The build's page names, as the router takes them: each html file of stk_files less its .html,
    blank-separated; a name of other characters, and a .well-known one, is left out."""
    if not stk_files:
        return ""
    data = cli.read_file(stk_files)
    if data is None:
        return ""
    names: List[str] = []
    for p in data.split("\n"):
        if cli.re(_PAGE_RE, p) and not p.startswith(".well-known/"):
            names.append(p[:-5])
    return " ".join(names)


def _router_size(text: str) -> int:
    """The router's code in bytes as CloudFront counts it (stack.sh's awk): its lines less the template's
    eight-space indent, one newline between each."""
    marker = "      FunctionCode: |\n"
    i = text.find(marker)
    if i < 0:
        return 0
    p = text[i + len(marker):]
    n = 0
    c = 0
    while p and p[0] != "\n":
        nl = p.find("\n")
        line = p if nl < 0 else p[:nl]
        n += len(line.encode("utf-8", "surrogateescape")) - 8 + (1 if c else 0)
        c += 1
        if nl < 0:
            break
        p = p[nl + 1:]
    return n


def render(file: str) -> None:
    """The app's template into FILE, from cbase.conf and the stk_* values above. Stops the tool when the
    router's code is over CloudFront's size limit, or the app shell is not in the build."""
    has_static = False
    has_vm = False
    has_ws = False
    for r in conf.routes:
        if r.origin == "static":
            has_static = True
        elif r.origin == "vm":
            has_vm = True
        elif r.origin == "ws":
            has_ws = True
    if conf.api_host:
        has_vm = True
    apiws = has_ws
    for r in conf.api_routes:
        if r.origin == "ws":
            apiws = True
    flags = ""
    if has_static:
        flags += " static"
    if has_vm or has_ws:
        flags += " vm"
    if has_vm:
        flags += " vmroute"
    if has_ws:
        flags += " ws"
    if stk_addr:
        flags += " addr"
    if stk_cert and conf.domain:
        flags += " cert"
    if conf.api_host:
        flags += " api"
    if apiws:
        flags += " apiws"
    if stk_api_cert:
        flags += " apicert"
    if conf.aliases != "off":
        flags += " aliases"

    shell = conf.shell if conf.shell else "/index.html"
    pages = ""
    if has_static:
        if stk_files:
            data = cli.read_file(stk_files)
            if data is not None:
                found = False
                for p in data.split("\n"):
                    if p == shell[1:]:
                        found = True
                if not found:
                    cli.fail(cli.EXIT_FAIL, "the app shell is %s (CBASE_SHELL), and the static dir has no %s: nothing was changed" % (shell, shell[1:]))
        pages = _pages()

    aliases = ", ".join(conf.domains)

    # The distribution's routes: every route but the last, the default being the last (conf.sh).
    nroutes = len(conf.routes)
    routes = _routes_text(conf.routes[nroutes - 1], conf.routes[:nroutes - 1], None)

    # The API host's: CBASE_API_ROUTE's, then the ws routes (CBASE_ROUTE's) it does not name.
    api: List[confmod.Route] = []
    seen = " "
    for r in conf.api_routes:
        api.append(r)
        seen += r.pattern + " "
    for r in conf.routes:
        if r.origin != "ws" or (" %s " % r.pattern) in seen:
            continue
        api.append(r)
    api_def = confmod.Route("*", "vm", "nocache", "ALL", 0)
    apiroutes = _routes_text(api_def, api, "ServerTimingPolicy")

    template_clear()
    template_set("APP", conf.app)
    template_set("APIHOST", conf.api_host if conf.api_host else "")
    template_set("CERT", stk_cert)
    template_set("APICERT", stk_api_cert)
    template_set("ALIASES", aliases)
    template_set("BUCKET", stk_bucket)
    template_set("BUCKETREGION", stk_bucket_region)
    template_set("PREFIX", stk_prefix)
    template_set("SHELL", shell)
    template_set("PAGES", pages)
    template_set("ROUTES", routes)
    template_set("APIROUTES", apiroutes)
    template_set("ADDR", stk_addr)

    cli.write_file(file, template_render("app.yaml", flags))

    # The router's code, as CloudFront counts it: over its limit, the build has too many pages.
    back = cli.read_file(file)
    if back is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % file)
        return
    n = _router_size(back)
    if n > STACK_FUNCTION_MAX:
        cli.fail(cli.EXIT_FAIL, "the router's code is %d bytes with the build's page list, over CloudFront's %d: the build has too many pages; nothing was changed" % (n, STACK_FUNCTION_MAX))


# ---- the change-set flow (stack.sh) --------------------------------------------------------------


def _fail_describe() -> None:
    """A failed read of the change set (the shell's aws_fail "describing change set ...")."""
    aws.fail("describing change set %s" % stk_cs)


def _cs(*args: str) -> Tuple[int, str]:
    """A change set's call on STACK_NAME with STACK_CS (the shell's cs())."""
    return aws.callv(["cloudformation", "--region", stk_region, "describe-change-set", "--stack-name", stk_name,
                      "--change-set-name", stk_cs], *args)


def _lines(text: str) -> List[str]:
    """The lines of TEXT (the C's line_next): none for an empty text, no empty piece after the last newline."""
    return _text_lines(text)


def _where(p: str) -> str:
    """A property path's place, as the origin header and parameter lines name it."""
    q = p
    if q.startswith("/Properties"):
        q = q[11:]
    if q.startswith("/DistributionConfig/"):
        q = q[20:]
    return q if q else "the property"


def _value(v: str) -> str:
    """A value on one line, at most 160 bytes (a longer one cut to 157 and "...")."""
    b = v.encode("utf-8", "surrogateescape").replace(b"\n", b" ").replace(b"\t", b" ")
    if len(b) > 160:
        b = b[:157] + b"..."
    return b.decode("utf-8", "surrogateescape")


def _drifts(lid: str, query: str) -> str:
    """The property differences of the drifted resource LID that QUERY selects: the query's tail is QUERY,
    the list's end included in it."""
    q = "StackResourceDrifts[?LogicalResourceId=='%s'] | [0].PropertyDifferences[%s" % (lid, query)
    rc, out = _cfn("describe-stack-resource-drifts", "--stack-name", stk_name, "--stack-resource-drift-status-filters",
                   "MODIFIED", "--query", q, "--output", "text")
    if rc != 0:
        aws.fail("reading %s's drift" % lid)
    return cli.text_of(out)


# ---- drift ---------------------------------------------------------------------------------------

_drift_id = ""
_drift_ds = ""
_drift_sds = ""


def _drift_read() -> bool:
    """One read of drift detection _drift_id; true once it is no longer running."""
    global _drift_ds, _drift_sds
    rc, out = _cfn("describe-stack-drift-detection-status", "--stack-drift-detection-id", _drift_id, "--query",
                   "[DetectionStatus,StackDriftStatus]", "--output", "text")
    if rc != 0:
        aws.fail("reading drift detection %s" % _drift_id)
    _drift_ds, _drift_sds = cli.split_tab(cli.text_of(out))
    return _drift_ds != "DETECTION_IN_PROGRESS"


def _drift_guard() -> None:
    """Stops on a drifted stack unless CBASE_ACCEPT_DRIFT=1 (--accept-drift)."""
    global _drift_id
    cli.out("drift: detecting on %s\n" % stk_name)
    rc, idout = _cfn("detect-stack-drift", "--stack-name", stk_name, "--query", "StackDriftDetectionId",
                     "--output", "text")
    if rc != 0:
        aws.fail("detecting drift on %s" % stk_name)
    _drift_id = cli.text_of(idout)
    if not aws.poll(121, _drift_read):
        cli.err("cbase: drift detection %s on %s did not finish\n" % (_drift_id, stk_name))
        sys.exit(cli.EXIT_FAIL)
    rc, dr = _cfn("describe-stack-resource-drifts", "--stack-name", stk_name, "--stack-resource-drift-status-filters",
                  "MODIFIED", "DELETED", "--query",
                  "StackResourceDrifts[].[LogicalResourceId,ResourceType,StackResourceDriftStatus]",
                  "--output", "text")
    if rc != 0:
        aws.fail("reading %s's drift" % stk_name)
    drifted = cli.text_of(dr)
    if drifted == "None":
        drifted = ""
    if _drift_ds == "DETECTION_COMPLETE" and _drift_sds == "IN_SYNC" and not drifted:
        cli.out("drift: none\n")
        return
    if _drift_ds != "DETECTION_COMPLETE":
        cli.err("drift: detection ended %s, so the stack could not be shown to be in sync\n" % _drift_ds)
    flt = aws.secret_filter("PropertyPath", "ExpectedValue", "ActualValue", None)
    for line in _lines(drifted):
        f = cli.read_tab_fields(line, 3)
        lid, typ, st = f[0], f[1], f[2]
        if not lid:
            continue
        cli.err("drift: %s (%s) %s\n" % (lid, typ, st))
        if st != "MODIFIED":
            continue
        q1 = "?PropertyPath && !(%s)].[PropertyPath,DifferenceType,ExpectedValue || '',ActualValue || '']" % flt
        text = _drifts(lid, q1)
        for l2 in _lines(text):
            g = cli.read_tab_fields(l2, 4)
            if not g[0]:
                continue
            cli.err("drift:   %s %s: expected %s, actual %s\n" % (_where(g[0]), g[1], _value(g[2]), _value(g[3])))
        q2 = "?%s].PropertyPath" % flt
        paths = _drifts(lid, q2)
        for l2 in _lines(paths):
            # read -r -a: the words of the line
            for w in cli.split_on(l2, " \t"):
                if w != "None":
                    cli.err("drift:   origin header %s changed (its value is not shown)\n" % _where(w))
    if cli.flags.accept_drift:
        cli.err("drift: accepted (--accept-drift): this deploy makes the stack's resources match the template again\n")
        return
    cli.err("cbase: the %s stack drifted from its template (above): a deploy would undo those changes. Put the change in cbase.conf, or run ./cbase deploy --accept-drift (CBASE_ACCEPT_DRIFT=1) to overwrite it. Nothing was changed.\n" % stk_name)
    sys.exit(cli.EXIT_FAIL)


# ---- the change set ------------------------------------------------------------------------------


def _details(lid: str, query: str) -> str:
    """The change set's details of the resource LID that QUERY selects."""
    q = "Changes[?ResourceChange.LogicalResourceId=='%s'] | [0].ResourceChange.Details[%s" % (lid, query)
    rc, out = _cs("--include-property-values", "--query", q, "--output", "text")
    if rc != 0:
        _fail_describe()
    return cli.text_of(out)


def _print_changes() -> None:
    """The change set, resource by resource, a secret's value never shown."""
    rc, out = _cs("--query", "Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]",
                  "--output", "text")
    if rc != 0:
        _fail_describe()
    lines = cli.text_of(out)
    cli.out("change set %s on %s (%s):\n" % (stk_cs, stk_name, stk_type))
    flt = aws.secret_filter("Target.Path", "Target.BeforeValue", "Target.AfterValue", "cause")
    for line in _lines(lines):
        f = cli.read_tab_fields(line, 4)
        action, lid, typ, repl = f[0], f[1], f[2], f[3]
        if not action:
            continue
        if action != "Modify":
            cli.out("  %-7s %-16s %s\n" % (action, lid, typ))
            continue
        cli.out("  %-7s %-16s %s (replacement: %s)\n" % (action, lid, typ, repl))
        q1 = "?Target.Name && !(%s)].[Target.Path || Target.Name,Target.BeforeValue || '',Target.AfterValue || '']" % flt
        text = _details(lid, q1)
        for l2 in _lines(text):
            g = cli.read_tab_fields(l2, 3)
            if not g[0]:
                continue
            w = _where(g[0])
            if not g[1] and not g[2]:
                cli.out("      %s changes\n" % w)
            else:
                cli.out("      %s: %s -> %s\n" % (w, _value(g[1]), _value(g[2])))
        q2 = "?%s].[Target.Path || Target.Name || '',CausingEntity || '']" % flt
        text2 = _details(lid, q2)
        for l2 in _lines(text2):
            g = cli.read_tab_fields(l2, 2)
            if not g[0]:
                continue
            w = _where(g[0])
            if g[1] != "None" and g[1] and "OriginCustomHeaders" not in g[0] and "HeaderValue" not in g[0]:
                cli.out("      parameter %s changed %s (its value is not shown)\n" % (g[1], w))
            else:
                cli.out("      origin header %s changed (its value is not shown)\n" % w)


def _refusals(types: str) -> str:
    """The change set's removals and replacements of the resources whose type is one of TYPES (a JMESPath
    condition), "<logical id>\\t<type>\\t<action>\\t<replacement>" a line; "" when none."""
    q = ("Changes[?(%s) && (ResourceChange.Action=='Remove' || ResourceChange.Replacement=='True' || "
         "ResourceChange.Replacement=='Conditional')].ResourceChange.[LogicalResourceId,ResourceType,Action,Replacement]"
         % types)
    rc, out = _cs("--query", q, "--output", "text")
    if rc != 0:
        _fail_describe()
    return cli.text_of(out)


_ALIAS_LID_RE = r"^  [A-Za-z]+:\Z"
_ALIASES_PREFIX = "        Aliases: ["


def _aliases(text: str) -> List[str]:
    """Each distribution's hostnames in a template, "<logical id> <host>" a line (stack.sh's awk)."""
    res: List[str] = []
    lid = ""
    for line in _lines(text):
        if cli.re(_ALIAS_LID_RE, line):
            lid = line[2:-1]
        if line.startswith(_ALIASES_PREFIX):
            s = line[len(_ALIASES_PREFIX):]
            if s.endswith("]"):
                s = s[:-1]
            if s:
                for h in s.split(", "):
                    res.append("%s %s" % (lid, h))
    return res


def _sorted_lines(items: List[str]) -> List[str]:
    """The non-empty lines, sorted in byte order (sort with LC_ALL=C)."""
    return cli.sort_bytes([x for x in items if x])


def _comm_only_left(a: List[str], b: List[str]) -> List[str]:
    """The lines of A not in B, both sorted (comm -23), with multiplicity."""
    res: List[str] = []
    i = 0
    j = 0
    while i < len(a):
        if j >= len(b):
            res.append(a[i])
            i += 1
            continue
        ka = a[i].encode("utf-8", "surrogateescape")
        kb = b[j].encode("utf-8", "surrogateescape")
        if ka == kb:
            i += 1
            j += 1
        elif ka < kb:
            res.append(a[i])
            i += 1
        else:
            j += 1
    return res


def _downtime_guard() -> None:
    """NO DOWNTIME after the first deploy (stack.sh's stack_downtime_guard)."""
    why: List[str] = []
    refused = _refusals("ResourceChange.ResourceType=='AWS::CloudFront::Distribution'")
    for line in _lines(refused):
        f = cli.read_tab_fields(line, 4)
        if not f[0]:
            continue
        if f[2] == "Remove":
            why.append("the distribution %s is removed" % f[0])
        else:
            why.append("the distribution %s is replaced: a new one, with a new cloudfront.net name, and the old one deleted" % f[0])
    rc, old_text = _cfn("get-template", "--stack-name", stk_name, "--query", "TemplateBody", "--output", "text")
    if rc != 0:
        aws.fail("reading %s's template" % stk_name)
    new_text = cli.read_file(stk_template)
    if new_text is None:
        cli.fail(cli.EXIT_FAIL, "cannot read %s" % stk_template)
        return
    a = _sorted_lines(_aliases(old_text))
    b = _sorted_lines(_aliases(new_text))
    for g in _comm_only_left(a, b):
        sp = g.find(" ")
        if sp < 0 or sp + 1 >= len(g):
            continue
        why.append("the hostname %s is taken off the distribution %s" % (g[sp + 1:], g[:sp]))
    if not why:
        return
    for w in why:
        cli.err("downtime: %s\n" % w)
    if cli.flags.allow_downtime:
        cli.err("downtime: allowed (--allow-downtime): the app is unreachable at those hostnames until the change is done and DNS follows\n")
        return
    _discard_impl()
    cli.err("cbase: this change set would make %s unreachable for a while (above), and after an app's first deploy no deploy may: refused, and the change set deleted; nothing was executed. Run ./cbase deploy --allow-downtime (CBASE_ALLOW_DOWNTIME=1) to accept it.\n" % conf.app)
    sys.exit(cli.EXIT_FAIL)


def _keep_guard() -> None:
    """THE ONE HARD STOP. A bucket or a VM group's instance replaced or removed is refused."""
    refused = _refusals("ResourceChange.ResourceType=='AWS::S3::Bucket' || ResourceChange.ResourceType=='AWS::EC2::Instance'")
    if not refused:
        return
    for line in _lines(refused):
        f = cli.read_tab_fields(line, 4)
        if not f[0]:
            continue
        what = "bucket" if f[1] == "AWS::S3::Bucket" else "instance"
        if f[2] == "Remove":
            cli.err("refused: the %s %s would be removed\n" % (what, f[0]))
        else:
            cli.err("refused: the %s %s would be replaced (Replacement: %s)\n" % (what, f[0], f[3]))
    _discard_impl()
    cli.err("cbase: the change set would replace or remove a bucket (and every file in it) or a VM group's instance (and every app's data on its disk): refused, and the change set deleted; nothing was executed\n")
    sys.exit(cli.EXIT_FAIL)


def _wait(waiter: str, what: str) -> None:
    """CloudFormation's wait on STACK_NAME, or the stack's failed resources (masked) and the tool stops.
    WHAT names the step for the message."""
    rc, _o = _cfn("wait", waiter, "--stack-name", stk_name)
    if rc == 0:
        return
    cli.err("cbase: the %s stack %s (%s). Its failed resources:\n" % (stk_name, what, waiter))
    _rc, ev = _cfn("describe-stack-events", "--stack-name", stk_name, "--query",
                   "StackEvents[?ends_with(ResourceStatus, 'FAILED')].[LogicalResourceId,ResourceStatusReason]",
                   "--output", "text")
    aws.set_err_text(ev)
    aws.show(10)
    sys.exit(cli.EXIT_FAIL)


def _discard_impl() -> None:
    """Deletes the change set, and the empty stack a first change set made."""
    rc, _o = _cfn("delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs)
    if rc != 0:
        aws.fail("deleting change set %s" % stk_cs)
    if stk_type == "CREATE" and stk_status == "NONE":
        rc, _o = _cfn("delete-stack", "--stack-name", stk_name)
        if rc != 0:
            aws.fail("deleting the empty %s stack" % stk_name)


def discard() -> None:
    _discard_impl()


def changeset() -> None:
    """THE CHANGE-SET FLOW (stack.sh's stack_changeset) for STACK_NAME from STACK_TEMPLATE: the drift check,
    the change set printed, the hard stops (a bucket or an instance replaced, a downtime). Sets stk_changes;
    a dry run's change set is deleted here. Stops the tool on a refusal, having changed nothing."""
    global stk_type, stk_changes
    status()
    if stk_status == "NONE" or stk_status == "REVIEW_IN_PROGRESS":
        stk_type = "CREATE"
        cli.out("stack: %s is new\n" % stk_name)
    elif stk_status == "ROLLBACK_COMPLETE":
        cli.err("cbase: the %s stack's first create failed and rolled back (ROLLBACK_COMPLETE), and CloudFormation updates no such stack: delete it (aws cloudformation delete-stack --region %s --stack-name %s; it holds nothing), then deploy again\n"
                % (stk_name, stk_region, stk_name))
        sys.exit(cli.EXIT_FAIL)
    elif len(stk_status) >= 12 and stk_status.endswith("_IN_PROGRESS"):
        cli.err("cbase: the %s stack is %s: another deploy is running, or CloudFormation is still working; wait for it, then deploy again\n"
                % (stk_name, stk_status))
        sys.exit(cli.EXIT_FAIL)
    else:
        stk_type = "UPDATE"
        cli.out("stack: %s is %s\n" % (stk_name, stk_status))
        _drift_guard()
    # A change set of this name left by a run that stopped: gone before the new one.
    if stk_status != "NONE":
        rc, _o = _cfn("delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs)
        if rc != 0 and aws.code() != "ChangeSetNotFound":
            aws.fail("clearing change set %s" % stk_cs)
    cw = ["create-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs, "--change-set-type", stk_type,
          "--template-body", "file://%s" % stk_template, "--tags", stk_tag]
    if stk_caps:
        cw += ["--capabilities", stk_caps]
    for i, p in enumerate(_params):
        if i == 0:
            cw.append("--parameters")
        cw.append(p)
    cw += ["--query", "Id", "--output", "text"]
    rc, _o = _cfn(*cw)
    if rc != 0:
        aws.fail("creating a change set on %s" % stk_name)
    rc, _o = _cfn("wait", "change-set-create-complete", "--stack-name", stk_name, "--change-set-name", stk_cs)
    if rc != 0:
        rc, wo = _cs("--query", "StatusReason", "--output", "text")
        if rc != 0:
            _fail_describe()
        why = cli.text_of(wo)
        if "didn't contain changes" in why or "No updates are to be performed" in why:
            cli.out("stack: %s is up to date (no changes)\n" % stk_name)
            stk_changes = ""
            rc, _o = _cfn("delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs)
            if rc != 0:
                aws.fail("deleting change set %s" % stk_cs)
            return
        aws.set_err_text(why + "\n")
        cli.err("cbase: change set %s on %s failed:\n" % (stk_cs, stk_name))
        aws.show(5)
        _discard_impl()
        sys.exit(cli.EXIT_FAIL)
    stk_changes = "1"
    _print_changes()
    _keep_guard()
    if stk_app and stk_type == "UPDATE":
        _downtime_guard()
    if stk_app and cli.flags.dry_run:
        _discard_impl()


def execute() -> None:
    """Runs the change set and waits for the stack to finish. Stops the tool on failure."""
    w = ["execute-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs]
    if stk_no_rollback:
        w.append("--disable-rollback")
    rc, _o = _cfn(*w)
    if rc != 0:
        aws.fail("executing change set %s on %s" % (stk_cs, stk_name))
    cli.out("stack: executing %s (a new distribution takes some minutes)\n" % stk_cs)
    wname = "stack-update-complete"
    if stk_type != "UPDATE":
        wname = "stack-create-complete"
    _wait(wname, "did not finish")
    # ${w%-complete}: the waiter's name without its "-complete"
    done = wname
    if done.endswith("-complete"):
        done = done[:-9]
    cli.out("stack: %s is %s complete\n" % (stk_name, done))


def delete() -> None:
    """Deletes the stack and waits; nothing when there is none."""
    status()
    if stk_status == "NONE":
        cli.out("stack: there is no %s stack: nothing to delete\n" % stk_name)
        return
    rc, _o = _cfn("delete-stack", "--stack-name", stk_name)
    if rc != 0:
        aws.fail("deleting the %s stack" % stk_name)
    cli.out("stack: deleting %s (a distribution takes some minutes to go)\n" % stk_name)
    _wait("stack-delete-complete", "was not deleted")
    cli.out("stack: %s is deleted\n" % stk_name)
