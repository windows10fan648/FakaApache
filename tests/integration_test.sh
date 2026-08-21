#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BINARY="${REPO_ROOT}/fakaapache"
PORT="18080"
BASE_URL="http://127.0.0.1:${PORT}"
TEMP_DIR="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

fail() {
    echo "TEST FAILED: $*" >&2
    exit 1
}

assert_status() {
    local expected="$1"
    local actual="$2"
    [[ "${actual}" == "${expected}" ]] || fail "expected HTTP ${expected}, received ${actual}"
}

assert_contains() {
    local expected="$1"
    local file="$2"
    grep -Fq "${expected}" "${file}" || fail "expected '${expected}' in ${file}"
}

[[ -x "${SERVER_BINARY}" ]] || fail "build the server before running this test"

cat > "${TEMP_DIR}/siteconfig.fakaapache" <<CONFIG
server_name = test
bind_address = 127.0.0.1
port = ${PORT}
root_directory = ${REPO_ROOT}/www
index_file = index.html
CONFIG

"${SERVER_BINARY}" --config "${TEMP_DIR}/siteconfig.fakaapache" > "${TEMP_DIR}/server.log" 2>&1 &
SERVER_PID=$!

for _ in {1..50}; do
    if curl -fsS "${BASE_URL}/" > /dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
kill -0 "${SERVER_PID}" 2>/dev/null || fail "the server did not start"

status="$(curl -sS -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/")"
assert_status 200 "${status}"
assert_contains "FakaApache Debian Default Page" "${TEMP_DIR}/body"
assert_contains "X-Content-Type-Options: nosniff" "${TEMP_DIR}/headers"

status="$(curl -sS -X POST -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/")"
assert_status 405 "${status}"
assert_contains "Allow: GET" "${TEMP_DIR}/headers"

status="$(curl -sS -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/missing.html")"
assert_status 404 "${status}"

status="$(curl -sS --path-as-is -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/%2e%2e/siteconfig.fakaapache")"
assert_status 403 "${status}"

status="$(curl -sS --path-as-is -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/%zz")"
assert_status 400 "${status}"

large_header="$(head -c 17000 < /dev/zero | tr '\0' 'a')"
status="$(curl -sS -H "X-Large: ${large_header}" -D "${TEMP_DIR}/headers" -o "${TEMP_DIR}/body" -w '%{http_code}' "${BASE_URL}/")"
assert_status 431 "${status}"

echo "All integration tests passed."
