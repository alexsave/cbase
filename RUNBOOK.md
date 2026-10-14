# c_base runbook: the first deploy to AWS

This puts c_base live at `https://api.foolish.cards`.
There are two ways, and they build the same stack:

- **The CLI path (do this one):** `aws configure` once, then one script, `c_base/ops/deploy.sh`, does the bucket, the build, the upload, the stack, and the import.
- **The console path (the fallback):** the same steps by hand in the AWS console, kept below for when the CLI is not an option.

Both share the account setup (step 1), the certificate's DNS record (step 3), the quota check (step 7), DNS (step 8), the health check (step 9), and everything after the import (steps 11 to 14).
Everything is in the region **US East (N. Virginia) us-east-1**.

## What I set up for you already

- `c_base/ops/cbase.yaml`: one CloudFormation template that creates the whole backend (DynamoDB table, Lambda function, Function URL, WebSocket API, the one-minute bot scan, CloudFront at your domain, and an optional deploy role for GitHub Actions).
  It passes `cfn-lint`; nothing in it has been run against AWS yet.
- `c_base/ops/deploy.sh`: the CLI path, end to end (it has been checked with `bash -n` and shellcheck, never run against AWS: there were no credentials).
- `c_base/ops/package.sh`: builds `c_base/build/cbase-lambda.zip`, the static arm64 Lambda binary, and prints its sha256.
  The same zip is built by CI on every change and kept as the `cbase-lambda` artifact of the `c_base` workflow run, so Docker on your Mac is optional.
- `.github/workflows/c_base.yml`: builds and tests c_base on Linux and macOS, under ASan, lints the template, builds the zip, and on `main` deploys it to the function once the two deploy secrets exist (step 13).
- The admin console at `https://api.foolish.cards/admin` (the token from step 2).
- This runbook.

## What you must give me afterwards

Paste these back into the session so I can finish the verification:

1. The stack status (it should say `CREATE_COMPLETE`).
2. The stack's **Outputs** tab: `FunctionUrl`, `WebSocketUrl`, `DistributionDomain`, `TableName`, `FunctionName`, and `DeployRoleArn` if you made it.
3. The anon key (it is public: it ships inside the web bundle); on the CLI path it is `CBASE_ANON_KEY` in `c_base/ops/.env`.
4. Whether `https://api.foolish.cards/health` answers `ok cbase ...` (step 9).

Never paste the JWT secret or the admin token into a chat.

With those I run the latency bench against the live stack from this Mac (DESIGN.md, "The bench"); it needs only the public anon key:

```
make -C c_base bench && c_base/build/cbase-bench roundtrip --url https://api.foolish.cards --anon <AnonKey> --n 500 --concurrency 1
```

It signs up two throwaway accounts (`bench...@bench.invalid`) and plays real games against the function, so run it after step 9 and before the import (step 10), or delete those two accounts in the console afterwards.

## The CLI path

1. Do step 1 below (the account, the plan, the zero-spend budget).
2. Give the CLI credentials.
   The simplest: **IAM > Users > Create user** `alex-cli`, attach `AdministratorAccess`, then **Security credentials > Create access key > Command Line Interface**, and run:

   ```
   aws configure          # paste the two keys; region us-east-1; output json
   aws sts get-caller-identity
   ```

   The second command must print your account id.
   (With IAM Identity Center instead: `aws configure sso`, `aws sso login`, and `export AWS_PROFILE=<the profile>`.)
3. Request the certificate (CloudFront needs it in us-east-1):

   ```
   aws acm request-certificate --region us-east-1 --domain-name api.foolish.cards --validation-method DNS
   aws acm describe-certificate --region us-east-1 --certificate-arn <the ARN it printed> \
       --query 'Certificate.DomainValidationOptions[0].ResourceRecord'
   ```

   Add that CNAME in Google Cloud DNS exactly as step 3.6 to 3.8 below describe, and wait until `aws acm describe-certificate ... --query Certificate.Status` says `ISSUED`.
