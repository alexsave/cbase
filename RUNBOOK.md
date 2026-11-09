# Running a cbase product

How to run a product built on cbase, on each provider: `local` (`cbase serve`, on your machine or on one small VM) and `aws` (Lambda, DynamoDB, IoT Core and CloudFront).
Each provider ships its own templates and scripts; this guide strings them together.
The values that belong to one product's deployment are placeholders here:

| placeholder | what it is |
|---|---|
| `<product>` | the product's Makefile directory, the one that includes `cbase.mk` (`CBASE_PRODUCT`) |
| `<domain>` | the API's public name, for example `api.example.com` |
| `<account>`, `<region>` | the AWS account id and region; the templates assume `us-east-1` |
| `<stack>` | the CloudFormation stack: `cbase` for the aws provider, `cbase-vm` for the VM (each overridable) |
| `<bucket>` | the code bucket, `cbase-code-<account>-<region>` by default |

Every script below prints its usage from its header, which is the full reference: read it before running the script.
Paths are from the cbase root; a product that has cbase as a submodule runs them as `cbase/<path>`.

## 1. Before either provider

Build and test the product the way CI does: `make check` (and `make check-asan`) in `<product>`.
The deploy scripts refuse a tree that does not contain `origin/main` (`ops/require_main.sh`), so what ships is always a commit of main, never a branch that forked before a fix.
In a product, the repository checked is the product's (the superproject), since that is what is deployed.

### The ops settings file

The deploy scripts read their settings and secrets from one file outside the repository, through one reader (`ops/ops_env.sh`):

1. `$CBASE_OPS_ENV` when it is set;
2. else `~/.config/cbase/ops.env`;
3. else `ops/.env` (gitignored), when it is the only one there.

It is mode 0600 and holds only `CBASE_*` `NAME=value` lines; a value already in the environment wins over the file.
It lives outside the repository so that removing a worktree can never take the only copy of a secret.

The rule that matters: a secret is generated only for a stack that does not exist yet.
Against a live stack a fresh `CBASE_JWT_SECRET` would sign every user out and a fresh admin token would lock the console, so a missing secret there stops the run and says where to recover it from.
`ops/test/ops_env_test.sh` holds these rules.

## 2. The local provider on your machine

`cbase serve` is one process over one SQLite file; the README's quick start runs it.
Settings are `CBASE_*` variables (`core/src/config.h`; a start logs one summary line, and every setting with secrets redacted under `CBASE_LOG_LEVEL=debug`), or a file named by `CBASE_ENV_FILE` (docs/PLATFORM.md 2.10).
Outside `CBASE_DEV=1` it needs `CBASE_JWT_SECRET` (at least 32 characters) and `CBASE_PUBLIC_URL`, unless the app is storeless (docs/PLATFORM.md 2.7).
`CBASE_DB` is the database file (default `./cbase.sqlite`); a second `cbase serve` on the same file refuses to start.
`GET /health` is liveness and `GET /ready` readiness, when the app declares them (docs/PLATFORM.md 2.9).
Ctrl-C drains like SIGTERM: requests in flight finish, then the app's `serve_close` runs.

Backups: with `CBASE_BACKUP_BUCKET` (and `CBASE_BACKUP_REGION`) set, serve copies the database to S3 when it changed, every `CBASE_BACKUP_INTERVAL_S` (default 60) under `CBASE_BACKUP_KEY`, and restores the newest copy at start when the database file is missing (`providers/local/src/store/backup.h`).

## 3. The local provider on a VM

`providers/local/ops/vm.yaml` is `cbase serve` on one EC2 t4g.nano: an IPv6-only VPC (no public IPv4 address, nothing to scan), an auto-scaling group of exactly one instance that claims a fixed address at boot, a security group that admits the serve port only from CloudFront, SSM instead of SSH, a versioned backup bucket, two alarms, and a CloudFront distribution in front (`StagingUrl`).
[providers/local/ops/VM_RUNBOOK.md](providers/local/ops/VM_RUNBOOK.md) is the detailed record: the boot chain, why IPv6-only, backups and restores, OS updates and cost.

### Create the stack

1. Build the static binary: `make serve-binary` in `<product>` (an arm64 Alpine container, so Docker is needed) gives `dist/cbase-serve-linux-arm64` and its `.sha256`.
2. Deploy, with the product's two names:

   ```
   CBASE_PUBLIC_URL=https://<domain> CBASE_VM_ORIGIN_DOMAIN=vm-origin.<your zone> \
     providers/local/ops/vm_deploy.sh dist/cbase-serve-linux-arm64
   ```

   It uploads the binary and the boot script to `<bucket>/vm/`, makes the VM's own secrets once as SSM SecureStrings under `/cbase-vm/env/` (never regenerated, never printed), and creates or updates the stack.
   It never reads the ops settings file: the VM keeps its secrets in SSM.
