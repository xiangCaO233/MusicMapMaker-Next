#!/usr/bin/env bash
set -euo pipefail

detectCiBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 9 / 10 ))
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

ciBuildJobs="$(detectCiBuildJobs)"

git fetch --prune origin +refs/heads/ci:refs/remotes/origin/ci
git checkout --force -B ci origin/ci
git reset --hard origin/ci
git submodule update --init --recursive
git lfs pull

rm -rf build_clang
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMMM_PGO_INSTRUMENT=ON \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_clang
cmake --build build_clang --parallel "${ciBuildJobs}"
ctest --test-dir build_clang --output-on-failure
