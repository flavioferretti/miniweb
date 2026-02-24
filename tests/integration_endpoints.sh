#!/usr/bin/env bash
# integration_endpoints.sh — smoke + payload contract tests for MiniWeb
# Covers: view pages, metrics, packages API, man API.
set -euo pipefail

PORT=19001
BASE="http://127.0.0.1:${PORT}"
PASS=0
FAIL=0

./build/miniweb -b 127.0.0.1 -p "${PORT}" >/tmp/miniweb_test.log 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true' EXIT
sleep 1

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

check_status() {
	local label="$1"
	local url="$2"
	local expected_status="${3:-200}"

	local actual
	actual=$(curl -o /dev/null -s -w '%{http_code}' "${url}")
	if [ "${actual}" = "${expected_status}" ]; then
		echo "  PASS  ${label}  (HTTP ${actual})"
		PASS=$((PASS + 1))
	else
		echo "  FAIL  ${label}  expected HTTP ${expected_status}, got ${actual}"
		FAIL=$((FAIL + 1))
	fi
}

check_json_key() {
	local label="$1"
	local url="$2"
	local key="$3"

	local body
	body=$(curl -fsS "${url}" 2>/dev/null || true)
	if echo "${body}" | grep -q "\"${key}\""; then
		echo "  PASS  ${label}  (key '${key}' present)"
		PASS=$((PASS + 1))
	else
		echo "  FAIL  ${label}  key '${key}' missing in: ${body:0:120}"
		FAIL=$((FAIL + 1))
	fi
}

check_text_nonempty() {
	local label="$1"
	local url="$2"

	local body
	body=$(curl -fsS "${url}" 2>/dev/null || true)
	if [ -n "${body}" ]; then
		echo "  PASS  ${label}  (non-empty response)"
		PASS=$((PASS + 1))
	else
		echo "  FAIL  ${label}  empty response"
		FAIL=$((FAIL + 1))
	fi
}

# ---------------------------------------------------------------------------
# View pages
# ---------------------------------------------------------------------------
echo "--- View pages ---"
check_status "GET /"          "${BASE}/"
check_status "GET /docs"      "${BASE}/docs"
check_status "GET /apiroot"   "${BASE}/apiroot"
check_status "GET /packages"  "${BASE}/packages"
check_status "GET /networking" "${BASE}/networking"
check_status "GET /missing"   "${BASE}/missing"   404

# ---------------------------------------------------------------------------
# Metrics API
# ---------------------------------------------------------------------------
echo "--- Metrics API ---"
check_status   "GET /api/metrics"          "${BASE}/api/metrics"
check_json_key "metrics has 'cpu'"         "${BASE}/api/metrics"  "cpu"

# ---------------------------------------------------------------------------
# Packages API
# ---------------------------------------------------------------------------
echo "--- Packages API ---"
check_status   "GET /api/packages/list"              "${BASE}/api/packages/list"
check_json_key "packages/list has 'packages'"        "${BASE}/api/packages/list"     "packages"

check_status   "GET /api/packages/search?q=curl"     "${BASE}/api/packages/search?q=curl"
check_json_key "packages/search has 'packages'"      "${BASE}/api/packages/search?q=curl" "packages"

check_status   "GET /api/packages/search (no q)"     "${BASE}/api/packages/search"   400

check_status   "GET /api/packages/info?name=curl"    "${BASE}/api/packages/info?name=curl"
check_json_key "packages/info has 'found'"           "${BASE}/api/packages/info?name=curl" "found"

check_status   "GET /api/packages/info (no name)"    "${BASE}/api/packages/info"     400

check_status   "GET /api/packages/which?path=/usr/bin/env" \
               "${BASE}/api/packages/which?path=/usr/bin/env"
check_json_key "packages/which has 'found'"          "${BASE}/api/packages/which?path=/usr/bin/env" "found"

check_status   "GET /api/packages/which (no path)"   "${BASE}/api/packages/which"    400

check_status   "GET /api/packages/files?name=curl"   "${BASE}/api/packages/files?name=curl"
check_json_key "packages/files has 'files'"          "${BASE}/api/packages/files?name=curl" "files"

check_status   "GET /api/packages/files (no name)"   "${BASE}/api/packages/files"    400

check_status   "GET /api/packages/unknown"           "${BASE}/api/packages/unknown"  404

# ---------------------------------------------------------------------------
# Man API
# ---------------------------------------------------------------------------
echo "--- Man API ---"
check_status   "GET /api/man/sections"               "${BASE}/api/man/sections"
check_json_key "man/sections has 'system'"           "${BASE}/api/man/sections"      "system"

check_status   "GET /api/man/pages?section=1"        "${BASE}/api/man/pages?section=1"
check_json_key "man/pages has 'pages'"               "${BASE}/api/man/pages?section=1" "pages"

check_status   "GET /api/man/pages (no section)"     "${BASE}/api/man/pages"         200
# returns {"error":"Missing section parameter"} — still 200, key present:
check_json_key "man/pages (no section) has 'error'"  "${BASE}/api/man/pages"         "error"

check_status   "GET /api/man/search?q=ls"            "${BASE}/api/man/search?q=ls"
check_text_nonempty "man/search?q=ls non-empty"      "${BASE}/api/man/search?q=ls"

check_status   "GET /api/man/resolve?name=ls"        "${BASE}/api/man/resolve?name=ls"
check_json_key "man/resolve has 'name'"              "${BASE}/api/man/resolve?name=ls" "name"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
if [ "${FAIL}" -gt 0 ]; then
	echo "Server log:"
	cat /tmp/miniweb_test.log | tail -20
	exit 1
fi
echo "integration_endpoints: ok"
