#!/bin/bash
# cleanup.sh STATE : terminate the run's instances, delete its security group and empty and delete its bucket.
# Acts ONLY on the exact ids and the bucket name recorded in STATE (never a wildcard or a tag search), and refuses a
# bucket name that is not cbase-perf-temp-cal-<8 hex>.
set -euo pipefail
STATE=${1:?usage: cleanup.sh state/<run>.env}
# shellcheck disable=SC1090
. "$STATE"
export AWS_PAGER= AWS_DEFAULT_REGION=$REGION
[[ $BUCKET =~ ^cbase-perf-temp-cal-[0-9a-f]{8}$ ]] || { echo "refusing bucket name $BUCKET" >&2; exit 1; }
[[ $SG =~ ^sg-[0-9a-f]+$ ]] || { echo "refusing sg $SG" >&2; exit 1; }
IDS=$(grep '^INSTANCE_' "$STATE" | cut -d= -f2 | tr '\n' ' ')
for i in $IDS; do [[ $i =~ ^i-[0-9a-f]+$ ]] || { echo "refusing instance id $i" >&2; exit 1; }; done
# shellcheck disable=SC2086
aws ec2 terminate-instances --instance-ids $IDS >/dev/null   # a no-op for one that already powered off
# shellcheck disable=SC2086
aws ec2 wait instance-terminated --instance-ids $IDS
aws ec2 delete-security-group --group-id "$SG"
aws s3 rm --recursive "s3://$BUCKET/" --only-show-errors
aws s3api delete-bucket --bucket "$BUCKET"
mv "$STATE" "$STATE.cleaned"
echo "cleaned $RUN"
