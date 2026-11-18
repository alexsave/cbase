#!/bin/bash
# timing.sh FILE... - sums what CBASE_TEST_TIMING recorded (lib.sh): per file, the AWS CLI calls as count,
# total seconds and mean, then the ten operations that cost the most (count, seconds). Used by CI's macOS
# deploy tool job to say where the time goes.
set -euo pipefail
for f in "$@"; do
  awk -v f="$f" '
    $1 == "aws" { n++; t += $2; op = $3 " " $4; c[op]++; s[op] += $2 }
    END {
      printf "%s: aws %d calls, %.1f s, %.3f s each\n", f, n, t, (n ? t / n : 0)
      for (o in c) printf "  %6d %8.1f s  %s\n", c[o], s[o], o | "sort -k2 -rn | head -10"
    }' "$f"
done
