#!/bin/bash
# Builds the Lambda upload: c_base/build/cbase-lambda.zip and its sha256.
#
#   c_base/ops/package.sh
#
# The binary itself is `make -C c_base bootstrap` (tools/build_bootstrap.sh: a
# static Linux arm64 `bootstrap` built in a pinned Alpine container, the same
# bytes every run). This script calls it rather than repeating it, copies the
# zip to the name the runbook and the CloudFormation template use, checks the
# zip holds exactly one executable named `bootstrap` (what provided.al2023
# runs), and prints what to upload.
#
# Needs Docker. Run on this Mac or on the CI's ubuntu runner.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$here" bootstrap

src="$here/dist/bootstrap.zip"
out="$here/build/cbase-lambda.zip"
mkdir -p "$here/build"
cp "$src" "$out"

# The zip must hold `bootstrap` at its root and nothing else, executable.
listing="$(unzip -Z1 "$out")"
if [ "$listing" != "bootstrap" ]; then
  echo "package.sh: $out holds [$listing], expected exactly [bootstrap]" >&2
  exit 1
fi
mode="$(unzip -Z "$out" bootstrap | awk '{print $1}')"
case "$mode" in
  -rwx*) ;;
  *) echo "package.sh: bootstrap in the zip is $mode, not executable" >&2; exit 1 ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
  sum="$(sha256sum "$out" | awk '{print $1}')"
else
  sum="$(shasum -a 256 "$out" | awk '{print $1}')"
fi
echo "$sum  cbase-lambda.zip" > "$out.sha256"
size="$(wc -c < "$out" | tr -d ' ')"

cat <<MSG

Upload these (c_base/RUNBOOK.md, steps 3 and 4):
  1. the code:      $out
                    ($size bytes, sha256 $sum)
                    to the S3 bucket you name as the stack's CodeBucket, key cbase-lambda.zip
  2. the template:  $here/ops/cbase.yaml
                    in CloudFormation > Create stack > Upload a template file
MSG
