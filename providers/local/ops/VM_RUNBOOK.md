# cbase-vm runbook

## State (2026-10-19)

The stack `cbase-vm` is live in us-east-1 (us-east-1a), running `cbase serve` from commit `b7dd1ace0` (sha256 `07be1f2d...e08c3`).
It takes no production traffic: no CloudFront distribution or DNS record points at it.
Verified live through SSM Run Command:

- `systemctl is-active cbase` is `active`, and the local `GET /health` and `GET /ready` answer 200.
- A request to `http://[<OriginIpv6>]:8080/v1/bots` without `X-Cbase-Origin` from a non-loopback address gets 403.
- The instance has no IPv4 address; the SSM agent is Online over IPv6 and S3 dual-stack and `ssm.<region>.api.aws` served the boot.
- The instance holds the fixed `OriginIpv6` (`::10`) and the alarms and group are as in vm.yaml.

What the offline plan got wrong, and the fix:

- Auto Scaling refuses a launch template that names an address (`Ipv6Addresses`), so `vm_boot.sh` claims `::10` itself with `ec2:AssignIpv6Addresses`, retrying while EC2 frees it from a replaced instance.
- `UseDualStackEndpoint` alone left the agent's credential check on `ssm.<region>.amazonaws.com` (IPv4 only), so the agent config names the `api.aws` endpoints.
- `ec2messages` has no IPv6 name, so the agent logs `MDSInteractor ... GetMessages Error` every few minutes; Run Command and sessions go through `ssmmessages` and work. The errors are noise.

Step 3 blocker: `foolish.cards` is served by Google Cloud DNS (`ns-cloud-d1.googledomains.com`), not Route 53, so the AAAA record for the origin must be added there.

## What runs where

