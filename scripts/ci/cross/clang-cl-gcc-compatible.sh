#!/usr/bin/env bash
# 为只理解 GNU 探针参数的第三方 Make 项目包装 clang-cl。
# 普通编译继续调用 clang-cl；仅 -dM 宏探针切换到同版本 clang 驱动。
# 非仅编译调用显式选择 lld，保持 MSVC ABI 链接路径。
set -euo pipefail

# 三个环境变量允许交叉构建脚本固定 LLVM 版本与目标 triple。
clangCl="${MMM_CLANG_CL:-clang-cl-22}"
clangC="${MMM_CLANG_C:-clang-22}"
targetTriple="${MMM_MSVC_TARGET_TRIPLE:-x86_64-pc-windows-msvc}"

# 参数扫描不执行外部进程，完成后仅选择一个最终 exec 路径。
# GNU Make 项目通常通过 -dM -E 探测目标宏；clang-cl 不识别 -dM，
# 仅在该探测路径切换到同版本 clang 驱动，其余编译仍严格使用 clang-cl。
# 两个标志分别记录宏探针与只编译模式，决定最终 driver 参数。
isMacroProbe=0
isCompileOnly=0
for argument in "$@"; do
    # 第一遍不改写参数，只识别调用意图。
    case "${argument}" in
        -dM)
            # -dM 是需要 GNU clang frontend 的唯一决定性标志。
            isMacroProbe=1
            ;;
        -c | /c | -E | /E)
            # 编译或预处理调用不需要显式链接器。
            isCompileOnly=1
            ;;
    esac
done

if (( isMacroProbe )); then
    # 新数组保存移除 clang-cl 专用参数后的 GNU frontend 参数。
    probeArguments=()
    convertSystemInclude=0
    for argument in "$@"; do
        if (( convertSystemInclude )); then
            # -imsvc 的下一个路径转换为 GNU clang 的 -isystem 二元参数。
            probeArguments+=("-isystem" "${argument}")
            convertSystemInclude=0
            continue
        fi
        case "${argument}" in
            -imsvc)
                # 延迟到下一轮读取目录参数，避免丢失含空格路径。
                convertSystemInclude=1
                ;;
            /EHsc | /MT | /MTd | /MD | /MDd | /Z7 | /Zi | /Od | /O[0-9a-zA-Z]* | /Ob[0-9]* | /RTC[0-9]* | /nologo | /c)
                # MSVC 专用运行库、调试和优化参数不传给 GNU clang 探针。
                ;;
            *)
                # 预处理宏、定义、include 和输入文件等其余参数原样保留。
                probeArguments+=("${argument}")
                ;;
        esac
    done
    # exec 保持原工具退出码与信号语义，不额外包装子进程。
    exec "${clangC}" --target="${targetTriple}" "${probeArguments[@]}"
fi

if (( !isCompileOnly )); then
    # 链接调用强制 lld，防止 clang-cl 在 Linux 上寻找不可用的 link.exe。
    exec "${clangCl}" --target="${targetTriple}" -fuse-ld=lld "$@"
fi

# 其余编译与预处理调用使用原始 clang-cl 参数。
exec "${clangCl}" --target="${targetTriple}" "$@"