4. Put the ARN where the script reads it, and if `IAM > Identity providers` already lists `token.actions.githubusercontent.com`, say so:

   ```
   echo 'CBASE_CERT_ARN=arn:aws:acm:us-east-1:...' >> c_base/ops/.env
   echo 'CBASE_GITHUB_REPO=alexsave/foolish' >> c_base/ops/.env     # optional: the deploy role for step 13
   echo 'CBASE_GITHUB_OIDC=no' >> c_base/ops/.env                   # only if the provider already exists
   ```

   `c_base/ops/.env` is gitignored and kept at mode 0600; the script creates it and generates the three secrets into it on its first run (`openssl rand -hex 32`), and reuses them on every later run.
   Keep a copy in your password manager: losing the JWT secret signs everyone out, and the anon key is baked into the web and iOS builds.
5. Start Docker Desktop (the build runs in an arm64 Alpine container), then deploy:

   ```
   c_base/ops/deploy.sh
   ```

   It prints the account and region, builds and uploads the zip, creates the stack (5 to 15 minutes; CloudFront is the slow part), and prints the outputs table and what to do next.
   Running it again later deploys a new build: the zip's name carries its sha256, so CloudFormation sees new code.
6. Do steps 7, 8 and 9 below (quota, DNS, health check), and send me what "What you must give me afterwards" lists.
7. The import: export from Supabase (step 10.5 below), rehearse into SQLite (step 10.6), then:

   ```
   c_base/ops/deploy.sh --import c_base/migrate/out
   rm -rf c_base/migrate/out /tmp/cbase-rehearsal.sqlite
   ```

   The script hands `cbase-import` temporary credentials exported from the CLI for that one command; no access key is created for it.
   Do it back to back with step 11 at a quiet hour, as step 10 explains.
8. Continue with steps 11 to 14.

## The console path (the fallback)

The steps below do the same by hand, with no CLI.

## 1. The AWS account

1. Sign in at https://console.aws.amazon.com/ (or create the account there; a card is required even on the free plan).
2. Open **Billing and Cost Management > Free Tier** and note which plan the account is on.
   An account opened on or after 2025-07-15 is on the credit-based Free plan: note the credit balance and the plan end date shown there.
   An older account shows the legacy 12-month free tier; note whether it has expired.
3. Everything this stack uses is in an always-free allowance except the WebSocket API, which costs cents a month after its first free year.
   On the Free plan, some services are blocked until you upgrade; if a stack resource later fails with an error that mentions the plan, upgrade to the Paid plan (Billing > Free Tier > Upgrade plan), which keeps any remaining credits.
4. Set a spending alarm: **Billing and Cost Management > Budgets > Create budget > Use a template > Zero spend budget**, with your email.
   Two budgets are free.
   This is what catches a mistake: anything that starts billing, a DynamoDB table switched to on-demand or provisioned past the free units included, emails you once the month's actual spend passes one cent.
   After the stack is up, **Billing and Cost Management > Free Tier** lists DynamoDB's always-free offers (25 GB, 25 write capacity units, 25 read capacity units) with this month's usage against each; the stack provisions exactly 25 and 25 across the table and its two indexes, so both should read at or under 100%.
   If they read over, the indexes are billed outside the free units: tell me, and the split in `ops/cbase.yaml` moves.
5. Switch the region selector (top right) to **US East (N. Virginia) us-east-1**.

## 2. Generate the three secrets on your Mac

Run these in Terminal, one at a time, and save each result in your password manager under the name shown.

```
openssl rand -base64 32    # JwtSecret     (CBASE_JWT_SECRET)
openssl rand -base64 32    # AdminToken    (CBASE_ADMIN_TOKEN)
openssl rand -hex 32       # AnonKey       (CBASE_ANON_KEY)
```

