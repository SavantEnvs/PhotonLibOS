#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

D=/opt/toolchains/deps
# PHOTON_ENABLE_ECOSYSTEM's FetchContent deps, pre-fetched into the image (mayhem/Dockerfile).
CMAKE_DEPS=(
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  -DFETCHCONTENT_SOURCE_DIR_RAPIDJSON="$D/rapidjson-1.1.0/src"
  -DFETCHCONTENT_SOURCE_DIR_RAPIDXML="$D/rapidxml-d2f3f4e301d557ad042158e999f25b8696b8776d/src"
  -DFETCHCONTENT_SOURCE_DIR_RAPIDYAML="$D/rapidyaml-0.5.0/src"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)
# Upstream compiles with -Werror -Wall, tuned for gcc; clang-19 raises warnings upstream's CI
# never sees (-Winconsistent-missing-override, ...) and treats non-POD va_arg in
# fs/cache/persistent_cache.cpp as an error by default. -w / -Wno-non-pod-varargs only silence
# diagnostics; code generation is unchanged.
QUIET="-w -Wno-non-pod-varargs"
# Four UBSan checks are relaxed (only these): each fires on a benign pattern Photon uses by
# design, on (nearly) every input, which would hide everything else the target can find:
#   alignment        packed wire structs read through misaligned pointers (HTTP/2 frame headers,
#                    streams.cpp LE_bswap32), fine on x86-64;
#   function         Delegate/callback.h calls member functions through a type-erased
#                    `int (*)(void*, ...)` pointer (thread pool ctor, every handler);
#   nonnull-attribute  memcpy(dst, nullptr, 0) on empty buffers (url.cpp, cookie_jar.cpp);
#   pointer-overflow   offset arithmetic on null/empty buffers (basic_socket.h iov cursors).
# ASan and the rest of UBSan stay on and halting; a null-buffer dereference is still an ASan SEGV.
FUZZ_SAN="$SANITIZER_FLAGS${SANITIZER_FLAGS:+ -fno-sanitize=alignment,function,nonnull-attribute,pointer-overflow}"
SYSLIBS="-laio -lcurl -lssl -lcrypto -lz -lpthread -ldl -lrt"

# 1) The whole test suite (every photon_add_test target ctest registers), with upstream's normal
#    flags. Build type Debug (-O0 -g, asserts ON): upstream's CI uses MinSizeRel, which also keeps
#    asserts but adds -march=native and would tie the binaries to the build host's CPU; Release
#    (-DNDEBUG) would compile out every assert() the suites and the library check.
#    Compiled with gcc (by name), the compiler every upstream CI job uses: several suites rely on
#    GNU extensions clang rejects (common/test/test_throttle.cpp initializes a VLA).
CC=gcc CXX=g++ cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DPHOTON_BUILD_TESTING=ON \
  -DPHOTON_ENABLE_ECOSYSTEM=ON "${CMAKE_DEPS[@]}" \
  -DCMAKE_C_FLAGS="-w $COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="-w $COVERAGE_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="$COVERAGE_FLAGS"
cmake --build build-tests -j"$MAYHEM_JOBS"

# 2) The library, instrumented: ASan+UBSan (halting) + SanCov + DWARF 3. Build type "Fuzz" has
#    no per-config flags, so nothing appends a plain -g (DWARF 5) or -O level after ours.
FUZZ_FLAGS="$FUZZ_SAN $DEBUG_FLAGS -O1 -fsanitize=fuzzer-no-link $QUIET"
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Fuzz -DPHOTON_BUILD_TESTING=OFF \
  -DPHOTON_ENABLE_ECOSYSTEM=ON "${CMAKE_DEPS[@]}" \
  -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS"
cmake --build build-fuzz -j"$MAYHEM_JOBS" --target photon_static
LIBS="build-fuzz/output/libphoton_sole.a build-fuzz/output/libeasy_weak.a build-fuzz/output/libfstack_weak.a"

# 3) Harnesses: libFuzzer binary + run-once standalone reproducer each, LSan off (lsan_off.cc).
OBJ=build-fuzz/mayhem-obj
mkdir -p "$OBJ"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$OBJ/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"
HFLAGS="-std=c++14 -O1 -I$SRC/include -I$SRC $QUIET"
for h in url http_response http_server rpc_serve rpc_deserialize simple_dom h2 redis net_address; do
  $CXX $FUZZ_SAN $DEBUG_FLAGS -fsanitize=fuzzer-no-link $HFLAGS \
    -c "mayhem/harnesses/fuzz_$h.cpp" -o "$OBJ/fuzz_$h.o"
  $CXX $FUZZ_SAN $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ/fuzz_$h.o" "$OBJ/lsan_off.o" \
    $LIBS $SYSLIBS -o "/mayhem/fuzz_$h"
  $CXX $FUZZ_SAN $DEBUG_FLAGS "$OBJ/fuzz_$h.o" "$OBJ/standalone_main.o" "$OBJ/lsan_off.o" \
    $LIBS $SYSLIBS -o "/mayhem/fuzz_$h-standalone"
done
