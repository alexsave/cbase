# Moving foolish.cards off Vercel (2026-10-14)

> **Since then (2026-10-17):** the move is done.
> foolish.cards, www, utils.foolish.cards and uttt.live are all served from S3 behind CloudFront (`providers/aws/ops/site/web.yaml`), and `.github/workflows/web_aws.yml` builds and deploys both sites on every push to `main`.
> The `/m` unfurl is the C route `foolish/server/src/app/unfurl_http.c` (DESIGN.md, "Unfurl route"), and `foolish/src/app/m/` is gone.
> `web.yml` and `uttt-web.yml` remain only as workflow_dispatch Vercel rollbacks until about 2026-10-28 (RUNBOOK.md, "W9").
> What follows is the design as written and its as-built record (section 7); the present state is RUNBOOK.md, "Web hosting".

## 1. The one-line answer

It does not save money: Vercel Hobby is $0 today and the AWS shape is $0 too.
What it buys is one account and one bill, no deploy or upload quotas (both bit on 2026-09-18), no non-commercial clause, and the north-star gain: the last Next server code, the `/m/<payload>` link unfurl, moves into C and the web becomes purely static client code.
Recommendation: do it, but after the Supabase cutover is finished and stable, and rehearse on uttt.live first (section 6).

## 2. What Vercel is underneath, and what we use of it

Vercel Functions run on AWS Lambda; Vercel's own engineering post describes the tunnel to Lambda instances that became Fluid compute (https://vercel.com/blog/fluid-how-we-built-serverless-servers).
Its region codes map to AWS regions (iad1 is us-east-1), and the CDN is Vercel's own PoP network in front of those regions (https://vercel.com/docs/fundamentals/what-is-compute).
So it is not a thin reseller: the value it adds is zero-config Next.js (SSR, ISR, middleware, image optimization), previews and a dashboard.

We use almost none of that value:

| Vercel feature | Used | How |
|---|---|---|
| Static hosting + CDN | yes | the whole site; `_next/static/**` immutable, HTML and `public/` revalidated |
| Server rendering | one route | `foolish/src/app/m/[payload]/layout.tsx` `generateMetadata`, for link unfurls |
| Web Analytics | yes | `@vercel/analytics` in the root layout; page views only, which MONETIZATION_ROADMAP calls "not enough" |
| Env vars | yes | `vercel pull` exists only to fetch `NEXT_PUBLIC_BACKEND_URL` |
| Prebuilt CLI deploy | yes | `.github/workflows/web.yml`: CI builds, then `vercel deploy --prebuilt --archive=tgz [--prod]` |
| Previews | opt-in | a `preview` label, gated by `scripts/web_deploy_scope.sh` to save the 100 deploys a day |
| Git integration, builds | no | disconnected on purpose: Vercel's image has no libclang for structgen |
| Middleware, route handlers, ISR, Edge Config, KV, Blob, cron, `next/image` | no | none exist in the app |

Every other page is `'use client'`.
The root layout is a static server component (metadata, Google Fonts, the analytics tag), which a static export renders at build time.

## 3. The cost

Sources read 2026-10-14: https://vercel.com/docs/plans/hobby, https://vercel.com/docs/limits, https://vercel.com/docs/limits/fair-use-guidelines, https://vercel.com/pricing, https://aws.amazon.com/cloudfront/pricing/, https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/flat-rate-pricing-plan.html, https://aws.amazon.com/s3/pricing/, https://aws.amazon.com/amplify/pricing/, https://aws.amazon.com/route53/pricing/.
Our load: a few MB of static files, tens of users, well under 5 GB of egress a month.

| Option | Monthly | Allowance | Catches |
|---|---|---|---|
| Vercel Hobby (today) | $0 | 100 GB transfer, 1M requests, 1M invocations, 50k analytics events | 100 deploys a day and 5,000 API file uploads a day per account; non-commercial use only; over a cap, features pause about 30 days, no way to pay |
| Vercel Pro | $20 per developer seat, $20 usage credit | 1 TB transfer, 6,000 deploys a day | pure cost for this app; still a second account and bill |
| S3 + CloudFront Free flat-rate plan | $0 ($0.50 more only if we need a Route 53 zone) | 1M requests, 100 GB, 5 GB S3 storage credit, ACM cert, CloudFront Functions | never bills; sustained excess degrades delivery; no standard logging; one apex per plan, at most 3 free plans per account |
| S3 + CloudFront pay-as-you-go | $0 at our volume | always-free 1 TB and 10M requests, then $0.085/GB | a metered bill is possible; logging allowed |
| Amplify Hosting (Next SSR) | $0 for 12 months, then about $1 to $3 | 15 GB served, 1,000 build minutes, 12 months only | $0.15/GB served; Next up to 15 only; no on-demand ISR, streaming or edge middleware |

