#!/usr/bin/env bash
# run.sh - the HTTP/2 interop check (make h2-interop): tools/h2_interop/server.js on Node's
# node:http2, called by build/h2_interop through cb_grpc_unary, cleartext and TLS.
#
# Opt in: it needs node (NODE, default the one on PATH) and the openssl CLI, which the local
# build does not. The certificate is made fresh each run, for localhost, under build/h2-interop-run.
# Both listeners take free ports; the server is stopped by its PID whatever happens.
#
# usage: tools/h2_interop/run.sh <h2_interop binary>
set -euo pipefail

DRIVER="$1"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NODE="${NODE:-node}"
OUT="$ROOT/build/h2-interop-run"
command -v "$NODE" > /dev/null || { echo "h2-interop: no node (set NODE)"; exit 1; }
command -v openssl > /dev/null || { echo "h2-interop: no openssl CLI"; exit 1; }
mkdir -p "$OUT"
rm -f "$OUT/ports" "$OUT/ports.new"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -days 2 -nodes \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost \
  -keyout "$OUT/key.pem" -out "$OUT/cert.pem" > /dev/null 2>&1

"$NODE" "$ROOT/tools/h2_interop/server.js" "$OUT/cert.pem" "$OUT/key.pem" "$OUT/ports" > "$OUT/server.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2> /dev/null || true; wait "$pid" 2> /dev/null || true' EXIT

n=0
until [ -s "$OUT/ports" ] || [ $n -gt 300 ]; do
  kill -0 "$pid" 2> /dev/null || { cat "$OUT/server.log"; echo "h2-interop: the server exited"; exit 1; }
  sleep 0.1; n=$((n + 1))
done
[ -s "$OUT/ports" ] || { cat "$OUT/server.log"; echo "h2-interop: the server never listened"; exit 1; }
read -r clear tls < "$OUT/ports"
echo "h2-interop: node $("$NODE" --version) on ports $clear (cleartext) and $tls (TLS)"
"$DRIVER" "$clear" "$tls" "$OUT/cert.pem"
