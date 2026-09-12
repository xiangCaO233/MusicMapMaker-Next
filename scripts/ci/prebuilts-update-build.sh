#!/usr/bin/env bash
# 编排 Windows 与 Linux 全部受支持工具链的第三方源码构建和预编译 staging。
# 每个配置先完成全矩阵 configure，再进入 build/stage，尽早暴露环境或生成错误。
# 默认覆盖 Debug 与 RelWithDebInfo，且当前只允许静态预编译布局。
# 各平台细节委托给对应 build 和 stage 脚本，本层保持矩阵坐标一致。
# --skip 系列开关用于缩小矩阵，但至少必须保留一个目标工具链。
set -euo pipefail

# 输出矩阵、目录、并行度和过滤开关契约。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/prebuilts-update-build.sh [options]

Configure, build, and stage all source-built prebuilt libraries used by CI.

Default matrix:
  Windows MSVC 2026:    clang-cl 22 + lld-link + MSVC/UCRT
  Windows MinGW clang64: clang 22 llvm-mingw UCRT + libc++
  Windows MinGW ucrt64:  GCC 14 UCRT64
  Linux gcc14:           native GCC 14
  Linux clang19:         native Clang 19

Options:
  --configs <list>             Space/comma separated CMake configs. Default: Debug RelWithDebInfo
  --build-root <path>          Root for generated build directories. Default: build_prebuilts_update
  --jobs <count>               Parallel build jobs passed to child build scripts.
  --linkage <static>           PROJECT_LINKAGE/ICE_LINKAGE value. Default: static.
                               Shared staging is not implemented in this script yet.
  --llvm-mingw-root <path>     Complete llvm-mingw UCRT toolchain root for clang64.
  --configure-only             Run only the configure-only phase.
  --reuse-build-dirs           Do not remove build directories during the configure-only phase.
  --skip-windows               Skip all Windows cross prebuilt libraries.
  --skip-linux                 Skip all Linux native prebuilt libraries.
  --skip-msvc                  Skip Windows msvc/2026 prebuilts.
  --skip-mingw-clang           Skip Windows mingw/clang64 prebuilts.
  --skip-mingw-gcc             Skip Windows mingw/ucrt64 prebuilts.
  --skip-linux-gcc             Skip Linux gcc/gcc14 prebuilts.
  --skip-linux-clang           Skip Linux clang/clang19 prebuilts.
  -h, --help                   Show this help

Environment overrides:
  PREBUILTS_UPDATE_CONFIGS       Default configs list
  PREBUILTS_UPDATE_BUILD_ROOT    Default build root
  PREBUILTS_UPDATE_JOBS          Default parallel jobs
  LLVM_MINGW_ROOT                Default llvm-mingw UCRT toolchain root
  CMAKE_GENERATOR                Passed through to child build scripts. Default: Ninja
EOF
}

# 使用在线处理器约四分之三作为子构建默认并行度。
detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        # Linux 优先使用 nproc 感知容器限制。
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        # POSIX getconf 是兼容回退。
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        # 混合 Runner 可使用 Windows 风格环境变量。
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        # 异常探测结果保守回退单线程。
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    # 整数除法后至少保留一个任务。
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

