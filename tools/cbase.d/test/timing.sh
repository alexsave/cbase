#!/bin/bash
# timing.sh FILE... - sums what CBASE_TEST_TIMING recorded (lib.sh): per file, the AWS CLI calls and the
# tool's runs, each as count, total seconds and mean, and the tool's own share (its runs less the CLI
# calls inside them). Used by CI's macOS deploy tool job to say where the time goes.
set -euo pipefail
for f in "$@"; do
  awk -v f="$f" '
    $1 == "aws"  { an++; at += $2 }
    $1 == "tool" { tn++; tt += $2 }
    END {
      printf "%s: aws %d calls, %.1f s, %.3f s each; tool %d runs, %.1f s, %.3f s each; the tool less its aws calls %.1f s\n",
        f, an, at, (an ? at / an : 0), tn, tt, (tn ? tt / tn : 0), tt - at
    }' "$f"
done