The anon key is hex rather than base64 on purpose.
Realtime clients send it in a URL query string, where base64's `+` reads as a space, and the iOS build reads it from an `.xcconfig`, where `//` starts a comment.

## 3. The certificate for api.foolish.cards

CloudFront only takes certificates from us-east-1, whatever region the rest is in.

1. Open **Certificate Manager** (search "ACM" in the console search bar) and confirm the region is us-east-1.
2. Click **Request** > **Request a public certificate** > **Next**.
3. Fully qualified domain name: `api.foolish.cards`.
4. Validation method: **DNS validation**; key algorithm: RSA 2048.
   Click **Request**.
5. Open the new certificate (status **Pending validation**).
   Under **Domains** there is a CNAME name and a CNAME value; keep this tab open.
6. In another tab open the Google Cloud console > **Network services > Cloud DNS** > the `foolish.cards` zone > **Add standard** (or **Add record set**).
7. DNS name: the part of the ACM CNAME name before `.api.foolish.cards.`, followed by `.api`.
   For example, if ACM shows `_3f9c2a.api.foolish.cards.`, type `_3f9c2a.api` (Cloud DNS adds `.foolish.cards.` itself).
8. Resource record type: **CNAME**; TTL: 300; canonical name: the ACM CNAME value exactly, ending in `.acm-validations.aws.`.
   Click **Create**.
9. Leave the apex `foolish.cards` A record (Vercel) alone; nothing in this runbook touches it.
10. Wait until ACM shows the certificate as **Issued** (usually 5 to 30 minutes; refresh the page).
11. Copy the certificate's **ARN** (starts with `arn:aws:acm:us-east-1:`).
    You need it in step 6.

## 4. The code zip

Either of these gives you `cbase-lambda.zip`:

- Without Docker: GitHub > the repository > **Actions** > **c_base** > the latest green run on `main` (or on this branch) > **Artifacts** > `cbase-lambda`. Unzip the download; inside is `cbase-lambda.zip` and its `.sha256`.
- With Docker Desktop running: `c_base/ops/package.sh` from the repository root.
  It prints the path, the size and the sha256.

## 5. The S3 bucket for the code

CloudFormation cannot create a Lambda function of this runtime without its code, so the zip goes to S3 first.

1. Open **S3** > **Create bucket**.
2. Bucket type: General purpose.
   Name: `cbase-code-` followed by your 12-digit account id (top right menu shows it), for example `cbase-code-123456789012`.
3. Region: us-east-1.
   Leave **Block all public access** on and every other setting at its default.
   Click **Create bucket**.
4. Open the bucket > **Upload** > **Add files** > `cbase-lambda.zip` > **Upload**.
5. The object key must be exactly `cbase-lambda.zip`.

## 6. Create the stack

1. Before you start: open **IAM > Identity providers** and note whether `token.actions.githubusercontent.com` is already listed.
2. Open **CloudFormation** > **Create stack** > **With new resources (standard)**.
3. Prepare template: **Choose an existing template**; template source: **Upload a template file** > **Choose file** > `c_base/ops/cbase.yaml` > **Next**.
4. Stack name: `cbase`.
5. Parameters:
   - CodeBucket: the bucket name from step 5.
   - CodeKey: `cbase-lambda.zip` (the default).
   - JwtSecret, AnonKey, AdminToken: the three values from step 2.
   - DomainName: `api.foolish.cards` (the default).
   - CertificateArn: the ARN from step 3.
   - AllowedOrigins: `*` (the default, which is what the Supabase functions answered).
   - FunctionName: `cbase`; TableName: `cbase` (the defaults).
   - GitHubRepo: `alexsave/foolish` to create the deploy role for step 13, or empty to skip it.
   - CreateGitHubOidcProvider: `no` if step 6.1 found the provider already listed, else `yes`.
6. **Next**. On **Configure stack options** leave everything at its default and tick **I acknowledge that AWS CloudFormation might create IAM resources with custom names**. **Next**.
7. Review, then **Submit**.
8. Wait for the stack status to reach **CREATE_COMPLETE** (5 to 15 minutes; the CloudFront distribution is the slow part).
   The **Events** tab shows progress.