S3 at a few MB is under a cent a month, inside the Free plan's storage credit.
Invalidations: the first 1,000 paths a month are free and `/*` counts as one.
The `/m` route runs on the existing Lambda, inside its always-free 1M requests.
Amplify is the wrong tool: it exists to run Next on a server, and the point of moving is to stop needing one.

## 4. The design if we move

### (a) The `/m/<payload>` unfurl becomes a C route in c_base

Crawlers (Messages, Slack, WhatsApp) run no JavaScript, so `/m/<payload>` must answer real HTML with the tags in the head.
Today the TS route percent-decodes and trims the payload, and if it starts with `1` (the text-level format version) it calls three wasm exports; each is a thin wrapper over kernel C that c_base already links:

| TS call | wasm export | kernel C |
|---|---|---|
| `kernelB32Decode(text.slice(1))` | `wasm_replay_b32_decode` (`foolish/c/wasm/wasm_msg_api.c`) | `b32_decode` (`shared/c/b32.h`), which stops at `-` |
| `kernelMsgDecode(bytes)` | `wasm_msg_decode` (`foolish/c/wasm/wasm_api.c`) | `msg_decode` then `msg_replay` into a `Game` (`c/src/msg_wire.h`) |
| reads `env.joins[].name`, `nPlayers`, `phase`, `carryKey`, `turn` | `wasm_msg_header_ptr` | the `MsgEnvelope` fields `joins`, `n_players`, `phase`, `carry_key`, `turn` |

`msg_wire.c` and `replay.c` are already in the kernel's core list that `c_base/Makefile` compiles, so the route adds no kernel source.
The C route must call `msg_replay` too, not only `msg_decode`: the TS route falls back to the generic tags when the replay fails, and the golden test below would catch the difference.

The route, in `foolish/server/src/app/` next to the other GET handlers, answers `GET /m/<payload>` with a small HTML page:

- `<title>` and `description`: "A vs B - turn N", "A vs B - a finished Durak game" (phase 3), "A vs B - a rematch, waiting to start" (phase 0 with a carry key), names joined with " vs " when two or more are non-empty, else "N players"; the description "A Durak game in iMessage. Foolish turns a message thread into a card table."; and on any failure the fallback title "A Durak game in iMessage" with the bare blurb as description.
- `og:title`, `og:description`, `og:type` website, `og:site_name` Foolish, `twitter:card` summary, `twitter:title`, `twitter:description`, and `robots` noindex, follow.
- The client redirect to `/` that `m/[payload]/page.tsx` does today with `router.replace('/')`, as one inline `location.replace('/')` plus a `<noscript>` meta refresh.
- Every name HTML-escaped (`& < > " '`): nicknames are self-reported, and Next escaped them for us.
- `Cache-Control: public, max-age=86400`: the payload is the content, so one crawler fetch per link reaches Lambda.

The test is a unit test, `foolish/server/test/unit_unfurl.c`, against goldens taken from the TS route's current output before it is deleted: run `next build` and `next start` once, fetch a fixed set of payloads (a two-seat game mid-turn, a four-seat game, a finished game, a rematch lobby, a game with one named seat, a name holding `<` and `"`, a truncated payload, a payload with a share-link `-extras` suffix, a non-`1` prefix, garbage), and record each page's tag set as (name or property, content) pairs.
The C output is compared as the same pair set, not byte for byte, because Next's surrounding markup is not part of the contract.
The fixtures are kernel-made envelopes, not hand-written bytes, and the test is mutation-checked like every other.

Then `foolish/src/app/m/` is deleted outright, and with it the only server-side wasm load in the web app.

### (b) The Next app becomes `output: 'export'`

