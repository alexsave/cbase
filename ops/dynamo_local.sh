#!/bin/bash
# Amazon's DynamoDB Local with the table ops/cbase.yaml defines, for the
# DynamoDB store's tests and the import rehearsal (DESIGN.md "Store").
#
#   c_base/ops/dynamo_local.sh            start (or reuse) it, create the table if missing
#   c_base/ops/dynamo_local.sh --reset    the same, and the table dropped and created empty
#   c_base/ops/dynamo_local.sh --stop     stop and remove the container (its data goes with it)
#   eval "$(c_base/ops/dynamo_local.sh)"  start it and export the env it prints
#
# Prints, on stdout, the environment the store and the AWS CLI need to reach
# it: AWS_ENDPOINT_URL_DYNAMODB=http://127.0.0.1:<port>, a region, and dummy
# credentials (DynamoDB Local accepts any non-empty pair). Progress goes to
# stderr.
#
# The table: ops/dynamo_table.py reads the template's AWS::DynamoDB::Table
# resource and writes the create-table, TTL and point-in-time-recovery input,
# so this table is the stack's table, never a second spelling of it.
#
# The container runs in memory with -sharedDb: without it DynamoDB Local keeps
# a separate database per access key and region, and a client whose dummy key
# differs from the CLI's sees no table at all. When something already answers
# on the port (CI's own container, say) it is used as it is.
#
# Settings, from the environment:
#   CBASE_DYNAMO_PORT   8000
#   CBASE_TABLE         cbase
#   AWS_REGION          us-east-1
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
name="cbase-dynamodb-local"
image="amazon/dynamodb-local:3.3.1@sha256:ff89bd48ff32cd8d9be5fee8873b65b8854dc408f1afe881be6eb00247bc0dab"
port="${CBASE_DYNAMO_PORT:-8000}"
table="${CBASE_TABLE:-cbase}"
region="${AWS_REGION:-us-east-1}"
endpoint="http://127.0.0.1:$port"
reset=no

die() { echo "dynamo_local.sh: $*" >&2; exit 1; }
say() { echo "dynamo_local.sh: $*" >&2; }

case "${1:-}" in
  "") ;;
  --reset) reset=yes ;;
  --stop)
    if docker ps -a --format '{{.Names}}' | grep -qx "$name"; then
      docker rm -f "$name" >/dev/null
      say "stopped $name"
    else
      say "$name is not running"
    fi
    exit 0 ;;
  -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
  *) die "unknown argument $1" ;;
esac

aws_bin="$(command -v aws || true)"
[ -n "$aws_bin" ] || aws_bin=/opt/homebrew/bin/aws
[ -x "$aws_bin" ] || die "the AWS CLI is not installed"
command -v python3 >/dev/null || die "python3 is not installed"

export AWS_ACCESS_KEY_ID=local AWS_SECRET_ACCESS_KEY=local AWS_REGION="$region" AWS_DEFAULT_REGION="$region"
export AWS_ENDPOINT_URL_DYNAMODB="$endpoint" AWS_PAGER="" AWS_EC2_METADATA_DISABLED=true
ddb() { "$aws_bin" dynamodb --endpoint-url "$endpoint" --output text "$@"; }
answers() { curl -s -o /dev/null --max-time 2 "$endpoint" 2>/dev/null; }

if ! answers; then
  command -v docker >/dev/null || die "nothing answers on $endpoint and docker is not installed"
  if docker ps -a --format '{{.Names}}' | grep -qx "$name"; then
    say "starting the existing $name"
    docker start "$name" >/dev/null
  else
    say "running $image as $name on port $port"
    docker run -d --name "$name" -p "127.0.0.1:$port:8000" "$image" -jar DynamoDBLocal.jar -inMemory -sharedDb >/dev/null
  fi
  for _ in $(seq 1 60); do
    answers && break
    sleep 0.5
  done
  answers || die "DynamoDB Local did not answer on $endpoint"
fi

exists() { ddb describe-table --table-name "$table" >/dev/null 2>&1; }
if [ "$reset" = yes ] && exists; then
  say "dropping table $table"
  ddb delete-table --table-name "$table" >/dev/null
fi
if ! exists; then
  input="$(mktemp -d)"
  trap 'rm -rf "$input"' EXIT
  python3 -I "$here/ops/dynamo_table.py" "$here/ops/cbase.yaml" "$table" "$input"
  say "creating table $table from ops/cbase.yaml"
  ddb create-table --cli-input-json "file://$input/create.json" >/dev/null
  ddb wait table-exists --table-name "$table"
  ddb update-time-to-live --cli-input-json "file://$input/ttl.json" >/dev/null
  # DynamoDB Local has no backups; the stack's table does. Recorded, not fatal.
  ddb update-continuous-backups --cli-input-json "file://$input/pitr.json" >/dev/null 2>&1 ||
    say "point-in-time recovery is not supported by DynamoDB Local (the stack enables it)"
fi

cat <<EOF
export AWS_ENDPOINT_URL_DYNAMODB=$endpoint
export AWS_REGION=$region
export AWS_DEFAULT_REGION=$region
export AWS_ACCESS_KEY_ID=local
export AWS_SECRET_ACCESS_KEY=local
export CBASE_STORE=dynamo
export CBASE_TABLE=$table
EOF