3. Point an AAAA record for `vm-origin.<your zone>` at the stack's `OriginIpv6` output.
   CloudFront reaches an origin by name, not address, and the fixed address keeps the record valid across instance replacements.
4. Check it: `providers/local/ops/vm_deploy.sh --verify` (the service is active, the binary's sha256, the instance's own `/health` and `/ready`, and `<PublicUrl>/health`), all through SSM Run Command, with no inbound port.

The stack's own distribution (`StagingUrl`) caches nothing and adds the origin secret header; to serve `<domain>`, put a distribution that holds `<domain>`'s certificate in front of the same origin name.

### Ship a new build

```
providers/local/ops/vm_release.sh dist/cbase-serve-linux-arm64    # upload, point at it, restart, verify
providers/local/ops/vm_release.sh --restart                       # run this tree's boot script again on the release named
providers/local/ops/vm_release.sh --verify                        # the checks only
```

`vm_release.sh` changes the code only, never the stack: it points `/cbase-vm/release/serve` at the new binary and runs the instance's boot stub through SSM, which fetches and checks it and restarts the service.
A boot-script or unit change ships the same way.
A restart drops open WebSockets; clients reconnect.
After the checks pass it prunes the bucket, keeping the new release and the one before it.

Rollback: run `vm_release.sh` on an older build of main, or point `/cbase-vm/release/serve` at an older `vm/cbase-serve-*` key and run `--restart`.
`ops/s3_prune.sh --dry-run vm` shows what a prune would delete.

### Day to day

- Shell: `aws ssm start-session --target <instance-id>` (needs the Session Manager plugin); the server's log is `journalctl -u cbase`.
- Alarms: subscribe an address to the `AlarmTopic` output (`aws sns subscribe ... --protocol email`).
- OS updates: the boot runs no package manager, so fixes arrive with new images: run `vm_deploy.sh` (it picks up the newest AL2023 image), then an instance refresh with `MinHealthyPercentage` 0, since the old instance holds the fixed address.
  A replacement restores the newest backup at start.
- Scale to zero: `CBASE_VM_DESIRED=0 providers/local/ops/vm_deploy.sh` (the instance and its disk go; the VPC, buckets and parameters stay).

## 4. The aws provider

`providers/aws/ops/cbase.yaml` is the whole backend in one stack: the DynamoDB table (provisioned inside the always-free 25 read and 25 write units, split 15/5/5 over the table and its two indexes, point-in-time recovery on, TTL on `expires_at`), the Lambda function (the static arm64 `bootstrap`, 256 MB, 60 s, reserved concurrency 30), its Function URL, realtime over IoT Core when `IotEndpoint` is set, an EventBridge schedule, CloudFront at `<domain>`, ten CloudWatch alarms and their SNS topic.
CloudFront sends `/mqtt` to IoT Core and everything else to the Function URL, so clients see one origin.

The template still carries two choices of the product it was first written for: one schedule, named `"scan"`, invoked every minute, and two cached public paths, `/v1/leaderboard` and `/v1/bots`.
An app with other schedules or cached paths edits those resources until the template takes them from the app.

### The first deploy

1. Give the AWS CLI credentials (`aws configure`, or `aws sso login` and `AWS_PROFILE`), and check that `aws sts get-caller-identity` prints `<account>`.
2. Request the certificate in us-east-1 (CloudFront reads certificates only there), add the CNAME it asks for at your DNS host, and wait for `ISSUED`:

   ```
   aws acm request-certificate --region us-east-1 --domain-name <domain> --validation-method DNS
   aws acm describe-certificate --region us-east-1 --certificate-arn <arn> --query Certificate.Status
   ```

3. Put the settings in the ops settings file: `CBASE_DOMAIN=<domain>`, `CBASE_CERT_ARN=<arn>`, and optionally `CBASE_ALARM_EMAIL`, `CBASE_ALLOWED_ORIGINS`, `CBASE_STACK`, `CBASE_TABLE`, `CBASE_FUNCTION_NAME`, `CBASE_CODE_BUCKET` and `CBASE_IOT` (the header of `providers/aws/ops/deploy.sh` lists every one and its default).
4. Start Docker (the build runs in an arm64 Alpine container), then:

   ```
   CBASE_PRODUCT=<product> providers/aws/ops/deploy.sh
   ```

   It creates the code bucket if missing, generates `CBASE_JWT_SECRET` and `CBASE_ADMIN_TOKEN` into the settings file for a new stack and writes them to SSM Parameter Store as SecureStrings (the function reads them at container start, never from its environment), builds and uploads the zip under a name carrying its sha256, runs a change set (refused when it would replace a resource, unless `CBASE_ALLOW_REPLACE=1`), and prints the outputs: `FunctionUrl`, `DistributionDomain`, `DistributionId`, `TableName`, `PublicUrl`.
   Keep a copy of the settings file in a password manager: losing the JWT secret signs everyone out.