# 将调用方相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许大型构建根位于独立磁盘。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖脚本调用目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 接受空格或逗号分隔的配置列表并写入全局数组。
setConfigs() {
    local rawConfigs="${1//,/ }"
    # 先把逗号转为空格，再按 shell 字段规则拆分。

    read -r -a configs <<<"${rawConfigs}"
    if (( ${#configs[@]} == 0 )); then
        # 空矩阵不能形成任何预编译发布结果。
        printf "error: --configs must not be empty\n" >&2
        exit 1
    fi
}

# 限制矩阵配置为标准 CMake 构建类型。
validateConfig() {
    case "$1" in
        Debug | Release | RelWithDebInfo | MinSizeRel)
            # 合法配置名称原样进入构建和 staging 目录。
            ;;
        *)
            # 未知配置不得由下游脚本自行猜测映射。
            printf "error: unsupported CMake config: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

# 以可复核格式打印标签和 shell 转义后的命令再执行。
runCommand() {
    local label="$1"
    shift

    printf "\n==> %s\n" "${label}"
    printf "    "
    # %q 保留数组参数边界，便于从日志准确复现。
    printf "%q " "$@"
    printf "\n"
    "$@"
}

# 以下 helper 统一生成不会跨平台或配置碰撞的构建目录。
msvcBuildDir() {
    printf "%s/windows-msvc-2026-%s\n" "${buildRoot}" "$1"
}

# MinGW Clang 目录编码 clang64 工具链标签。
mingwClangBuildDir() {
    printf "%s/windows-mingw-clang64-%s\n" "${buildRoot}" "$1"
}

# MinGW GCC 目录编码 ucrt64 工具链标签。
mingwGccBuildDir() {
    printf "%s/windows-mingw-ucrt64-%s\n" "${buildRoot}" "$1"
}

# Linux GCC 目录编码 gcc14 编译器标签。
linuxGccBuildDir() {
    printf "%s/linux-gcc14-%s\n" "${buildRoot}" "$1"
}

# Linux Clang 目录编码 clang19 编译器标签。
linuxClangBuildDir() {
    printf "%s/linux-clang19-%s\n" "${buildRoot}" "$1"
}

# 配置 clang-cl/MSVC 源码依赖构建树，不执行编译。
runMsvcConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(msvcBuildDir "${config}")"

    # 数组固定 ABI 坐标并请求第三方 target 配置。
    local command=(
        bash
        "${crossScriptDir}/msvc-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag 2026
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        # 默认清除旧 cache，复用需显式开关。
        command+=(--fresh)
    fi

    runCommand "configure windows msvc/2026 ${config}" "${command[@]}"
}

# 构建并 staging 一套 clang-cl/MSVC 配置。
runMsvcBuild() {
    local config="$1"
    local buildDir
    buildDir="$(msvcBuildDir "${config}")"

    # 构建阶段复用前一阶段生成的同一绝对目录。
    runCommand "build windows msvc/2026 ${config}" \
        bash \
        "${crossScriptDir}/msvc-clang-build.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag 2026 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --sources-build \
        --prebuilt-targets
    # clang-cl 归档声明 CodeView 内嵌，清理可能存在的旧 PDB。
    runCommand "stage windows msvc/2026 ${config}" \
        bash \
        "${crossScriptDir}/stage-msvc-clang-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag 2026 \
        --embedded-symbols
}

# 配置 MinGW Clang/llvm-mingw 源码依赖构建树。
runMingwClangConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(mingwClangBuildDir "${config}")"

    # clang64 tag、静态链接和依赖 target 在矩阵层固定。
    local command=(
        bash
        "${crossScriptDir}/mingw-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag clang64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if [[ -n "${llvmMingwRoot}" ]]; then
        # 完整 llvm-mingw 根仅在调用方提供时传递。
        command+=(--llvm-mingw-root "${llvmMingwRoot}")
    fi
    if (( freshConfigure )); then
        # 默认使用全新 CMake cache。
        command+=(--fresh)
    fi

    runCommand "configure windows mingw/clang64 ${config}" "${command[@]}"
}

# 构建并 staging 一套 MinGW Clang 配置。
runMingwClangBuild() {
    local config="$1"
    local buildDir
    buildDir="$(mingwClangBuildDir "${config}")"

    # 构建参数与 configure 阶段除停止点外保持一致。
    local command=(
        bash
        "${crossScriptDir}/mingw-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag clang64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
    )
    if [[ -n "${llvmMingwRoot}" ]]; then
        # 工具链根必须在两个阶段使用同一值。
        command+=(--llvm-mingw-root "${llvmMingwRoot}")
    fi

    # 先完成全部归档，再调用对应 MinGW staging。
    runCommand "build windows mingw/clang64 ${config}" "${command[@]}"
    runCommand "stage windows mingw/clang64 ${config}" \
        bash \
        "${crossScriptDir}/stage-mingw-clang-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag clang64
}

# 配置 MinGW GCC UCRT64 源码依赖构建树。
runMingwGccConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(mingwGccBuildDir "${config}")"

    # ucrt64 tag 与下游 staging 输出目录保持一致。
    local command=(
        bash
        "${crossScriptDir}/mingw-gcc-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag ucrt64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        # 默认清理旧工具链 cache。
        command+=(--fresh)
    fi

    runCommand "configure windows mingw/ucrt64 ${config}" "${command[@]}"
}

# 构建并 staging 一套 MinGW GCC 配置。
runMingwGccBuild() {
    local config="$1"
    local buildDir
    buildDir="$(mingwGccBuildDir "${config}")"

    # 构建阶段复用配置阶段的目录和 ABI 参数。
    runCommand "build windows mingw/ucrt64 ${config}" \
        bash \
        "${crossScriptDir}/mingw-gcc-build.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag ucrt64 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --sources-build \
        --prebuilt-targets
    # staging 脚本负责 COFF 归档验证和规范输出布局。
    runCommand "stage windows mingw/ucrt64 ${config}" \
        bash \
        "${crossScriptDir}/stage-mingw-gcc-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag ucrt64
}

# 配置原生 Linux GCC 14 源码依赖构建树。
runLinuxGccConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(linuxGccBuildDir "${config}")"

    # 预编译 producer 显式关闭业务模块 PGO 插桩。
    local command=(
        bash
        "${ciScriptDir}/linux-build.sh"
        --compiler gcc14
        --build-dir "${buildDir}"
        --build-type "${config}"
        --toolchain gcc
        --compiler-tag gcc14
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --no-pgo-instrument
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        # 默认清理旧 GCC CMake cache。
        command+=(--fresh)
    fi

    runCommand "configure linux gcc/gcc14 ${config}" "${command[@]}"
}

# 构建并 staging 一套 Linux GCC 14 配置。
runLinuxGccBuild() {
    local config="$1"
    local buildDir
    buildDir="$(linuxGccBuildDir "${config}")"

    # 构建参数与 configure 阶段保持一致。
    runCommand "build linux gcc/gcc14 ${config}" \
        bash \
        "${ciScriptDir}/linux-build.sh" \
        --compiler gcc14 \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain gcc \
        --compiler-tag gcc14 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --no-pgo-instrument \
        --sources-build \
        --prebuilt-targets
    # 输出目录固定为 linux/x86_64/gcc/gcc14/config。
    runCommand "stage linux gcc/gcc14 ${config}" \
        bash \
        "${ciScriptDir}/stage-linux-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain gcc \
        --compiler-tag gcc14
}

# 配置原生 Linux Clang 19 源码依赖构建树。
runLinuxClangConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(linuxClangBuildDir "${config}")"

    # 源码依赖构建显式关闭自动 PGO 插桩。
    local command=(
        bash
        "${ciScriptDir}/linux-build.sh"
        --compiler clang19
        --build-dir "${buildDir}"
        --build-type "${config}"
        --toolchain clang
        --compiler-tag clang19
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --no-pgo-instrument
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        # 默认清理旧 Clang CMake cache。
        command+=(--fresh)
    fi

    runCommand "configure linux clang/clang19 ${config}" "${command[@]}"
}

# 构建并 staging 一套 Linux Clang 19 配置。
runLinuxClangBuild() {
    local config="$1"
    local buildDir
    buildDir="$(linuxClangBuildDir "${config}")"

    # 构建参数与 configure 阶段保持一致。
    runCommand "build linux clang/clang19 ${config}" \
        bash \
        "${ciScriptDir}/linux-build.sh" \
        --compiler clang19 \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain clang \
        --compiler-tag clang19 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --no-pgo-instrument \
        --sources-build \
        --prebuilt-targets
    # 输出目录固定为 linux/x86_64/clang/clang19/config。
    runCommand "stage linux clang/clang19 ${config}" \
        bash \
        "${ciScriptDir}/stage-linux-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain clang \
        --compiler-tag clang19
}

# 先遍历所有配置和启用工具链完成生成阶段。
runConfigurePhase() {
    local config
    for config in "${configs[@]}"; do
        # 每个配置在进入任一子脚本前验证一次。
        validateConfig "${config}"
        if (( enableMsvc )); then
            # 各 enable 标志独立控制矩阵单元。
            runMsvcConfigure "${config}"
        fi
        if (( enableMingwClang )); then
            # 完整 llvm-mingw 参数由专用 helper 处理。
            runMingwClangConfigure "${config}"
        fi
        if (( enableMingwGcc )); then
            # GCC 与 Clang MinGW 使用独立构建树。
            runMingwGccConfigure "${config}"
        fi
        if (( enableLinuxGcc )); then
            # 原生 GCC 与交叉矩阵顺序执行。
            runLinuxGccConfigure "${config}"
        fi
        if (( enableLinuxClang )); then
            # Clang 配置是当前阶段最后一个矩阵单元。
            runLinuxClangConfigure "${config}"
        fi
    done
}

# 在全部配置成功后遍历相同矩阵执行构建和 staging。
runBuildPhase() {
    local config
    for config in "${configs[@]}"; do
        # 配置已由 runConfigurePhase 验证，无需重复解析。
        if (( enableMsvc )); then
            # 每个 build helper 内部紧接对应 staging。
            runMsvcBuild "${config}"
        fi
        if (( enableMingwClang )); then
            # 任一单元失败会因严格模式停止后续矩阵。
            runMingwClangBuild "${config}"
        fi
        if (( enableMingwGcc )); then
            # 不并发写预编译目录，保持日志和输出确定。
            runMingwGccBuild "${config}"
        fi
        if (( enableLinuxGcc )); then
            # Linux 目标复用同一构建并行度上限。
            runLinuxGccBuild "${config}"
        fi
        if (( enableLinuxClang )); then
            # 最后完成 clang19 归档集合。
            runLinuxClangBuild "${config}"
        fi
    done
}

# 通过脚本位置定位 CI helper、交叉 helper 和项目根。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ciScriptDir="${scriptDir}"
crossScriptDir="${ciScriptDir}/cross"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

defaultConfigs="${PREBUILTS_UPDATE_CONFIGS:-Debug RelWithDebInfo}"
configs=()
# 环境默认值同样经过统一列表解析。
setConfigs "${defaultConfigs}"

# buildRoot 下每个矩阵单元使用独立子目录。
buildRoot="${PREBUILTS_UPDATE_BUILD_ROOT:-build_prebuilts_update}"
buildJobs="${PREBUILTS_UPDATE_JOBS:-$(detectBuildJobs)}"
# staging 当前只实现静态布局。
projectLinkage="static"
llvmMingwRoot="${LLVM_MINGW_ROOT:-}"
configureOnly=0
# 默认 configure 阶段清理每个构建树。
freshConfigure=1
# 五个矩阵目标默认全部启用。
enableMsvc=1
enableMingwClang=1
enableMingwGcc=1
enableLinuxGcc=1
enableLinuxClang=1

# 在启动任何子脚本前完整解析矩阵过滤选项。
while (( $# > 0 )); do
    case "$1" in
        --configs)
            # 命令行配置列表覆盖环境默认矩阵。
            if (( $# < 2 )); then
                printf "error: --configs requires a value\n" >&2
                exit 1
            fi
            setConfigs "$2"
            shift 2
            ;;
        --build-root)
            # 路径稍后统一解析到项目根。
            if (( $# < 2 )); then
                printf "error: --build-root requires a value\n" >&2
                exit 1
            fi
            buildRoot="$2"
            shift 2
            ;;
        --jobs)
            # 单一并行度传给所有子构建。
            if (( $# < 2 )); then
                printf "error: --jobs requires a value\n" >&2
                exit 1
            fi
            buildJobs="$2"
            shift 2
            ;;
        --linkage)
            # 参数保留为未来 shared staging 扩展入口。
            if (( $# < 2 )); then
                printf "error: --linkage requires a value\n" >&2
                exit 1
            fi
            projectLinkage="$2"
            shift 2
            ;;
        --llvm-mingw-root)
            # 工具链根只传给 MinGW Clang 单元。
            if (( $# < 2 )); then
                printf "error: --llvm-mingw-root requires a value\n" >&2
                exit 1
            fi
            llvmMingwRoot="$2"
            shift 2
            ;;
        --configure-only)
            # 只运行第一阶段，不生成或覆盖预编译归档。
            configureOnly=1
            shift
            ;;
        --reuse-build-dirs)
            # 禁用传给各子构建的 --fresh 参数。
            freshConfigure=0
            shift
            ;;
        --skip-windows)
            # 平台级开关一次关闭三套 Windows 工具链。
            enableMsvc=0
            enableMingwClang=0
            enableMingwGcc=0
            shift
            ;;
        --skip-linux)
            # 平台级开关一次关闭两套 Linux 编译器。
            enableLinuxGcc=0
            enableLinuxClang=0
            shift
            ;;
        --skip-msvc)
            # 单独关闭 clang-cl/MSVC 矩阵单元。
            enableMsvc=0
            shift
            ;;
        --skip-mingw-clang)
            # 单独关闭 llvm-mingw/clang64 单元。
            enableMingwClang=0
            shift
            ;;
        --skip-mingw-gcc)
            # 单独关闭 MinGW GCC/ucrt64 单元。
            enableMingwGcc=0
            shift
            ;;
        --skip-linux-gcc)
            # 单独关闭原生 GCC 14 单元。
            enableLinuxGcc=0
            shift
            ;;
        --skip-linux-clang)
            # 单独关闭原生 Clang 19 单元。
            enableLinuxClang=0
            shift
            ;;
        -h | --help)
            # 帮助成功退出且不验证任何工具链。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数作为调用错误处理。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # 子构建并行度必须是正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" ]]; then
    # 动态布局还需同时 staging bin、导入库和 symbols，当前拒绝。
    printf "error: --linkage currently supports only 'static'; shared staging needs separate bin/libs/symbols layout support\n" >&2
    exit 1
