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

    local buildJobs=$(( maxThreads * 3 / 4 ))
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

ciBuildJobs="$(detectCiBuildJobs)"
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

git submodule update --init --recursive
bash "${scriptDir}/pull-lfs-for-build.sh" \
    --platform windows \
    --arch x86_64 \
    --toolchain mingw \
    --compiler-tag ucrt64 \
    --build-type RelWithDebInfo \
    --linkage static \
    --include-tests

rm -rf build_gcc
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSOURCES_BUILD=OFF \
    -DBUILD_TESTING=ON \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_gcc
cmake --build build_gcc --parallel "${ciBuildJobs}"
ctest --test-dir build_gcc --output-on-failure
