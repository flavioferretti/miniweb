#!/usr/bin/env bash
set -euo pipefail
./build/miniweb -b 127.0.0.1 -p 19001 >/tmp/miniweb_test.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 1
curl -fsS http://127.0.0.1:19001/ >/dev/null
curl -fsS http://127.0.0.1:19001/docs >/dev/null
curl -fsS http://127.0.0.1:19001/api/metrics >/dev/null
curl -fsS http://127.0.0.1:19001/apiroot >/dev/null
echo "integration_endpoints: ok"