5. Point a CNAME for `<domain>` at `DistributionDomain`.
6. Check that `https://<domain>/health` answers `ok cbase <version> mode=lambda ...`, and open `https://<domain>/admin` with the admin token when the app declares the console.

`deploy.sh` is also how a template change ships, and running it again with a new build deploys that build.
A product's own steps against the table after every deploy (seeding its rows, an import with `--import DIR`) are its `CBASE_DEPLOY_AFTER` command, run with temporary credentials, the stack's table and its distribution.

### Closing the origin

The Function URL and IoT Core's endpoint each have a public AWS hostname.
CloudFront adds `X-Cbase-Origin: <CBASE_ORIGIN_SECRET>` to everything it sends to either, and the function answers anything without it 403 before it reads the store (`core/src/config.h`); at IoT Core the custom authorizer refuses a connection without it.
Adding the secret, or rotating it, is two deploys, because the function changes at once and the distribution takes minutes:

```
CBASE_ORIGIN_ENFORCED=no providers/aws/ops/deploy.sh   # CloudFront starts sending it; nothing checks it yet
aws cloudfront wait distribution-deployed --id <DistributionId>
providers/aws/ops/deploy.sh                            # now the function refuses requests without it
```

### Ship new code

`providers/aws/ops/lambda_deploy.sh <product>/build/cbase-lambda.zip` ships a built zip (`providers/aws/ops/package.sh` makes it) without touching the template: it updates the stack on its previous template with only the code key changed, checks the function's code hash and `<PublicUrl>/health`, and prunes the bucket to this zip and the one before it.
Rollback is the same command on an older zip, or a stack update with the old `CodeKey`.

### Day to day

- Logs: every invocation ends with one `inv:` line (`providers/aws/src/main_lambda.c`): the status and time, `store=` (DynamoDB), `push=` (IoT Core) and `other=` (Lambda invoke, CloudFront, SSM), each as calls/ms/new connections.
  In CloudWatch Logs Insights on the function's log group:

  ```
  fields @timestamp, @message
  | filter @message like /inv: http/
  | parse @message /inv: http (?<method>\S+) (?<path>\S+) (?<status>\d+) ms=(?<ms>[\d.]+) after=(?<after>[\d.]+) store=(?<store_calls>\d+)\/(?<store_ms>[\d.]+)\/(?<store_dials>\d+) push=(?<push_calls>\d+)\/(?<push_ms>[\d.]+)/
  | stats count(*) as n, pct(ms, 50) as p50, pct(ms, 95) as p95, avg(store_calls) as store_calls by method, path
  | sort n desc
  ```

- Alarms: Lambda throttles, errors and concurrency, Function URL 5xx, invocations past 6000 in 5 minutes, the table's and each index's consumed units, and refused writes, mailed to `CBASE_ALARM_EMAIL` once its confirmation link is clicked.
- Lost secrets: read them back from SSM (`aws ssm get-parameter --name <SecretsPrefix>jwt-secret --with-decryption`), never generate new ones; the origin secret is the distribution's `X-Cbase-Origin` header value.
- The DynamoDB table survives a stack delete on purpose (the template retains it), so a second create with the same name needs the old table removed first.

### Cost

At a small product's volume Lambda, DynamoDB's provisioned 25 and 25 units, the Function URL, EventBridge Scheduler, CloudFront, ACM, ten alarms and SNS email are inside their always-free allowances.
Point-in-time recovery is not: $0.20 per GB-month of table, a fraction of a cent for a few MB.
IoT Core bills per million connection-minutes and messages after its first free year: cents a month.
Nothing checks the capacity split against the free allowance, so a change to it is checked by hand: the six numbers add up to at most 25 a side, the billing mode stays PROVISIONED, and no auto scaling (it would scale past 25 and bill).
`providers/aws/test/unit_dynamo.c` does hold the template's index names, key types and TTL attribute to the store's.

## 5. Deploying from CI

cbase's own CI (`.github/workflows/ci.yml`) builds and tests, and deploys nothing.
A product deploys from its own workflow with its own role, through GitHub's OIDC provider: on each push to main it builds the binary and runs `vm_release.sh` (local) or `lambda_deploy.sh` (aws).
`providers/aws/ops/role_covers_release.sh SCRIPT ROLE.yaml` fails when the script makes an AWS call the product's role template does not grant, so the two cannot drift; template changes stay hand steps with an operator's credentials.
Give the deploying workflow one concurrency group per ref with `cancel-in-progress: false` on main: `require_main.sh` lets the first attempt of a run on main deploy its own commit even when main has moved on (the run queued behind it deploys the newer one), and refuses a re-run, which would roll main back.
