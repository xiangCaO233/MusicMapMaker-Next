#!/usr/bin/env bash
set -euo pipefail

git fetch --prune origin +refs/heads/ci:refs/remotes/origin/ci
git checkout --force -B ci origin/ci
git reset --hard origin/ci
git lfs pull

rm -rf build_clang
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMMM_PGO_INSTRUMENT=ON \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_clang
cmake --build build_clang
ctest --test-dir build_clang --output-on-failure