9. Open the **Outputs** tab and copy every value: you need `DistributionDomain` next, and the whole list goes back to me.

If the status becomes `ROLLBACK_COMPLETE`, open **Events**, find the first row whose status is `CREATE_FAILED`, and send me its **Status reason**.
Then delete the stack, and also delete the `cbase` table in **DynamoDB > Tables** if it is still there (the template keeps the table on delete so data is never lost by accident, which would make a second create fail on the name).

## 7. Raise the Lambda concurrency quota if it is low

1. Open **Service Quotas** > **AWS services** > **AWS Lambda** > **Concurrent executions**.
2. If the applied value is below 100 (new accounts sometimes start at 10), click **Request increase at account level** and ask for 1000.
   It is free and usually approved within a day.

## 8. Point api.foolish.cards at CloudFront

1. In Google Cloud DNS, in the `foolish.cards` zone, **Add standard**.
2. DNS name: `api`; type: **CNAME**; TTL: 300; canonical name: the `DistributionDomain` output followed by a dot, for example `d1a2b3c4d5e6f7.cloudfront.net.`. **Create**.

## 9. Check it answers

1. Open the `FunctionUrl` output with `health` appended (for example `https://abc123.lambda-url.us-east-1.on.aws/health`).
   It should read `ok cbase 0.1.0 mode=lambda uptime_s=...`.
2. After a few minutes for DNS, open `https://api.foolish.cards/health`.
   It should read the same.
3. Open `https://api.foolish.cards/admin`, paste the AdminToken, and check the Overview panel says mode `lambda` and store `dynamo`.

Send me the outputs now (see "What you must give me afterwards"); I verify the API and the realtime path before any players are moved.

## 10. Import the data from Supabase

The import runs on your Mac and writes straight into DynamoDB with a temporary access key that you delete right after.
GitHub Actions is not used for this on purpose: the export holds every password hash, and it must never become a CI artifact.

Players who sign up or move between the export and step 11 are lost, so do steps 10 and 11 back to back at a quiet hour.

1. Build the tools: `make -C c_base` (gives `c_base/build/cbase-import`).
2. Create the temporary key: **IAM > Users > Create user**, name `cbase-import`, no console access, **Next**.
3. **Attach policies directly** > **Create policy** > **JSON**, paste the following with your account id in place of `ACCOUNT`, name it `cbase-import`, create it, then attach it to the user and finish creating the user.

   ```
   {"Version": "2012-10-17", "Statement": [{"Effect": "Allow",
     "Action": ["dynamodb:GetItem", "dynamodb:PutItem", "dynamodb:UpdateItem", "dynamodb:DeleteItem",
                "dynamodb:Query", "dynamodb:BatchWriteItem", "dynamodb:ConditionCheckItem", "dynamodb:DescribeTable"],
     "Resource": ["arn:aws:dynamodb:us-east-1:ACCOUNT:table/cbase", "arn:aws:dynamodb:us-east-1:ACCOUNT:table/cbase/index/*"]}]}
   ```

4. Open the user > **Security credentials** > **Create access key** > **Command Line Interface (CLI)** > tick the confirmation > **Create access key**. Keep the page open.
5. Export from Supabase into `c_base/migrate/out/` with `c_base/migrate/export.sh`.
   It reads the Supabase CLI's token from the macOS keychain, so run `supabase login` first if `supabase projects list` does not work; every statement it sends is a read-only `SELECT` (`c_base/migrate/README.md`).
6. Rehearse into a throwaway SQLite file first, and read the counts it prints:

   ```
   CBASE_STORE=sqlite CBASE_DB=/tmp/cbase-rehearsal.sqlite c_base/build/cbase-import c_base/migrate/out
   ```

