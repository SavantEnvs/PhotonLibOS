# syntax=docker/dockerfile:1.7
#
# Commit-image Dockerfile (SMALL form, PREFERRED) — builds ON TOP of the org C/C++ base
# image (ghcr.io/savantenvs/base). The base already provides debian + clang/llvm +
# libFuzzer/afl++ + the identities + the build-contract ENV, so this only adds the
# per-commit build (built on top of the prebuilt base, rather than building the toolchain in-image).
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) \
#       [--build-arg SOURCE_DATE_EPOCH=$(date +%s)] \
#       -t mytag .

# The org base, pinned to a fixed DATE-tagged base (YYYYMMDD; shared by all integrations
# minted against it). Use the current dated tag; pin by digest in production: ...base@sha256:<digest>.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang ("must be a non-negative decimal integer") both here and for
# any in-image rebuild. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH

# Sanitizer override: the base exports SANITIZER_FLAGS (ASan+UBSan, halting) as the default.
# To change it for a build (e.g. ASan-only, or add MSan): --build-arg SANITIZER_FLAGS="...".
# Declare it WITHOUT a default (no '='): an undefaulted ARG lets the base ENV flow through to
# build.sh when not passed, and an explicit --build-arg overrides it. (An empty default — `=` —
# would instead shadow the base ENV and silently wipe the sanitizers.)
ARG SANITIZER_FLAGS
# Coverage (optional, off by default): instrument the TEST build to measure how much of the project
# the suite exercises — --build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping".
# Empty by default (no effect on the normal oracle build); build.sh appends it to the test build only.
ARG COVERAGE_FLAGS=
# Build parallelism (optional). build.sh falls back to nproc when unset: --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

# Extra apt packages: PhotonLibOS links libaio, libcurl, openssl and zlib (system packages, per
# upstream's CI), and its gtest/gflags suites use the packaged googletest + gflags. redis-server:
# test.sh starts one for test-ecosystem's redis tests, as upstream's CI does.
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        libaio-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
        libgtest-dev libgmock-dev libgflags-dev nasm redis-server \
 && rm -rf /var/lib/apt/lists/*
# fs/cache/test/cache_test chdir()s into a hard-coded /mnt/tmp scratch directory; give it one.
RUN mkdir -p /mnt/tmp && chown 2000:2000 /mnt/tmp

# PHOTON_ENABLE_ECOSYSTEM FetchContent's rapidjson, rapidxml and the rapidyaml single header at
# configure time. Pre-fetch each from a pinned revision + sha256 into /opt/toolchains/deps; build.sh
# points FETCHCONTENT_SOURCE_DIR_<NAME> at them with FETCHCONTENT_FULLY_DISCONNECTED=ON (air-gapped).
ARG RAPIDJSON_VER=1.1.0
ARG RAPIDJSON_SHA256=bf7ced29704a1e696fbccf2a2b4ea068e7774fa37f6d7dd4039d0787f8bed98e
ARG RAPIDXML_REV=d2f3f4e301d557ad042158e999f25b8696b8776d
ARG RAPIDXML_SHA256=3a0c070840bc478fbfee5c3be9364543a62e8b5b9b34091d6481d7a62a768449
ARG RAPIDYAML_VER=0.5.0
ARG RAPIDYAML_SHA256=09a5be64f7add04e24a675704a16b3aea7ed80e9ff73d865a3b92301971d0815
RUN set -e; D=/opt/toolchains/deps \
 && mkdir -p $D/rapidjson-${RAPIDJSON_VER}/src $D/rapidxml-${RAPIDXML_REV}/src $D/rapidyaml-${RAPIDYAML_VER}/src \
 && curl -fsSL -o /tmp/rj.tgz https://github.com/Tencent/rapidjson/archive/refs/tags/v${RAPIDJSON_VER}.tar.gz \
 && echo "${RAPIDJSON_SHA256}  /tmp/rj.tgz" | sha256sum -c - \
 && tar -xzf /tmp/rj.tgz --strip-components=1 -C $D/rapidjson-${RAPIDJSON_VER}/src \
 && curl -fsSL -o /tmp/rx.tgz https://github.com/photonlibos/rapidxml/archive/${RAPIDXML_REV}.tar.gz \
 && echo "${RAPIDXML_SHA256}  /tmp/rx.tgz" | sha256sum -c - \
 && tar -xzf /tmp/rx.tgz --strip-components=1 -C $D/rapidxml-${RAPIDXML_REV}/src \
 && curl -fsSL -o $D/rapidyaml-${RAPIDYAML_VER}/src/rapidyaml-${RAPIDYAML_VER}.hpp \
      https://github.com/biojppm/rapidyaml/releases/download/v${RAPIDYAML_VER}/rapidyaml-${RAPIDYAML_VER}.hpp \
 && echo "${RAPIDYAML_SHA256}  $D/rapidyaml-${RAPIDYAML_VER}/src/rapidyaml-${RAPIDYAML_VER}.hpp" | sha256sum -c - \
 && rm -f /tmp/rj.tgz /tmp/rx.tgz && chmod -R a+rX,go-w $D
USER mayhem

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git
# history) is copied to /mayhem as `mayhem`; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Anything a Dockerfile step materializes into the tree (downloaded fixtures, generated headers,
# vendored deps) MUST be committed or (re)created by build.sh: rlenv's patch grader runs
# `git clean -ffdX` before every graded build, so a gitignored, untracked file is gone by then and
# every honest patch scores zero (SPEC §6.2 item 16; verify-repo.sh checks it).
#
# OPTIONAL compile-cache pre-fill (SPEC §6.2 item 18). Bases minted with /opt/mayhem/ccache/prefill
# run the build under rlenv's pinned ccache, so the patch grader's first (otherwise cold) rebuild is
# warm — this is what keeps large C/C++ targets inside the build time limit. Older bases fall back to
# the plain build; a missing or stale pre-fill only costs a cold cache. Never enable ccache in
# build.sh or via ENV/PATH, and keep the compiler resolving by name (CC=clang) or nothing is cached.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
