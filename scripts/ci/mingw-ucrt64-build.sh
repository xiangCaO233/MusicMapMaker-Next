#!/usr/bin/env bash
set -euo pipefail

git fetch --prune origin +refs/heads/ci:refs/remotes/origin/ci
git checkout --force -B ci origin/ci
git reset --hard origin/ci
git submodule update --init --recursive
git lfs pull

rm -rf build_gcc
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_gcc
cmake --build build_gcc
ctest --test-dir build_gcc --output-on-failure