7. Then the real import, pasting the two values from step 10.4 into the same Terminal window:

   ```
   export AWS_REGION=us-east-1 AWS_ACCESS_KEY_ID='paste' AWS_SECRET_ACCESS_KEY='paste'
   CBASE_STORE=dynamo CBASE_TABLE=cbase c_base/build/cbase-import c_base/migrate/out
   ```

8. Check the counts it prints match the rehearsal.
9. Delete the key and the user: **IAM > Users > cbase-import > Delete**, and delete the `cbase-import` policy under **IAM > Policies**.
10. Delete the export: `rm -rf c_base/migrate/out /tmp/cbase-rehearsal.sqlite`, and close the Terminal window.
11. In the admin console, the Users, Games and Bots panels should now show the imported rows.

## 11. Move the website

The web build bakes these two values in at build time, and CI builds the site (Vercel no longer builds from Git), so a value change needs a CI rebuild.

1. Vercel > the foolish project > **Settings > Environment Variables**.
2. Edit `NEXT_PUBLIC_SUPABASE_URL` for **Production** to `https://api.foolish.cards`.
3. Edit `NEXT_PUBLIC_SUPABASE_KEY` for **Production** to the AnonKey from step 2.
4. Write down the old two values first: they are the rollback.
5. GitHub > **Actions** > **web** > the latest successful run on `main` > **Re-run all jobs**.
   Its `vercel pull` fetches the new values and the deploy ships them.
6. Open https://foolish.cards, sign in with an existing account, and play a move against a bot.
   The admin console's Logs panel shows the requests.

## 12. Move the iOS apps

1. Set the Release values wherever the Release build reads `SUPABASE_URL` and `SUPABASE_KEY` (`foolish/ios/Config/Release.xcconfig`, or the untracked `Local.xcconfig` that includes it):

   ```
   SUPABASE_URL = https:/$()/api.foolish.cards
   SUPABASE_KEY = <the AnonKey>
   ```

   The `$()` is required: in an `.xcconfig`, `//` starts a comment.
2. Bump the build number, archive, and upload a TestFlight build as usual; test online play on a device before releasing it.
3. Installs that do not update keep calling Supabase until they do.
   That is why step 14 waits 30 days.

## 13. GitHub deploys

With the deploy role from step 6 (GitHubRepo set), every push to `main` that changes c_base redeploys the function after the tests pass.

1. GitHub > the repository > **Settings > Secrets and variables > Actions > New repository secret**.
2. `CBASE_AWS_ROLE_ARN`: the `DeployRoleArn` output.
3. `CBASE_FUNCTION_NAME`: `cbase`.
4. Optional `CBASE_AWS_REGION`: only if the stack is not in us-east-1.
5. The role trusts only workflow runs on `refs/heads/main` of `alexsave/foolish`, and may only update this one function's code.
6. If you skipped GitHubRepo in step 6, create the role later by updating the stack: **CloudFormation > cbase > Update > Use existing template**, set GitHubRepo, and submit.

## 14. Retire Supabase, after 30 days

1. Keep the Supabase project running for 30 days after the TestFlight build of step 12 is released, so old iOS installs keep working.
2. A free Supabase project pauses itself after a week without traffic; if that happens inside the 30 days and old installs still matter, restore it from the Supabase dashboard.
3. After 30 days: Supabase dashboard > the project > **Project Settings > General > Pause project**.
4. Do not delete it until you are sure no rollback is needed; a paused project keeps its data.

## Rollback

The web is the only switch that matters on day one, and it flips back in minutes:

1. Vercel > **Settings > Environment Variables**: set `NEXT_PUBLIC_SUPABASE_URL` and `NEXT_PUBLIC_SUPABASE_KEY` (Production) back to the old values from step 11.4.
2. GitHub > **Actions** > **web** > the latest run on `main` > **Re-run all jobs**.
3. Anything players did on c_base since the switch stays in DynamoDB and is not copied back.
4. A bad c_base build is rolled back separately: **Lambda > cbase > Code > Upload from > .zip file** with an earlier `cbase-lambda.zip` (every CI run keeps one for 30 days).

