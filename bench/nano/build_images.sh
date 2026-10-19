#!/bin/bash
# Build nano-perf:al2023 and nano-perf:perf from a freshly pulled amazonlinux:2023,
# then check no security update is pending (empty output means current).
set -euo pipefail
cd "$(dirname "$0")"
docker build --pull --no-cache --target base -t nano-perf:al2023 -f Dockerfile.al2023 .
docker build --no-cache --target perf -t nano-perf:perf -f Dockerfile.al2023 .
docker run --rm nano-perf:al2023 dnf -q check-update --releasever=latest --security || [ $? -eq 100 ]
