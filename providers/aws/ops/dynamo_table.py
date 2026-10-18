# dynamo_table.py - the DynamoDB table ops/cbase.yaml defines, as AWS CLI input.
#
#   python3 -I ops/dynamo_table.py ops/cbase.yaml <table name> <out dir>
#
# Reads the template's one AWS::DynamoDB::Table resource and writes, into
# <out dir>:
#   create.json  create-table's --cli-input-json (keys, attributes, indexes,
#                billing and provisioned throughput; DynamoDB Local accepts the
#                throughput and enforces none of it)
#   ttl.json     update-time-to-live's --cli-input-json
#   pitr.json    update-continuous-backups's --cli-input-json
# so ops/dynamo_local.sh creates exactly the table the stack creates and the two
# cannot drift. A property this script does not know how to carry over is an
# error, not a silent omission: a new one in the template must be mapped here.
#
# The standard library only (no PyYAML on a stock Mac): the reader below takes
# the YAML subset the resource is written in (block mappings, block sequences,
# one-line flow mappings, plain scalars, `!Ref TableName`) and refuses anything
# else.
import json
import os
import re
import sys


def die(msg):
    sys.stderr.write("dynamo_table.py: %s\n" % msg)
    sys.exit(1)


def scalar(text, table):
    text = text.strip()
    if text.startswith("!Ref "):
        if text[5:].strip() != "TableName":
            die("unexpected !Ref %r" % text)
        return table
    if text.startswith("!"):
        die("unsupported tag in %r" % text)
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?[0-9]+", text):
        return int(text)
    return text


def flow_map(text, table):
    inner = text.strip()
    if not (inner.startswith("{") and inner.endswith("}")):
        die("not a flow mapping: %r" % text)
    out = {}
    for part in inner[1:-1].split(","):
        if not part.strip():
            continue
        k, sep, v = part.partition(":")
        if not sep or "{" in v or "[" in v:
            die("unsupported flow mapping entry %r" % part)
        out[k.strip()] = scalar(v, table)
    return out


def value(text, table):
    t = text.strip()
    return flow_map(t, table) if t.startswith("{") else scalar(t, table)


def parse_block(lines, i, indent, table):
    """Parses the block starting at lines[i], all at exactly `indent`.
    Returns (value, next index)."""
    if lines[i][1].startswith("- "):
        seq = []
        while i < len(lines) and lines[i][0] == indent and lines[i][1].startswith("- "):
            rest = lines[i][1][2:]
            if ":" in rest and not rest.lstrip().startswith("{"):
                # "- Key: value" opens a mapping whose further keys sit at indent + 2.
                sub = [(indent + 2, rest)]
                j = i + 1
                while j < len(lines) and lines[j][0] > indent:
                    sub.append(lines[j])
                    j += 1
                v, k = parse_block(sub, 0, indent + 2, table)
                if k != len(sub):
                    die("trailing lines in a sequence item near %r" % rest)
                seq.append(v)
                i = j
            else:
                seq.append(value(rest, table))
                i += 1
        return seq, i
    out = {}
    while i < len(lines) and lines[i][0] == indent:
        text = lines[i][1]
        if text.startswith("- "):
            die("a sequence item among mapping keys: %r" % text)
        key, sep, rest = text.partition(":")
        if not sep:
            die("not a mapping entry: %r" % text)
        key = key.strip()
        if rest.strip():
            out[key] = value(rest, table)
            i += 1
        else:
            i += 1
            if i >= len(lines) or lines[i][0] <= indent:
                die("empty value for %r" % key)
            out[key], i = parse_block(lines, i, lines[i][0], table)
    if i < len(lines) and lines[i][0] > indent:
        die("unexpected indentation at %r" % lines[i][1])
    return out, i


def table_resource(path, table):
    with open(path, encoding="utf-8") as f:
        raw = f.read().split("\n")
    # The resource: the lines under the key whose Type is AWS::DynamoDB::Table.
    starts = [n for n, l in enumerate(raw) if l.strip() == "Type: AWS::DynamoDB::Table"]
    if len(starts) != 1:
        die("expected exactly one AWS::DynamoDB::Table in %s, found %d" % (path, len(starts)))
    head = starts[0] - 1
    head_indent = len(raw[head]) - len(raw[head].lstrip())
    lines = []
    for l in raw[head + 1:]:
        s = l.split(" #", 1)[0].rstrip() if not l.lstrip().startswith("#") else ""
        if not s.strip():
            continue
        ind = len(s) - len(s.lstrip())
        if ind <= head_indent:
            break
        lines.append((ind, s.strip()))
    res, n = parse_block(lines, 0, lines[0][0], table)
    if n != len(lines):
        die("could not read the whole table resource")
    return res["Properties"]


def main():
    if len(sys.argv) != 4:
        die("usage: dynamo_table.py <cbase.yaml> <table name> <out dir>")
    path, table, out = sys.argv[1:]
    p = dict(table_resource(path, table))
    create = {}
    for k in ("TableName", "BillingMode", "ProvisionedThroughput", "AttributeDefinitions", "KeySchema",
              "GlobalSecondaryIndexes"):
        if k in p:
            create[k] = p.pop(k)
    # PROVISIONED needs throughput on the table and on every index, and
    # PAY_PER_REQUEST refuses it anywhere: said here, not by create-table later.
    provisioned = create.get("BillingMode") == "PROVISIONED"
    for what, part in [("the table", create)] + [(g.get("IndexName"), g) for g in create.get("GlobalSecondaryIndexes", [])]:
        if ("ProvisionedThroughput" in part) != provisioned:
            die("%s: ProvisionedThroughput %s with BillingMode %s" % (
                what, "missing" if provisioned else "given", create.get("BillingMode")))
    ttl = p.pop("TimeToLiveSpecification", None)
    pitr = p.pop("PointInTimeRecoverySpecification", None)
    if p:
        die("properties this script does not carry over: %s (map them here)" % ", ".join(sorted(p)))
    if create.get("TableName") != table or "KeySchema" not in create:
        die("the resource has no TableName: !Ref TableName or no KeySchema")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "create.json"), "w") as f:
        json.dump(create, f, indent=1, sort_keys=True)
    with open(os.path.join(out, "ttl.json"), "w") as f:
        json.dump({"TableName": table, "TimeToLiveSpecification": ttl}, f, indent=1, sort_keys=True)
    with open(os.path.join(out, "pitr.json"), "w") as f:
        json.dump({"TableName": table, "PointInTimeRecoverySpecification": pitr}, f, indent=1, sort_keys=True)


main()