## Costs, for reference

Lambda (1M requests and 400,000 GB-seconds a month), DynamoDB (25 GB, and 25 write and 25 read units of provisioned capacity, which the table and its indexes use exactly), the Function URL, EventBridge Scheduler, CloudFront and ACM are inside their always-free allowances at foolish's volume.
DynamoDB's point-in-time recovery is not: $0.20 per GB-month, about a tenth of a cent a month at our few MB.
The WebSocket API is free for the first 12 months, then $1 per million messages and $0.25 per million connection-minutes: cents a month.
The code zip in S3 is about 2 MB: well under a cent a month.
CloudWatch Logs keeps 30 days (set in the template) inside its 5 GB free allowance.

## Web hosting: foolish.cards and uttt.live off Vercel

The design is `docs/VERCEL.md`; the stack is `ops/web.yaml`; the deploy is `ops/web_deploy.sh`.
Everything up to the DNS flip is done: the stack `cbase-web` is live and both sites are uploaded and serving on their CloudFront names.
Until DNS moves nothing reaches it, so it is harmless.

What exists (us-east-1, account 487337210065):

| Thing | Value |
|---|---|
| Stack | `cbase-web` |
| Bucket | `cbase-web-487337210065` (prefixes `foolish/`, `utils/`, `uttt/`) |
| foolish distribution | `E1YC4MJXHIOBQD`, `dgow77nr7v5kf.cloudfront.net` (foolish.cards, www, utils) |
| uttt distribution | `E1PSTHCGSF35HM`, `d2hb0vxf10cj0w.cloudfront.net` (uttt.live, www) |
| foolish certificate | `arn:aws:acm:us-east-1:487337210065:certificate/42ecb64e-bf84-4372-9170-2e645119b12f` |
| uttt certificate | `arn:aws:acm:us-east-1:487337210065:certificate/6f0b38fc-6ed7-40b3-83ef-14347c9cb4ae` |

Two distributions, not one: a CloudFront flat-rate plan covers one distribution with at most one apex domain, and an account holds three Free plans, so api, foolish and uttt use exactly the three.

### W1. Validate the two certificates

Squarespace > Domains > foolish.cards (then uttt.live) > **DNS > DNS Settings > Add record**, type **CNAME**, one per row.
In Squarespace's Host field type the name WITHOUT the domain at its end (the first column below already has it cut).

| Domain | Host | Type | Data |
|---|---|---|---|
| foolish.cards | `_94dd294a48c86186e616d0b27ff953cb` | CNAME | `_49023923d4e291bfda886e04ed7e8969.wzccmgtwzk.acm-validations.aws.` |
| foolish.cards | `_1043bf13b4c9c7509aa150c4cf652ea4.www` | CNAME | `_7b65955f569b0ee08fb6d70cd92bda60.wzccmgtwzk.acm-validations.aws.` |
| foolish.cards | `_647d6b5b48d15624b07a1f54887974e5.utils` | CNAME | `_e29e73ddf64b9147f3550aa0f62662c9.wzccmgtwzk.acm-validations.aws.` |
| uttt.live | `_faca958838ac79f06a92911f68615c4c` | CNAME | `_da5d6f5a49462a73d3f1535041af4c67.wzccmgtwzk.acm-validations.aws.` |
| uttt.live | `_7f7b50a58f31999214fc4ad280da4a73.www` | CNAME | `_eca85018c66683829aebdad41c6a2b38.wzccmgtwzk.acm-validations.aws.` |

These records only prove ownership; they change nothing that is served, and they stay in place for good (ACM renews against them).
Check, until both say ISSUED (minutes to an hour):

```
aws acm describe-certificate --region us-east-1 --certificate-arn <arn> --query Certificate.Status
```

### W2. Put the names on the distributions