| Blocker | Answer |
|---|---|
| `rewrites()`: `/privacy`, `/imessage-privacy`, `/support` to `.html` files in `public/` | the CloudFront Function below maps the clean URLs (the App Store records point at them) |
| `headers()`: COOP/COEP when `FOOLISH_CROSS_ORIGIN_ISOLATION=1` (off by default) | a response-headers policy on the default behaviour, attached only when a template parameter asks for it |
| `m/[payload]` dynamic segment | leaves with the route, (a) |
| `[game_id]` root catch-all for replay codes and game ids, no `generateStaticParams` | export one shell (a placeholder `generateStaticParams` entry), and the client reads the code from `location.pathname` instead of the route param; the Function serves that shell for any extensionless path that is not a known page |
| `generateMetadata` on the dynamic route | leaves with the route |
| `not-found.tsx` | exports `404.html`; with the Function's fallback it is reached only for missing files with an extension |

Watch the known gotcha: a stray `next.config.js` shadows `next.config.mjs`.

The SPA fallback is a CloudFront Function, not custom error responses.
Custom error responses are distribution-wide: they would turn a missing JS chunk into a 200 of HTML and would also rewrite error answers from the `/m` origin.
The Function (viewer request, default behaviour only) does three things: an extensionless path that is a known page gets `.html` appended, any other extensionless path becomes the game shell, and a path with an extension passes through.
The known-page list is written into the Function's code by the deploy from the export's own `out/*.html`, so nobody maintains it by hand.
It is JavaScript because CloudFront Functions accept nothing else, tolerated for the same reason as the existing `/realtime/v1/websocket` rewrite in `providers/aws/ops/cbase.yaml`.

### (c) A second CloudFront distribution, not more behaviours on the API's

A new stack `providers/aws/ops/site/web.yaml`, separate from `cbase.yaml`, with its own distribution for `foolish.cards` and `www.foolish.cards`.
Why separate:

- The API distribution's default behaviour is the Function URL with caching disabled; the web's default is S3 with caching on, so sharing one would make every path a special case in one direction or the other.
- Separate lifecycles: a web deploy then cannot touch the backend stack, and the web's CI role gets S3 and invalidation rights only.
- Each Free plan carries one distribution and its own 1M requests and 100 GB.
- Same-origin API calls would be the one argument for sharing, and the clients already call `api.foolish.cards` cross-origin with CORS working.

The cost of separate: api, foolish.cards and uttt.live use exactly the three free plans an account may hold, so a fourth site would have to go pay-as-you-go (also $0 at this volume).

The pieces:

- ACM certificate in us-east-1 for `foolish.cards` and `www.foolish.cards`, DNS-validated by a `_xxx` CNAME at Squarespace.
- A private S3 bucket with Origin Access Control; no website endpoint, no public access.
- A second origin: the c_base Function URL (a stack output today), with a `/m/*` behaviour pointing at it, `Managed-AllViewerExceptHostHeader` for origin requests and `Managed-CachingOptimized`, which honours the route's own `Cache-Control`.
- Default behaviour to S3 with `Managed-CachingOptimized` and the Function above; compression on (CloudFront compresses HTML, JS and CSS, which S3 never does).
- Cache lifetime per path comes from the objects' own `Cache-Control`, set at upload (d), so only managed cache policies are needed; that sidesteps the Free plan's exclusion of custom caching rules.
- `/.well-known/apple-app-site-association` uploaded with `Content-Type: application/json` (S3 would otherwise say `binary/octet-stream`) and served at the apex with no redirect, since the app's entitlement is `applinks:foolish.cards` only.
- `oracle.wasm.gz`, `oracle-mt.wasm.gz` and the bundled `bots.wasm.gz` and `web.wasm.gz` under `_next/static/media/` are uploaded with `Content-Type: application/octet-stream` and no `Content-Encoding`: the client fetches and gunzips them itself, and a `Content-Encoding: gzip` would make the browser unzip them first and the client fail.
  Verify with `curl -sI -H 'Accept-Encoding: gzip, br'` that no `Content-Encoding` comes back.

### (d) The deploy

`web.yml` keeps its build job as it is (structgen, wasm, `next build`, now producing `out/`) and replaces `vercel pull/build/deploy` with:

