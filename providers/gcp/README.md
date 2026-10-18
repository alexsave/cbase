# c_base provider: Google Cloud (gcp)

## Status

This is an unbuilt slot.
Nothing in this directory compiles.
Build it only when wanted, against the conformance suites (store, push, ingress, job), with the Firestore emulator as its CI lane.
local stays the reference provider and enforces the tightest limits, so a gcp build that passes the suites inherits the contract without new rules.
The provider is chosen at link time, like aws and local.

## Mapping

### Store: Firestore native mode

- Item identity: one document per item, with the document id encoding pk and sk.
  Encode as `<pk>\x1f<sk>` percent-escaped, or a fixed-length hash of both plus the raw pk and sk kept as fields.
  Ids are capped at 1500 bytes and may not contain `/`, so the escape is mandatory.
- CAS: the version tag is the document's `updateTime`.
  A conditional put or update sends an `updateTime` precondition (or `exists: false` for create-only).
  The opaque version tag the interface hands out wraps that timestamp.
- Conditions beyond a version match: read inside a transaction, evaluate in core's condition code, then write.
  Firestore has no server-side condition expressions.
- Transactions: up to 500 documents per commit, which is at or above the store interface's limit, so local's tighter cap stays binding.
- GSIs: two overloaded GSIs become two composite indexes over (`gsi1pk`, `gsi1sk`) and (`gsi2pk`, `gsi2sk`), declared in Terraform.
  Query maps to a structured query with an equality on the partition field and a range on the sort field.
- Cursor: a wrapped last-document snapshot (path plus the ordering fields), opaque to callers.
- Batch get: `BatchGetDocuments`.
- TTL: a TTL policy on one timestamp field.
  Deletion is lazy (typically within about a day), so reads must filter expired items, exactly as with DynamoDB TTL.
- String lists: native arrays.
- Main risk: Firestore documents sustain roughly 1 write per second each (a soft limit, bursts above it contend and fail).
  A bot drive that hammers one table document would hit it.
  Mitigation: keep the per-table hot item small, batch a bot's turns into one write, and add a write-rate case to the store conformance suite so the emulator lane cannot hide it.
  Note that the emulator does not enforce this limit, so it needs a live-project smoke.

### Push: MQTT over WebSocket, served by the Cloud Run process

- IoT Core was retired in 2023, so gcp serves MQTT itself, through core's hub (the same hub `cbase serve` runs in-process for local).
- With max-instances=1 the hub is the whole broker and needs no relay.
- Past one instance, publish to Pub/Sub (one topic, one subscription per instance) and have each instance replay into its own hub.
- Cloud Run supports WebSockets with a request timeout up to 60 minutes, so clients must reconnect on close, which the client already does.

### Ingress: Cloud Run

- One container runs core's process ingress, shared with any future Cloud Run main.
- Same routes, same netwire bytes as every other provider.

### Jobs and schedules: Cloud Scheduler plus Cloud Tasks

- `scan`: one Cloud Scheduler job, HTTP target, authenticated with OIDC to the Cloud Run service.
- `bot_drive:<id>`: a Cloud Task per id, named for the id so a duplicate enqueue is rejected (task names dedupe for about an hour after execution), with `scheduleTime` for delays.
  The job interface's cancel maps to a task delete.

### Edge purge

- Cloud CDN invalidation, or keep CloudFront in front of Cloud Run and let the purge hook stay as in aws.
  Prefer keeping CloudFront until a reason appears, since Cloud CDN has no free tier.

### Secrets

- Secret Manager, read at startup through the Cloud Run secret-as-env-var binding.
  The `ops.env` equivalent is one secret per key.

### Deploy

- Terraform.
  `terraform plan` is the zero-replacement gate the deploy contract requires: CI fails if the plan shows any `replace` or `delete` of a stateful resource (Firestore database, indexes, secrets).
- CI authenticates from GitHub with Workload Identity Federation (a pool plus an OIDC provider bound to the repo and branch), the analogue of the AWS OIDC role.
  No long-lived service account keys.

## Free-tier fit