```
CBASE_WEB_FOOLISH_CERT=arn:aws:acm:us-east-1:487337210065:certificate/42ecb64e-bf84-4372-9170-2e645119b12f \
CBASE_WEB_UTTT_CERT=arn:aws:acm:us-east-1:487337210065:certificate/6f0b38fc-6ed7-40b3-83ef-14347c9cb4ae \
  c_base/ops/web_deploy.sh stack
```

CloudFront refuses an alias another distribution holds, but Vercel is not a CloudFront distribution, so this works while DNS still points at Vercel.
The distribution update takes a few minutes.

### W3. Put both distributions on the Free plan

CloudFront console > **Distributions** > the distribution > the pricing plan > **Free**, once for each of the two.
Plans are chosen only in the console; until then they run pay-as-you-go, which is also $0 at this volume.

### W4. Prove it before DNS moves

The certificate and Host are exactly what visitors will send; only the address is forced:

```
for p in / /dashboard /privacy /imessage-privacy /support /abc123 /.well-known/apple-app-site-association; do
  curl -sI --connect-to foolish.cards:443:dgow77nr7v5kf.cloudfront.net:443 "https://foolish.cards$p" | head -1
done
curl -sI --connect-to foolish.cards:443:dgow77nr7v5kf.cloudfront.net:443 https://foolish.cards/.well-known/apple-app-site-association | grep -i content-type   # application/json
curl -sI -H 'Accept-Encoding: gzip, br' --connect-to foolish.cards:443:dgow77nr7v5kf.cloudfront.net:443 https://foolish.cards/oracle.wasm.gz | grep -ci content-encoding   # 0
curl -s --connect-to foolish.cards:443:dgow77nr7v5kf.cloudfront.net:443 https://foolish.cards/m/1x | grep -o '<title>[^<]*'
curl -sI --connect-to uttt.live:443:d2hb0vxf10cj0w.cloudfront.net:443 https://uttt.live/234 | grep -i location   # /243
```

Today, before any of this, Vercel answers `https://foolish.cards/...` with a 307 to `www`, and serves the association file there as `application/octet-stream`.
Apple fetches the association file at the apex without following redirects, so universal links are broken on Vercel today; the apex flip below fixes that.

### W5. The DNS flip, in this order

