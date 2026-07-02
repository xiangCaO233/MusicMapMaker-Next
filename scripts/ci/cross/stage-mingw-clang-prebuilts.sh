#!/usr/bin/env bash
set -euo pipefail

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export MINGW_GCC_PREBUILT_COMPILER_TAG="${MINGW_CLANG_PREBUILT_COMPILER_TAG:-clang64}"

exec "${scriptDir}/stage-mingw-gcc-prebuilts.sh" "$@"
