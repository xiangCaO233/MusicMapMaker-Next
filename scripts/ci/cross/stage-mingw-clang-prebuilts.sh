#!/usr/bin/env bash
# 将通用 MinGW 预编译 staging 流程切换到 clang64 目录标签。
set -euo pipefail

# 绝对脚本目录保证从任意 CI 工作目录调用同级实现。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 环境变量允许发布任务覆盖默认 clang64 标签。
export MINGW_GCC_PREBUILT_COMPILER_TAG="${MINGW_CLANG_PREBUILT_COMPILER_TAG:-clang64}"

# exec 复用完整 GCC staging 实现并原样转发所有参数与退出状态。
exec "${scriptDir}/stage-mingw-gcc-prebuilts.sh" "$@"