A day ahead, lower the TTL of every record you are about to change to 300 (Squarespace > DNS Settings, each record's TTL).
DNSSEC must be off for an ALIAS at `@` (it is: `dig +short DS foolish.cards` prints nothing).

1. **uttt.live**, the rehearsal: replace the `@` A record (76.76.21.21) with an **ALIAS** `@` to `d2hb0vxf10cj0w.cloudfront.net`, and the `www` CNAME with `d2hb0vxf10cj0w.cloudfront.net`.
   Test: `dig +short uttt.live A` and `dig +short uttt.live AAAA` return CloudFront addresses (not 76.76.21.21), `curl -sI https://uttt.live/` says `server: CloudFront`, and a replay link from the app plays.
   If Squarespace will not take the ALIAS, stop here and use the fallback below before touching foolish.cards.
2. **www.foolish.cards**: the `www` CNAME (today `d204b352f329121a.vercel-dns-017.com`) becomes `dgow77nr7v5kf.cloudfront.net`.
   Test: `curl -sI https://www.foolish.cards/` says `server: CloudFront`; sign in and play a move against a bot.
3. **foolish.cards**: the `@` A record (216.198.79.193) becomes an **ALIAS** `@` to `dgow77nr7v5kf.cloudfront.net`.
   Test: `dig +short foolish.cards A` and `AAAA` return CloudFront addresses, `curl -sI https://foolish.cards/.well-known/apple-app-site-association` is a 200 with `application/json` and no redirect, then `cd foolish && npm run test:validate` (universal_links_validation) and a link tapped on a phone opens the app.
4. **utils.foolish.cards**, whenever: it is a different site (the sibling repo `foolish-utils`), so first build it there with `output: 'export'` and `images.unoptimized`, upload with `c_base/ops/web_deploy.sh site utils --out ../foolish-utils/out` (the right content types, cache headers and invalidation), check it with `curl --connect-to` as in W4, then point the `utils` CNAME (today `1a8ccd91b52ad608.vercel-dns-017.com`) at `dgow77nr7v5kf.cloudfront.net`.

Fallback if the apex ALIAS does not work: a Route 53 hosted zone ($0.50 a month; ALIAS queries to CloudFront are free).
Create the zone for the domain, recreate EVERY record from Squarespace in it (the ACM CNAMEs too), add the apex as an A and AAAA alias to the distribution, then change the domain's nameservers at Squarespace to the zone's four.
Not an apex-to-www forward: the iOS app's entitlement is `applinks:foolish.cards` only, and Apple does not follow redirects for the association file.

### W6. Two weeks in parallel

`web.yml` and `uttt-web.yml` keep deploying every push to Vercel (their builds set `VERCEL=1`, which keeps the server build), and `web_aws.yml` deploys the same commit to S3, so either host serves the current site.
Watch for two weeks; the c_base admin console's Logs panel shows the `/m` unfurl requests coming through the new distribution.

### W7. Rollback

DNS only, and the 300-second TTL makes it minutes:

- foolish.cards: `@` back to an A record 216.198.79.193; `www` back to CNAME `d204b352f329121a.vercel-dns-017.com`.
- uttt.live: `@` back to an A record 76.76.21.21; `www` back to CNAME `e0da883ddeb2a161.vercel-dns-017.com`.
- utils: CNAME back to `1a8ccd91b52ad608.vercel-dns-017.com`.

A bad web deploy on AWS without a DNS rollback: re-run `web_aws` on the last good commit (Actions > web_aws > that run > Re-run all jobs), or restore objects from the bucket's versions (kept 30 days).

### W8. GitHub deploys for the web

1. Create the role: `CBASE_WEB_GITHUB_REPO=alexsave/foolish CBASE_WEB_GITHUB_OIDC=yes c_base/ops/web_deploy.sh stack`.
   An account holds one GitHub OIDC provider: say `CBASE_WEB_GITHUB_OIDC=yes` only if IAM > Identity providers does not list `token.actions.githubusercontent.com` yet (today it does not), and then step 13's stack update must say CreateGitHubOidcProvider `no`.
2. GitHub > Settings > Secrets and variables > Actions: `CBASE_WEB_AWS_ROLE_ARN` = the cbase-web stack's `DeployRoleArn` output, and `CBASE_ANON_KEY` = the AnonKey from step 2 (`c_base/ops/.env`).
3. The role may upload to the bucket, invalidate the two distributions, and update the stack's two page-list parameters on its existing template; a template change is always `web_deploy.sh stack` from this Mac.
4. Now and then, `c_base/ops/web_deploy.sh site foolish --prune-static` (and `uttt`) deletes hashed files no current page uses; a tab left open across two deploys reloads.
5. The Infinite Oracle's Mode B headers: `CBASE_WEB_ISOLATION=yes c_base/ops/web_deploy.sh stack` (and `no` to undo).

### W9. Retire Vercel, after the two weeks

1. Delete the `scope` and `deploy` jobs from `web.yml`, the `deploy` job from `uttt-web.yml`, `scripts/web_deploy_scope.sh` and `e2e/validation/web_deploy_scope_validation.test.ts` (they ration Vercel's 100 deploys a day), and the `VERCEL` halves of `foolish/next.config.mjs` and `uttt/web/next.config.mjs`.
2. Delete the repository secrets `VERCEL_TOKEN`, `VERCEL_ORG_ID`, `VERCEL_PROJECT_ID` and `UTTT_VERCEL_PROJECT_ID`.
3. Vercel dashboard: remove the domains from both projects, then delete the two projects (and the foolish-utils one once utils has moved).
