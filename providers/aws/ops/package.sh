#!/bin/bash
# Builds the Lambda upload: <product>/build/cbase-lambda.zip and its sha256.
#
#   CBASE_PRODUCT=<the product's Makefile directory> providers/aws/ops/package.sh
#
# CBASE_PRODUCT defaults to c_base itself (its example app). The binary itself is
# `make -C <product> bootstrap` (ops/build_static.sh: a
# static Linux arm64 `bootstrap` built in a pinned Alpine container, the same
# bytes every run). This script calls it rather than repeating it, copies the
# zip to the name the runbook and the CloudFormation template use, checks the
# zip holds exactly one executable named `bootstrap` (what provided.al2023
# runs), and prints what to upload.
#
# Needs Docker. Run on this Mac or on the CI's ubuntu runner.
set -euo pipefail

here="$(cd "$(dirname "$0")/../../.." && pwd)"   # the c_base root
prod="$(cd "${CBASE_PRODUCT:-$here}" && pwd)"    # the product's build
make -C "$prod" bootstrap

src="$prod/dist/bootstrap.zip"
out="$prod/build/cbase-lambda.zip"
mkdir -p "$prod/build"
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
  2. the template:  $here/providers/aws/ops/cbase.yaml
                    in CloudFormation > Create stack > Upload a template file
MSG
