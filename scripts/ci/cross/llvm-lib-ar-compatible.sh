#!/usr/bin/env bash
set -euo pipefail

llvmLib="${MMM_LLVM_LIB:-llvm-lib-22}"

if (( $# < 1 )); then
    printf "error: llvm-lib ar compatibility wrapper requires an output library\n" >&2
    exit 1
fi

# GNU ar 的可选首个参数只描述归档操作；llvm-lib 使用 /OUT: 指定同一目标。
if [[ "$1" =~ ^-?[a-zA-Z]+$ ]] && (( $# >= 2 )); then
    shift
fi

outputLibrary="$1"
shift
rm -f -- "${outputLibrary}"
exec "${llvmLib}" /nologo "/OUT:${outputLibrary}" "$@"