fi

if (( ! enableMsvc && ! enableMingwClang && ! enableMingwGcc && ! enableLinuxGcc && ! enableLinuxClang )); then
    # 空矩阵通常是过滤参数错误，不能报告虚假成功。
    printf "error: no prebuilt target is enabled\n" >&2
    exit 1
fi

buildRoot="$(projectPath "${buildRoot}")"
if [[ -n "${llvmMingwRoot}" ]]; then
    # 显式工具链根使用与构建根一致的路径解析规则。
    llvmMingwRoot="$(projectPath "${llvmMingwRoot}")"
fi

# 执行前打印完整矩阵摘要，便于人工确认写入范围。
printf "Prebuilt update matrix:\n"
printf "  configs: %s\n" "${configs[*]}"
printf "  build root: %s\n" "${buildRoot}"
printf "  jobs: %s\n" "${buildJobs}"
printf "  linkage: %s\n" "${projectLinkage}"
printf "  fresh configure: %s\n" "${freshConfigure}"
printf "  targets:"
if (( enableMsvc )); then
    # 只打印实际启用的矩阵单元。
    printf " windows-msvc-2026"
fi
if (( enableMingwClang )); then
    printf " windows-mingw-clang64"
fi
if (( enableMingwGcc )); then
    printf " windows-mingw-ucrt64"