- One VPC with an Amazon-provided IPv6 /56 and one IPv6-only subnet, an internet gateway and a `::/0` route.
- An auto-scaling group of exactly one instance (AL2023 minimal arm64, t4g.nano, 2 GB gp3, IMDSv2 over IPv6).
- The instance claims the subnet's `::10` address at boot (`vm_boot.sh`), so a replacement keeps the address an AAAA record will name.
- A security group that admits the serve port (8080) only from CloudFront's IPv6 origin-facing prefix list; there is no SSH.
- The instance role: `AmazonSSMManagedInstanceCore`, `s3:GetObject` on `<CodeBucket>/vm/*`, read/write on the backup bucket, read on `/cbase-vm/*` parameters.
- SSM parameters under `/cbase-vm/`: `release/serve` and `release/agent` (`<bucket> <key> <sha256>`), and `env/CBASE_*` (the server's environment).
- The VM's secrets (`CBASE_JWT_SECRET`, `CBASE_ADMIN_TOKEN`, `CBASE_ORIGIN_SECRET`) are SecureStrings that `vm_deploy.sh` made once for this VM only.
- They are not the live stack's secrets, and `vm_deploy.sh` never reads `~/.config/cbase/ops.env`.
- A backup bucket (versioned, public access blocked, SSE-S3, TLS only, noncurrent versions expire after 30 days, kept when the stack is deleted).
- Alarms on `StatusCheckFailed` and on `CPUCreditBalance` below 20, to the SNS topic `cbase-vm-alarms`, which has no subscription yet.

## Boot (no dnf)

`vm_boot.sh` is the user-data, gzipped and spliced with `cbase.service` by `vm_deploy.sh`.
It gets the role's credentials from IMDSv2 at `[fd00:ec2::254]` and signs its own requests with `curl --aws-sigv4`.
It reads every `/cbase-vm/` parameter with one `GetParametersByPath` call to `ssm.<region>.api.aws`.
It installs the SSM agent from the rpm pinned in `release/agent` (the minimal AMI ships none), with `UseDualStackEndpoint` on and IMDS in IPv6 mode.
It downloads the binary from `release/serve` over the S3 dual-stack endpoint and checks its sha256 before moving it into place.
It writes `/etc/cbase/cbase.env` (root, 0600), the unit, and restarts `cbase`.
It installs itself as `/usr/local/sbin/cbase-vm-boot`, so running it again is the upgrade.
The service runs as the system user `cbase` with its data in `/var/lib/cbase`, and listens on `:::8080` (dual-stack socket).
Its log is `journalctl -u cbase`; the boot script's log is `/var/log/cbase-vm-boot.log`.

## Why IPv6-only

An IPv6-only subnet has no public IPv4 address, which saves about $3.60 a month and leaves nothing to scan on IPv4.
Every dependency was checked offline (DNS, release notes, AWS docs) before relying on it, and the live instance confirmed it (see "State"):

- SSM: `ssm.us-east-1.api.aws` and `ssmmessages.us-east-1.api.aws` have AAAA records, and SSM Agent 3.3.3270.0 added `UseDualStackEndpoint` (its release notes).
- `ec2messages` has no dual-stack name, but agents since 3.3.40.0 use `ssmmessages` in its place.
- S3: `s3.dualstack.us-east-1.amazonaws.com` has AAAA records.
- IMDS: Nitro instances (t4g is one) answer at `fd00:ec2::254` once `HttpProtocolIpv6` is enabled.
- CloudFront: it supports IPv6 origins for custom origins, and the managed prefix list `com.amazonaws.global.ipv6.cloudfront.origin-facing` exists.

The trade: CloudFront needs a domain name for an origin, not an address.
Step 3 needs an AAAA record (for example `vm-origin.foolish.cards`) pointing at the stack output `OriginIpv6`, and the origin's IP address type set to IPv6.
The fixed `::10` address is what keeps that record valid across instance replacements.

## Deploy

```sh
c_base/bench/nano/build.sh vm1                       # static musl arm64 -> c_base/bench/nano/bin/serve-vm1
c_base/providers/local/ops/vm_deploy.sh              # upload as vm/cbase-serve-<short sha>, create or update the stack
c_base/providers/local/ops/vm_deploy.sh --verify     # systemctl is-active, local /health and /ready, journal tail
```

The first boot takes about two minutes after the instance launches.
`--verify` uses SSM Run Command, so it works with no inbound port at all.

## Upgrade the server

Build a new binary, commit (the version is the short git SHA), and run `vm_deploy.sh` to upload it and move `/cbase-vm/release/serve`.
Then run `vm_deploy.sh --restart`: the instance re-reads SSM, fetches and checks the new binary, and restarts `cbase`.
A restart drops open WebSockets; clients reconnect.

## Upgrade the OS

The image is the SSM public parameter for the latest AL2023 minimal arm64, resolved at each stack update.
Run `vm_deploy.sh` (it picks up the newest image into a new launch template version), then replace the instance:

```sh
aws autoscaling start-instance-refresh --auto-scaling-group-name cbase-vm \
  --preferences '{"MinHealthyPercentage":0,"InstanceWarmup":120}'
```

`MinHealthyPercentage` must be 0: the old instance holds `::10` and has to go before the new one can take it.
The SQLite data lives on the instance's root volume, so a replacement starts with an empty database until backups exist.

## Backups (not built yet)

The backup bucket exists and the role can read and write it, but the server does not use it yet.
The plan is a C job in `cbase serve`: `VACUUM INTO` a snapshot, upload it to the bucket when it changed, and restore the newest one at boot.
Until that lands, instance replacement loses the data, so do not refresh or terminate an instance that holds data anyone needs.

## Shell access

```sh
aws ssm start-session --target <instance-id>
```

This needs the Session Manager plugin for the AWS CLI on the Mac.

## Alarms

Subscribe an address when one is confirmed:

```sh
aws sns subscribe --topic-arn <AlarmTopic output> --protocol email --notification-endpoint <address>
```

The instance uses standard (not unlimited) CPU credits, so it cannot run up a surplus-credit bill; the credit alarm says when it is throttled to its 5% baseline.

## Scale to zero

```sh
CBASE_VM_DESIRED=0 c_base/providers/local/ops/vm_deploy.sh
```

This terminates the instance (and its data) but keeps the VPC, buckets, parameters and alarms, which cost nothing at rest.
`CBASE_VM_DESIRED=1` brings it back with a fresh instance.

## Delete the stack

```sh
aws cloudformation delete-stack --stack-name cbase-vm
aws cloudformation wait stack-delete-complete --stack-name cbase-vm
```

The backup bucket is retained on purpose; empty and delete it by hand when it is truly unwanted.
The SecureStrings were made outside the stack, so delete them too: `aws ssm delete-parameters --names /cbase-vm/env/CBASE_JWT_SECRET /cbase-vm/env/CBASE_ADMIN_TOKEN /cbase-vm/env/CBASE_ORIGIN_SECRET`.
The releases under `s3://<CodeBucket>/vm/` stay until removed.

## OS updates

The VM picks up OS fixes only through new AMIs, because the boot script runs no `dnf` and AL2023 repos are locked to the AMI's release.
The procedure is a monthly ASG instance refresh, which replaces the instance with one launched from the latest AL2023 AMI.
Schedule: the first Monday of each month, and at once when a security advisory names a package on the VM.
This is a schedule note only; no AWS resource carries it.
The rig image is rebuilt the same way: `c_base/bench/nano/build_images.sh` pulls a fresh amazonlinux:2023 and upgrades with `--releasever=latest`.

## Cost

About $3.07 a month for the t4g.nano on demand, plus $0.16 for the 2 GB gp3 volume.
No public IPv4, no NAT gateway, no load balancer.
Two CloudWatch alarms are inside the free tier (10 alarms).