1. Assume a deploy role through GitHub OIDC; no stored keys.
2. `aws s3 sync out/_next/static s3://<bucket>/_next/static` with `Cache-Control: public, max-age=31536000, immutable`, and without `--delete`: a tab still holding the old HTML asks for the old chunks, so stale hashed files are pruned later by a lifecycle rule (say 30 days), not at deploy.
3. The `.wasm.gz` files and the association file in their own passes with the content types in (c).
4. Everything else (HTML, `public/`) with `Cache-Control: no-cache` and `--delete`, last, so no HTML points at a chunk that has not landed yet.
5. Publish the Function with the regenerated page list.
6. One `aws cloudfront create-invalidation --paths '/*'`: one path, free.

The two `NEXT_PUBLIC_` values become GitHub Actions variables (by then they are `https://api.foolish.cards` and its key), and `vercel pull` goes.
There is no deploy count and no upload count, so `scripts/web_deploy_scope.sh`, the `scope` job and `e2e/validation/web_deploy_scope_validation.test.ts`, which exist only to ration Vercel's 100 deploys a day, can be deleted.
Previews: not built by default.
If wanted later, a PR deploys to `s3://<bucket>/_pr/<n>/` and a host-matching rule in the Function serves `pr-<n>.preview.foolish.cards` from it, which needs a wildcard certificate and DNS record; the opt-in label shows how rarely they were used.

### (e) The apex

Squarespace's DNS page lists ALIAS records at `@` with a hostname target, provided DNSSEC is off (https://support.squarespace.com/hc/en-us/articles/31119879125645-DNS-records-for-web-hosting); one forum report has it working against a CloudFront domain, an older one says the UI lacked it.
So the first step is a test, before any cutover:

1. Prove the distribution with the real certificate and host before DNS moves: `curl -sI --connect-to foolish.cards:443:dXXXX.cloudfront.net:443 https://foolish.cards/` and the same for `/m/<payload>`, `/privacy` and the association file.
2. Lower the apex and www TTLs to 300 a day ahead.
3. Add `@` ALIAS to `dXXXX.cloudfront.net` and `www` CNAME to the same; check `dig +short foolish.cards A` returns CloudFront addresses (not 216.198.79.193) and `dig +short foolish.cards AAAA` too.

Fallbacks, in order:

- A Route 53 hosted zone ($0.50 a month, alias queries to CloudFront free), which means moving the nameservers and recreating every record, MX included.
- Not the apex-to-www forward: the iOS app associates `foolish.cards` only, Apple fetches the association file without following redirects, and a forward would break universal links in every shipped build.
- Not CloudFront Anycast static IPs: about $36.50 per IP a month, and unavailable on the flat-rate plans.

### (f) uttt.live

The same shape and simpler: no server code at all.
`napkin.png/route.ts` and `about` are `force-static` and export as files; `[code]` needs the same shell fallback; the `/privacy-msg` and `/support-msg` rewrites and the `/234` to `/243` redirect (a 301 from the Function) move into its Function; `uttt.wasm` and `uttt243.wasm` upload as `application/wasm` with `max-age=3600`.
Its own distribution and Free plan, its own bucket or a prefix in the same one, and `uttt-web.yml` gets the same sync steps.
Where uttt.live's DNS lives, and whether its registrar does ALIAS, is not in the repo: check before starting.

### (g) Analytics

Vercel's analytics counted page views, and the roadmap already judged that not enough.
CloudFront standard logs to S3 would replace it, but the Free plan excludes logging; it would mean pay-as-you-go plus an Athena or script step to read them.
Recommendation: nothing.
The questions that matter (who plays, how often, how many games) are answered better by the backend, which now holds every account and game in DynamoDB.
Deleting `@vercel/analytics` also takes a third-party script out of every page load.

## 5. What is lost and what is gained

Lost:

- Preview URLs per PR (rarely used; rebuildable as in (d)).
- Vercel's page-view analytics (replaced by nothing, deliberately).
- The dashboard's one-click rollback and atomic deploys; the deploy's ordering gives most of the atomicity, and a rollback is re-running the deploy job on an older commit (or S3 versioning).
- Runtime log search for the one function; the `/m` route logs to the c_base CloudWatch group instead.
- Zero-config Next server features, none of which the app uses, and which become unavailable rather than merely unused, which is the point.

