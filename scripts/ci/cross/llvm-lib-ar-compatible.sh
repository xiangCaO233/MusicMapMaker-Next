#!/usr/bin/env bash
# 将 GNU ar 风格调用转换为 llvm-lib 的 /OUT: 参数形式。
# 包装器服务 MSVC ABI 的第三方构建，不尝试模拟全部 ar 操作模式。
set -euo pipefail

# 环境覆盖允许上层固定与 clang-cl 配套的 LLVM 主版本。
llvmLib="${MMM_LLVM_LIB:-llvm-lib-22}"

# 至少需要输出库参数，空调用无法安全推导目标。
if (( $# < 1 )); then
    printf "error: llvm-lib ar compatibility wrapper requires an output library\n" >&2
    exit 1
fi

# GNU ar 的可选首个参数只描述归档操作；llvm-lib 使用 /OUT: 指定同一目标。
if [[ "$1" =~ ^-?[a-zA-Z]+$ ]] && (( $# >= 2 )); then
    # rcs 等操作字对 llvm-lib 没有等价位置参数，直接移除。
    shift
fi

# 首个剩余参数是目标库，其余参数均视为输入对象或归档。
outputLibrary="$1"
shift
# 先删除旧归档，模拟 GNU ar 重建而非增量保留旧成员的调用语义。
rm -f -- "${outputLibrary}"
# exec 直接传播 llvm-lib 的退出码，并以 /nologo 保持 CI 输出简洁。
exec "${llvmLib}" /nologo "/OUT:${outputLibrary}" "$@"
