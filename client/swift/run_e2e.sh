#!/bin/bash
# run_e2e.sh - Sign in with Apple's iOS half end to end in the simulator, with no Apple ID: starts the
# fake Apple and a `cbase serve` that trusts it (ts/demo/serve.mjs, which serves the web demo too),
# generates the example's project, and runs its unit and UI tests against them. The UI test's
# screenshots land in $CB_SHOTS (default build/ios-shots).
#
#   c_base/client/swift/run_e2e.sh <simulator udid>
#
# Builds c_base's cbase and fake_apple first. Stops what it started, whatever the result.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
client=$(cd "$here/.." && pwd)
cbase=$(cd "$client/.." && pwd)
sim=${1:?usage: run_e2e.sh <simulator udid>}
shots=${CB_SHOTS:-$client/build/ios-shots}
mkdir -p "$shots" "$client/build"
make -s -C "$cbase" -j4 build/cbase build/test/fake_apple
log=$client/build/e2e-stack.log
node "$client/ts/demo/serve.mjs" "$cbase" "$client/build/demo" 0 > "$log" 2>&1 &
stack=$!
trap 'kill $stack 2>/dev/null || true' EXIT
n=0
until grep -q "^demo:" "$log" || [ $n -gt 60 ]; do sleep 0.5; n=$((n+1)); done
line=$(grep "^demo:" "$log") || { cat "$log"; echo "run_e2e: the stack did not start"; exit 1; }
server=$(echo "$line" | sed -E 's/.*server (http[^,]*),.*/\1/')
mint=$(echo "$line" | sed -E 's/.*mint (http[^)]*)\).*/\1/')
echo "run_e2e: server $server, fake Apple mint $mint"
cd "$here/Example"
xcodegen generate > /dev/null
TEST_RUNNER_CB_SERVER=$server TEST_RUNNER_CB_MINT=$mint TEST_RUNNER_CB_SHOTS=$shots \
  xcodebuild -project CbaseAppleExample.xcodeproj -scheme CbaseAppleExample -destination "id=$sim" \
  -derivedDataPath "$client/build/ios-dd" test 2>&1 | grep -E "Test Case .*(passed|failed)|error:|\*\* "
echo "run_e2e: screenshots in $shots"
