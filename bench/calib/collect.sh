#!/bin/bash
# collect.sh STATE : sync the run's results from its bucket into results_<run>/ and show runner.log and instance state.
# Acts only on the bucket and instance ids recorded in STATE.
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATE=${1:?usage: collect.sh state/<run>.env}
# shellcheck disable=SC1090
. "$STATE"
export AWS_PAGER= AWS_DEFAULT_REGION=$REGION
OUT=$D/results_$RUN
mkdir -p "$OUT"
aws s3 sync --only-show-errors "s3://$BUCKET/perf-temp-cal/" "$OUT/" --exclude pkg.tgz --exclude '*.tgz'
for f in "$OUT"/*/runner.log; do echo "== $f"; tail -3 "$f"; done
IDS=$(grep '^INSTANCE_' "$STATE" | cut -d= -f2 | tr '\n' ' ')
# shellcheck disable=SC2086
aws ec2 describe-instances --instance-ids $IDS --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text
echo "results: $OUT"
