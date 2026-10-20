#!/usr/bin/env python3
"""compare.py A B : compare two calbench runs row by row.

A and B are each a directory (every .tsv under it (p<pass>_<group>.tsv) is read, so a box's run folder works) or a single
TSV. A row is keyed by (group, name, param). Where a key appears in several passes the median of each column is used.

Columns of the output, all in ns per op (a sample times k back-to-back ops and divides by k, so check k: add_dep x8
is 8 adds per op, load_latency is 8 loads per op, and a p99 of k > 1 is the p99 of k-op averages):
  p50_A p50_B p50_B/A   the median sample of each run, and B over A
  min_B/A               the fastest sample (the cleanest read of the core's own cost)
  p99_B/A               the tail
Rows whose p50 ratio is outside [1/T, T] (T = --threshold, default 1.10) are listed; --all lists every row.
The steal group's rows are time series (see calbench.c) and compare as plain numbers.
"""
import argparse
import glob
import math
import os
import statistics
import sys

COLS = ("group", "name", "param", "k", "n", "min_ns", "max_ns", "mean_ns", "std_ns", "p50_ns", "p99_ns")


def read_tsv(path):
    """Parse one run (a TSV file or a directory of them) into {(group, name, param): {col: median float}}.
    Lines starting with # are the header and the metadata; a row has the 19 columns of the #cols line."""
    files = [path] if os.path.isfile(path) else sorted(glob.glob(os.path.join(path, "**", "*.tsv"), recursive=True))
    if not files:
        sys.exit("no TSV rows under " + path)
    acc = {}
    for f in files:
        with open(f) as fh:
            for line in fh:
                if line.startswith("#") or not line.strip():
                    continue
                c = line.rstrip("\n").split("\t")
                if len(c) < len(COLS):
                    continue
                key = (c[0], c[1], c[2])
                row = acc.setdefault(key, {col: [] for col in COLS[3:]})
                for col, v in zip(COLS[3:], c[3:]):
                    row[col].append(float(v))
    return {k: {col: statistics.median(v) for col, v in r.items()} for k, r in acc.items()}


def ratio(b, a):
    if a > 0 and b > 0:
        return b / a
    return math.nan


def fmt(x):
    return "%.3g" % x if not math.isnan(x) else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--threshold", type=float, default=1.10)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--group", help="only this group")
    args = ap.parse_args()
    A, B = read_tsv(args.a), read_tsv(args.b)
    keys = sorted(set(A) & set(B))
    if args.group:
        keys = [k for k in keys if k[0] == args.group]
    print("# A=%s (%d rows)  B=%s (%d rows)  common=%d  only-A=%d  only-B=%d" % (
        args.a, len(A), args.b, len(B), len(keys), len(set(A) - set(B)), len(set(B) - set(A))))
    print("# ns per op; k = ops timed per sample; ratios are B/A; threshold %.2f" % args.threshold)
    print("group\tname\tparam\tk\tp50_A\tp50_B\tp50_B/A\tmin_B/A\tp99_B/A")
    ratios, shown = [], 0
    for k in keys:
        a, b = A[k], B[k]
        r50 = ratio(b["p50_ns"], a["p50_ns"])
        if not math.isnan(r50):
            ratios.append(r50)
        outside = not math.isnan(r50) and (r50 > args.threshold or r50 < 1 / args.threshold)
        if args.all or outside:
            shown += 1
            print("\t".join([k[0], k[1], k[2], "%d" % a["k"], fmt(a["p50_ns"]), fmt(b["p50_ns"]), fmt(r50),
                             fmt(ratio(b["min_ns"], a["min_ns"])), fmt(ratio(b["p99_ns"], a["p99_ns"]))]))
    if ratios:
        lr = sorted(ratios)
        print("# %d rows with a p50 ratio, %d listed; p50 B/A median %.3f, p5 %.3f, p95 %.3f, geomean %.3f" % (
            len(ratios), shown, statistics.median(lr), lr[int(0.05 * (len(lr) - 1))], lr[int(0.95 * (len(lr) - 1))],
            math.exp(statistics.fmean(math.log(x) for x in lr))))


if __name__ == "__main__":
    main()