Gained:

- One account, one bill, one CloudFront estate, one deploy model.
- No deploy or upload quota that can lock production out for 24 hours.
- No non-commercial clause hanging over a site that promotes an App Store product.
- The last server TypeScript in the web app is gone, the `/m` unfurl is kernel C with a golden test, and three files of deploy-rationing machinery go with Vercel.

Rollback: keep the Vercel project and its deploy job running in parallel for two weeks after cutover, so it holds the current build.
Rolling back is DNS only: apex `A` back to 216.198.79.193 and `www` back to its Vercel CNAME, with the 300 second TTL already in place.
Only after the two weeks does the Vercel job leave `web.yml`.

## 6. Effort, risk and recommendation

Effort: the C route and its golden test about a day; the static export, the Function, `providers/aws/ops/site/web.yaml` and the deploy steps about a day; the cutover and its checks half a day; uttt.live half a day.
Risks, highest first: the Squarespace apex ALIAS (tested before anything moves, Route 53 behind it); universal links (the association file at the apex, held by `e2e/validation/universal_links_validation.test.ts`, run against the live host after cutover); the shell fallback missing a page (the page list is generated, and the e2e suite runs against the CloudFront host before DNS moves); and unfurl caches at Apple and Slack, which keep the old card for a while but never break.

Recommendation: move, for the account, the quotas and the north star, not for money.
Order:

1. Finish the Supabase to c_base cutover first and let it run stable; it changes the web's two env values and the same domain, and two migrations at once make every failure ambiguous.
2. Build the `/m` C route with its goldens while the cutover settles; it ships behind no DNS change, reachable only through the Function URL.
3. Rehearse the whole hosting move on uttt.live, which has no server route and no universal links.
4. Then foolish.cards: stack, test with `--connect-to`, ALIAS, two weeks in parallel, retire Vercel.

`utils.foolish.cards` (a CNAME to Vercel) is Alex's personal toys site from the sibling repo `../foolish-utils`, deployed by Vercel's git integration: Next 16 with only client-side pages, no API routes, no env vars and no backend, so it is static in substance.
It can move whenever, with `output: 'export'` plus `images.unoptimized` (it uses `next/image`), into its own S3 prefix or bucket behind the same web distribution, with `utils.foolish.cards` added to the certificate and the aliases and the Function choosing the prefix by Host.
Nothing in the monorepo depends on it, and it is not part of the foolish move's order.

## 7. As built (2026-10-14)

Built ahead of the cutover, up to "ready to flip"; c_base/RUNBOOK.md "Web hosting" has the state and the steps.
Where it differs from sections 4 and 6, and why:

- Two distributions in one stack (`providers/aws/ops/site/web.yaml`), foolish (with www and utils) and uttt: a flat-rate plan covers one apex, and each distribution has its own copy of the same viewer-request Function, because a Function attached to a plan's distribution cannot be shared. `providers/aws/ops/site/test/web_router.test.mjs` runs that code out of the template and fails when the copies drift.
- The page list lives in two stack parameters substituted into the Function, written by `providers/aws/ops/site/web_deploy.sh` from each export on a stack update with the previous template, so the template stays the Function's one owner and CI can never apply a template change.
- The COOP/COEP headers come from a viewer-response Function, not a response-headers policy: a Free plan allows only managed headers policies.
- The SPA shell is `/_shell.html` (the one `generateStaticParams` entry); the clients read the segment from the address bar on it (`foolish/src/utils/useUrlGameId.ts`, `uttt/web/components/Replay.tsx`).
- Stale hashed chunks are pruned by hand (`web_deploy.sh site <s> --prune-static`), not by a lifecycle rule: an object's age is its first upload, so an age rule would delete live chunks. The bucket is versioned instead, old versions kept 30 days.
- Both apps still build as a server app when `VERCEL=1`, so the Vercel lanes keep serving the same site during the overlap; on Vercel `/m/:payload` is rewritten to `api.foolish.cards`.
- Found on the way: Vercel today answers the apex with a 307 to www and serves the association file as `application/octet-stream`, so universal links are broken now; the apex on CloudFront serves it at 200 as `application/json`.
