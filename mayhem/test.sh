#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# RUN upstream's whole ctest suite, as upstream's CI does (`ctest --timeout 3600 -E "test-blk"`
# with a redis-server up), from the Debug (asserts on) gcc build mayhem/build.sh made in build-tests.
# Excluded, each because it cannot run in this image (offline, unprivileged container):
#   test-blk              upstream's CI excludes these too (tcmu/ublk/vduse kernel modules, root)
#   test-iouring          io_uring_setup is unavailable in the container (every engine init fails)
#   test-ipv6             run separately below, minus ipv6.dns_lookup (resolves taobao.com),
#                         ipv6.get_host_by_peer (routes to 8.8.8.8) and V4ToV6Test.* (binds [::])
#   test-curl             fetches https://github.com/ (network egress; segfaults when it gets a reply)
#   client_function_test  run separately below, minus http_client.get, which GETs a public site
EXCLUDE='test-blk|^test-iouring$|^test-ipv6$|^test-curl$|^client_function_test$'
BT="$SRC/build-tests"
if [ ! -f "$BT/CTestTestfile.cmake" ] || [ ! -x "$BT/output/client_function_test" ] \
   || [ ! -x "$BT/output/test-ipv6" ]; then
  echo "MISSING test build in $BT (build.sh should have built it)" >&2
  emit_ctrf "cmake-ctest" 0 1; exit 1
fi

# test-ecosystem's redis tests talk to a local redis-server on the default port (as in upstream CI).
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/photon-test.XXXXXX")
redis-server --port 6379 --bind 127.0.0.1 --save '' --appendonly no --dir "$TMPD" \
  --daemonize yes --pidfile "$TMPD/redis.pid" --logfile "$TMPD/redis.log"
for _ in $(seq 50); do redis-cli -p 6379 ping >/dev/null 2>&1 && break; sleep 0.1; done
cleanup() { [ -f "$TMPD/redis.pid" ] && kill "$(cat "$TMPD/redis.pid")" 2>/dev/null; rm -rf "$TMPD"; }
trap cleanup EXIT

log="$TMPD/ctest.log"
( cd "$BT" && ctest --timeout 3600 -j"$MAYHEM_JOBS" --output-on-failure -E "$EXCLUDE" ) 2>&1 | tee "$log" \
  | grep -aE '^ *[0-9]+/[0-9]+ Test +#|tests passed|FAILED|Failure' || true
# ctest's summary: "NN% tests passed, M tests failed out of T".
sum=$(grep -aoE '[0-9]+ tests? failed out of [0-9]+' "$log" | tail -1)
if [ -z "$sum" ]; then
  echo "ctest printed no summary" >&2; tail -30 "$log"; f=1; p=0
else
  f=$(echo "$sum" | awk '{print $1}'); t=$(echo "$sum" | awk '{print $NF}'); p=$((t - f))
  [ "$t" -gt 0 ] || f=1
fi

# client_function_test (HTTP client, URL parsing, ...) as one more suite, minus its one test that
# needs the internet.
cl="$TMPD/client.log"
( cd "$BT/output" && ./client_function_test --gtest_filter='-http_client.get' ) >"$cl" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -aqE '^\[  PASSED  \] [0-9]+ test' "$cl" && ! grep -aqE '^\[  FAILED  \]' "$cl"; then
  p=$((p + 1)); echo "client_function_test: Passed"
else
  f=$((f + 1)); echo "client_function_test: FAILED (exit $rc)"; grep -a -E '^\[  FAILED  \]|Failure' "$cl" | head -20
fi

# test-ipv6's offline tests (ipv6.endpoint, ipv6.addr: [v6]:port / v6 address parsing and
# rejection) as one more suite, minus the ones that need network egress or an IPv6 stack.
v6="$TMPD/ipv6.log"
( cd "$BT/output" && ./test-ipv6 --gtest_filter='-ipv6.dns_lookup:ipv6.get_host_by_peer:V4ToV6Test.*' ) >"$v6" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -aqE '^\[  PASSED  \] [0-9]+ test' "$v6" && ! grep -aqE '^\[  FAILED  \]' "$v6"; then
  p=$((p + 1)); echo "test-ipv6: Passed"
else
  f=$((f + 1)); echo "test-ipv6: FAILED (exit $rc)"; grep -a -E '^\[  FAILED  \]|Failure' "$v6" | head -20
fi
emit_ctrf "cmake-ctest" "$p" "$f"