fi
if (( enableLinuxGcc )); then
    printf " linux-gcc14"
fi
if (( enableLinuxClang )); then
    printf " linux-clang19"
fi
printf "\n"

# 第一阶段确保所有启用工具链都能成功生成构建系统。
runConfigurePhase

if (( configureOnly )); then
    # 配置探针模式明确报告阶段完成并停止。
    printf "\nconfigure-only phase completed.\n"
    exit 0
fi

# 第二阶段按相同顺序构建并写入预编译目录。
runBuildPhase

# 维护约束：矩阵坐标必须与仓库预编译目录布局完全一致。
# 维护约束：Windows MSVC 默认组合固定 clang-cl 22 与 compiler tag 2026。
# 维护约束：MinGW Clang 默认组合固定 llvm-mingw UCRT、libc++ 与 clang64。
# 维护约束：MinGW GCC 默认组合固定 GCC 14 UCRT64 与 ucrt64。
# 维护约束：Linux GCC 默认组合固定 gcc14 可执行文件与 gcc/gcc14。
# 维护约束：Linux Clang 默认组合固定 clang19 可执行文件与 clang/clang19。
# 维护约束：工具链升级必须同步 build helper、stage helper 和目录命名。
# 维护约束：每个配置和工具链组合必须使用独立构建目录。
# 维护约束：不同配置不得共享单配置生成器的 CMake cache。
# 维护约束：不同平台不得从同一构建树提取归档。
# 维护约束：默认配置必须至少覆盖 Debug 与 RelWithDebInfo。
# 维护约束：Release 和 MinSizeRel 虽可选择，仍需遵守 staging 映射。
# 维护约束：配置列表顺序决定矩阵执行和日志顺序。
# 维护约束：重复配置会重复构建，调用方应提供去重列表。
# 维护约束：配置列表不得包含 shell 通配或非标准 CMake 名称。
# 维护约束：静态链接是当前唯一允许的统一更新模式。
# 维护约束：shared 支持需先实现 libs、bin 和 symbols 三类 staging。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 由子脚本保持同步。
# 维护约束：源码依赖 producer 不得从预编译目录自动回退补件。
# 维护约束：预编译更新构建不得启用业务模块 PGO 插桩。
# 维护约束：第三方依赖二进制不得携带 PGO instrumentation。
# 维护约束：Debug 与 RelWithDebInfo 归档都必须保留调试信息。
# 维护约束：MinGW 调试信息保留在 COFF 归档内而非旁路 PDB。
# 维护约束：clang-cl 内嵌 CodeView 模式必须清理陈旧外置 PDB。
# 维护约束：Linux ELF 归档不得在 staging 前执行 strip。
# 维护约束：configure 阶段必须在任何 build/stage 阶段之前全部完成。
# 维护约束：全配置优先生成可减少长构建后才发现环境错误的风险。
# 维护约束：任一 configure 失败必须终止整个矩阵。
# 维护约束：任一 build 或 stage 失败必须终止后续矩阵单元。
# 维护约束：脚本按序运行以避免多个任务同时覆盖 LFS 目标路径。
# 维护约束：子构建内部并行度由统一 buildJobs 控制。
# 维护约束：buildJobs 不代表矩阵级并行度。
# 维护约束：默认 fresh 配置保证旧 cache 不污染新工具链产物。
# 维护约束：reuse-build-dirs 只适用于调用方确认 ABI 参数未变化的场景。
# 维护约束：复用目录时仍应检查各子脚本输出的配置诊断。
# 维护约束：configure-only 不得执行 staging 或覆盖预编译归档。
# 维护约束：configure-only 可用于验证新 Runner 工具链是否齐全。
# 维护约束：skip-windows 与单工具链 skip 可以组合且保持幂等。
# 维护约束：skip-linux 与单编译器 skip 可以组合且保持幂等。
# 维护约束：所有 skip 结果为空时必须作为调用错误。
# 维护约束：buildRoot 必须在子目录名拼接前转为绝对路径。
# 维护约束：buildRoot 可以位于项目外，但不应指向源码仓根。
# 维护约束：实际删除安全性由各平台 build 脚本再次验证。
# 维护约束：LLVM_MINGW_ROOT 非空时必须在所有 clang64 阶段一致传递。
# 维护约束：矩阵层不自行修改 PATH 或探测 llvm-mingw 内部命令。
# 维护约束：平台 build 脚本负责工具、SDK、运行库和 sysroot 验证。
# 维护约束：平台 stage 脚本负责格式、架构、输出名和目标路径验证。
# 维护约束：本脚本不直接复制、删除或重命名预编译文件。
# 维护约束：本脚本不负责更新共享预编译头文件。
# 维护约束：二进制更新后需另行运行 stage-prebuilt-headers 保持 API 同步。
# 维护约束：主仓和 IonCachyEngine 同名依赖必须同批更新。
# 维护约束：scope 默认 all，矩阵层不生成半套仓库依赖。
# 维护约束：包过滤由单独 stage helper 提供，本矩阵默认完整清单。
# 维护约束：新增依赖 target 必须同步全部相关平台构建清单。
# 维护约束：新增归档必须同步各平台 staging 清单和 Find 模块。
# 维护约束：删除依赖前必须确认主仓和 ICE 均不再消费。
# 维护约束：输出二进制必须由 Git LFS 跟踪，不能直接进入 Git blob。
# 维护约束：staging 后调用方负责检查 LFS attributes 和工作树状态。
# 维护约束：预编译更新完成后必须执行每套 consumer 构建验证。
# 维护约束：consumer 验证必须使用 SOURCES_BUILD=OFF，禁止回退源码。
# 维护约束：跨平台无法在单一宿主运行的验证需由对应 CI Runner 完成。
# 维护约束：矩阵摘要必须在第一个子命令之前完整输出。
# 维护约束：runCommand 日志必须保持参数 shell 转义以便复现。
# 维护约束：命令数组不得改为拼接字符串或 eval。
# 维护约束：子命令退出状态直接决定矩阵是否继续。
# 维护约束：脚本不吞掉平台 helper 的 stdout 或 stderr。
# 维护约束：CMAKE_GENERATOR 只透传，不在矩阵层强制覆盖。
# 维护约束：自定义生成器必须由全部平台 helper 一致支持。
# 维护约束：自托管 Runner 资源路径变化应在对应平台脚本处理。
# 维护约束：本脚本不下载、安装或升级编译器和 SDK。
# 维护约束：本脚本不执行 git add、commit、push 或 LFS upload。
# 维护约束：本脚本不创建 release 包或网站发布内容。
# 维护约束：构建失败后的目录默认保留供诊断。
# 维护约束：fresh 清理由子脚本控制且不得触及项目根。
# 维护约束：成功退出表示启用矩阵的构建和 staging 均完成。
# 维护约束：成功结果仍需由调用方审查差异、符号和 LFS 状态。
# 维护约束：脚本新增选项必须同步 showUsage 与矩阵摘要。
# 维护约束：矩阵扩展到 macOS 前必须安排原生 Darwin Runner。
# 维护约束：动态预编译矩阵实现前不得放宽 static 校验。
# 维护约束：任务重跑前应先确认前一次没有并发进程占用构建目录。
# 维护约束：CI cache 键应包含脚本版本、工具链、配置和 linkage。
# 维护约束：预编译目录覆盖是可恢复 Git 工作树修改，不自动提交。
# 维护约束：任何不完整矩阵都应在提交说明中明确其 skip 范围。
# 维护约束：正式全量刷新不应使用任何 skip 或 reuse 开关。
# 维护约束：最终二进制集合必须与公共头文件来自同一依赖修订。
# 维护约束：主仓与 ICE 的预编译根必须分别由对应 staging wrapper 写入。
# 维护约束：矩阵层不应把主仓专属依赖写入引擎内部目录。
# 维护约束：stage helper 的默认 scope=all 是完整刷新前置条件。
# 维护约束：部分平台刷新只通过 skip 开关表达，不修改矩阵常量。
# 维护约束：部分配置刷新只通过 configs 参数表达，不改默认列表。
# 维护约束：默认值变化必须保留命令行覆盖能力以支持历史产物复现。
# 维护约束：构建目录名中的配置大小写应保持调用方原值。
# 维护约束：构建目录名不得包含未验证的工具链自由文本。
# 维护约束：矩阵 label 应完整显示 platform/toolchain/config 三元组。
# 维护约束：日志中的命令必须与实际执行数组完全相同。
# 维护约束：runCommand 不负责重试，避免重复 staging 隐藏首次失败。
# 维护约束：网络下载和工具安装由 CI 准备阶段完成。
# 维护约束：矩阵执行过程中不得切换源码 checkout 或依赖 submodule 修订。
# 维护约束：开始前应保证主仓和 ICE 子仓工作树状态可审查。
# 维护约束：staging 覆盖旧归档前应保留 Git 可恢复性。
# 维护约束：脚本不删除预编译目录中已不再生成的旧文件。
# 维护约束：移除旧产物必须单独核对 Find 模块后显式处理。
# 维护约束：PDB 清理由 MSVC staging 的 embedded-symbols 精确执行。
# 维护约束：MinGW staging 不应生成伪 PDB 或剥离归档内 CodeView。
# 维护约束：Linux staging 不应生成旁路 debug 文件而丢失归档调试段。
# 维护约束：每套归档的目标架构必须由平台 staging 验证。
# 维护约束：工具链生产的运行库类型必须与 static linkage 契约一致。
# 维护约束：RelWithDebInfo 不得错误映射到无调试信息的 Release 构建。
# 维护约束：Debug 文件后缀差异由 staging helper 按包处理。
# 维护约束：矩阵层不得猜测各上游库的实际输出文件名。
# 维护约束：新平台接入应实现独立 buildDir helper 和两个阶段函数。
# 维护约束：新工具链接入应提供对应 build 与 stage 脚本。
# 维护约束：新矩阵开关需加入空矩阵校验和摘要输出。
# 维护约束：平台级 skip 需覆盖该平台所有工具链单元。
# 维护约束：configure 阶段调用必须携带 configure-only。
# 维护约束：build 阶段调用不得携带 configure-only 或 fresh。
# 维护约束：stage 阶段只在对应 build 命令成功后执行。
# 维护约束：同一单元的 build 和 stage 必须使用相同 buildType。
# 维护约束：同一单元的 build 和 stage 必须使用相同 compiler tag。
# 维护约束：同一单元的 build 和 stage 必须使用相同 buildDir。
# 维护约束：完整刷新结束后应按文件检查注释、格式和 LFS 状态。
# 维护约束：提交预编译更新时应按依赖仓优先顺序逐级提交。
# 维护约束：发布预编译更新前应保留完整矩阵日志供追溯。