Figures are as last known and must be re-checked before relying on them.
Sources: [Firestore quotas](https://firebase.google.com/docs/firestore/quotas), [Cloud Run pricing](https://cloud.google.com/run/pricing), [Cloud Scheduler pricing](https://cloud.google.com/scheduler/pricing), [Cloud Tasks pricing](https://cloud.google.com/tasks/pricing), [Secret Manager pricing](https://cloud.google.com/secret-manager/pricing).

| Service | Free quota | Status |
| --- | --- | --- |
| Firestore | 1 GiB stored, 50k reads, 20k writes, 20k deletes per day, one free database per project | recalled, unverified this session |
| Cloud Run | request-based: 2M requests, 180k vCPU-seconds, 360k GiB-seconds per month | recalled, unverified |
| Cloud Scheduler | 3 free jobs per billing account | recalled, unverified |
| Cloud Tasks | first 1M operations per month | recalled, unverified |
| Secret Manager | 6 active versions, 10k access operations per month | recalled, unverified |
| Cloud CDN | none | recalled, unverified |

Verdict for foolish's measured load (about 1.2k requests/day, peak 10 concurrent):

- Requests: 36k a month against 2M. Fits easily.
- Firestore: 1.2k requests at a handful of reads and writes each is far under 50k reads and 20k writes a day. Fits, unless bot drive multiplies writes (each bot move is a write plus a task).
- Scheduler: `scan` is one job. Fits.
- The open question is MQTT sockets.
  A WebSocket is a long request, and CPU is billed while it is open.
  One vCPU held for 50 hours exhausts 180k vCPU-seconds, so sockets held around the clock would leave the free tier, while bursty play (10 concurrent for an hour or two a day) would not.
  Instance-based billing has its own free allotment, unverified here.
- Conclusion: $0 is plausible at this load but not guaranteed.
  Measure socket-hours from the aws deployment's connect logs before committing.

## What the client never sees

Providers adapt to the client protocol, never the reverse.
A client cannot tell gcp from aws or local: same routes, same netwire bytes, push delivered as MQTT over WebSocket.
No Firestore, Pub/Sub, or Cloud Run concept (document ids, updateTime, region, instance count) crosses the wire.
A gcp-only client branch or a gcp-only route is a bug in the provider, not a feature request.

## Build checklist

Mirror providers/aws's shape (src/ for the C, ops/ for deploy), one file per aws counterpart.

- `src/main_cloudrun.c` (aws: `src/main_lambda.c`): the Cloud Run entry, calling core's process ingress.
- `src/provider_gcp.c` (aws: `src/provider_aws.c`): the provider descriptor, naming the store, push and job backends.
- `src/store/store_firestore.c` (aws: `src/store/store_dynamo.c`): Firestore client, doc-id codec for pk+sk, updateTime CAS, transactions, cursor.
- `src/realtime/rt_pubsub.c` (aws: `src/realtime/rt_iot.c`): core's MQTT hub in-process, plus the Pub/Sub relay past one instance.
- `src/gcp/` (aws: `src/aws/`): the HTTPS client, Google auth (metadata-server tokens, the analogue of SigV4), Cloud Tasks and Scheduler (jobs `scan` and `bot_drive:<id>`, aws: `lambda_invoke.c`), Secret Manager (aws: `ssm.c`), Cloud CDN purge if used (aws: `cloudfront.c`).
- `src/gcp_json.c` (aws: `src/aws_json.c`): every JSON shape the Google APIs force, contained in this one file under the same gate.
- `providers/gcp/Makefile` or `.mk`: link-time selection, same variable as aws and local.
- `providers/gcp/terraform/`: Firestore database, composite indexes, TTL policy, Cloud Run service, Scheduler job, Tasks queue, Secret Manager, Workload Identity pool, plus a CI step that fails on any replace or delete in `terraform plan`.
- `providers/gcp/Dockerfile` for the Cloud Run image.
- CI lanes: store, push, ingress and job conformance against the Firestore emulator; a live-project smoke for the write-rate limit and socket-hours; the plan gate.
