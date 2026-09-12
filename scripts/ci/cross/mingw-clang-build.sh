#!/usr/bin/env bash
# 在 Linux 上配置并构建 Windows MinGW Clang x86_64 目标。
# 脚本支持完整 llvm-mingw UCRT 工具链，也兼容独立 Clang 配合 MSYS2 sysroot。
# 配置前验证 UCRT 导入库和 libc++ 头选择，防止混入 MSVCRT 或 libstdc++。
# 预编译模式按 ABI 坐标拉取 LFS 内容，源码模式构建 staging 所需依赖。
# 所有删除操作只作用于经过绝对路径解析和安全检查的构建目录。
set -euo pipefail

# 输出命令行契约和环境变量覆盖入口。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/mingw-clang-build.sh [options]

Configure and build the Windows MinGW clang cross target on Linux.

Options:
  --build-dir <path>      Build directory. Default: build_cross_mingw_clang
  --build-type <type>     CMake build type. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: clang64
  --jobs <count>          Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>        PROJECT_LINKAGE value: static or shared. Default: static
  --llvm-mingw-root <path>
                          Root of a complete llvm-mingw UCRT toolchain.
  --prefix <prefix>       MinGW tool prefix. Default: x86_64-w64-mingw32
  --sysroot <path>        MinGW sysroot. Default: ${WINDOWS_CROSS_ROOT}/msys64/clang64
  --toolchain <path>      CMake toolchain file. Default: cmake/toolchain/cross-mingw-clang.cmake
  --sources-build         Configure with SOURCES_BUILD=ON.
  --prebuilt-targets      Build only third-party targets used for staging.
  --configure-only        Configure and generate, then stop
  --fresh                 Remove the build directory before configuring
  -h, --help              Show this help

Environment overrides:
  MINGW_SYSROOT                     MinGW sysroot path
  MINGW_TOOLCHAIN_PREFIX            Default MinGW tool prefix
  MINGW_CLANG_PREBUILT_COMPILER_TAG Default prebuilt compiler tag
  LLVM_MINGW_ROOT                   Complete llvm-mingw UCRT toolchain root
  WINDOWS_CROSS_ROOT                Default: /mnt/cross/windows
  VULKAN_SDK                        Default: ${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0
  CMAKE_GENERATOR                   Default: Ninja
EOF
}

# 使用可用 CPU 线程的约四分之三作为默认并行度。
detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        # Linux 优先使用 nproc 感知容器限制。
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        # POSIX getconf 作为兼容回退。
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        # Windows 风格环境变量用于部分混合 Runner。
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        # 异常探测结果保守回退到单线程。
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    # 整数除法后仍保证至少一个任务。
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

# 要求唯一指定命令存在，否则输出统一错误。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 构建脚本不尝试自动安装宿主工具。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 要求候选命令列表中至少存在一个兼容实现。
requireAnyCommand() {
    local commandName

    for commandName in "$@"; do
        # 顺序表达优先实现，但此 helper 只负责可用性检查。
        if command -v "${commandName}" >/dev/null 2>&1; then
            # 任一候选可用即满足工具链契约。
            return 0
        fi
    done

    printf "error: none of the required commands were found:" >&2
    # 输出所有候选名，便于定位 PATH 配置问题。
    for commandName in "$@"; do
        printf " %s" "${commandName}" >&2
    done
    printf "\n" >&2
    exit 1
}

# 将相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许使用外部工具链和构建磁盘。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用脚本时的当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 探测完整 llvm-mingw UCRT 分发根目录。
detectLlvmMingwRoot() {
    if [[ -n "${LLVM_MINGW_ROOT:-}" ]]; then
        # 显式环境覆盖拥有最高优先级。
        printf "%s\n" "${LLVM_MINGW_ROOT}"
        return
    fi

    local candidate
    # 兼容固定版本目录、版本通配目录与系统级安装。
    for candidate in "${HOME:-}/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64" "${HOME:-}"/llvm-mingw-*-ucrt-*-x86_64 /opt/llvm-mingw; do
        if [[ -d "${candidate}/bin" ]]; then
            # bin 存在是可作为工具链根的最低条件。
            printf "%s\n" "${candidate}"
            return
        fi
    done

    return 0
}

# 根据显式值、llvm-mingw、MSYS2 和系统前缀依次选择 sysroot。
detectMingwSysroot() {
    local toolPrefix="$1"
    local llvmMingwRoot="$2"

    if [[ -n "${MINGW_SYSROOT:-}" ]]; then
        # 调用方显式 sysroot 不再执行自动探测。
        printf "%s\n" "${MINGW_SYSROOT}"
        return
    fi

    if [[ -n "${llvmMingwRoot}" ]]; then
        # 完整 llvm-mingw 优先询问目标编译器的实际 sysroot。
        local detectedSysroot=""
        if command -v "${toolPrefix}-clang" >/dev/null 2>&1; then
            # 编译器可能已由 llvm-mingw bin 注入 PATH。
            detectedSysroot="$("${toolPrefix}-clang" --print-sysroot 2>/dev/null || true)"
        fi
        if [[ -n "${detectedSysroot}" && -d "${detectedSysroot}" ]]; then
            # 只接受实际存在的编译器返回目录。
            printf "%s\n" "${detectedSysroot}"
            return
        fi

        local targetSysroot="${llvmMingwRoot}/${toolPrefix}"
        # 部分分发把目标 sysroot 放在三元组子目录。
        if [[ -d "${targetSysroot}/include" && -d "${targetSysroot}/lib" ]]; then
            # include 与 lib 同时存在才视为完整目标根。
            printf "%s\n" "${targetSysroot}"
            return
        fi

        printf "%s\n" "${llvmMingwRoot}"
        # 旧版平铺分发直接以工具链根作为最后兼容值。
        return
    fi

    local windowsCrossRoot="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
    # 默认外部 Windows 资源根可由 CI 环境覆盖。
    local msys2Clang64Sysroot="${windowsCrossRoot}/msys64/clang64"
    if [[ -d "${msys2Clang64Sysroot}/include" && -d "${msys2Clang64Sysroot}/lib" ]]; then
        # MSYS2 clang64 根同时提供 libc++ 和 UCRT 导入库。
        printf "%s\n" "${msys2Clang64Sysroot}"
        return
    fi

    local prefixedSysroot="/usr/${toolPrefix}"
    # 发行版交叉工具链通常安装到 /usr/<target>。
    if [[ -d "${prefixedSysroot}" ]]; then
        printf "%s\n" "${prefixedSysroot}"
        return
    fi

    if command -v "${toolPrefix}-gcc" >/dev/null 2>&1; then
        # GCC 仅用于探测共享的 MinGW sysroot，不参与 Clang 编译。
        local detectedSysroot
        detectedSysroot="$("${toolPrefix}-gcc" -print-sysroot)"
        if [[ -n "${detectedSysroot}" ]]; then
            # 非空结果由后续目录存在性检查验证。
            printf "%s\n" "${detectedSysroot}"
            return
        fi
    fi

    printf "/usr/x86_64-w64-mingw32\n"
}

# 确认完整 llvm-mingw 使用 UCRT 兼容导入库而非旧 MSVCRT。
verifyLlvmMingwUcrtRuntime() {
    local llvmMingwRoot="$1"
    local mingwSysroot="$2"

    if [[ -z "${llvmMingwRoot}" ]]; then
        # 非 llvm-mingw 路径由所选 sysroot 自身负责运行库契约。
        return
    fi

    local msvcrtImport="${mingwSysroot}/lib/libmsvcrt.a"
    # UCRT 导入库是该工作流的强制依赖。
    local ucrtImport="${mingwSysroot}/lib/libucrt.a"
    if [[ ! -f "${ucrtImport}" ]]; then
        # 缺失时禁止继续到链接阶段产生更隐晦错误。
        printf "error: llvm-mingw sysroot does not provide UCRT import library: %s\n" "${ucrtImport}" >&2
        exit 1
    fi

    if [[ -f "${msvcrtImport}" ]]; then
        # 兼容名存在时必须与 UCRT 归档内容完全相同。
        if ! cmp -s "${msvcrtImport}" "${ucrtImport}"; then
            # 不同内容意味着工具链可能链接到错误 C 运行库。
            printf "error: llvm-mingw libmsvcrt.a is not the UCRT compatibility import library: %s\n" "${msvcrtImport}" >&2
            exit 1
        fi
        printf "info: llvm-mingw UCRT runtime verified: libmsvcrt.a aliases libucrt.a\n"
        return
    fi

    # 没有兼容别名也可以，只要显式 UCRT 导入库存在。
    printf "info: llvm-mingw UCRT runtime verified: %s\n" "${ucrtImport}"
}

# 通过预处理探针确认 Windows GNU 目标实际选择 libc++。
verifyMingwClangCxxRuntime() {
    local toolPrefix="$1"
    local mingwSysroot="$2"
    local cxxCompiler=""

    if command -v "${toolPrefix}-clang++" >/dev/null 2>&1; then
        # 目标前缀编译器最能表达完整工具链配置。
        cxxCompiler="${toolPrefix}-clang++"
    elif command -v clang++-22 >/dev/null 2>&1; then
        # 版本化宿主 Clang 是标准 CI 回退。
        cxxCompiler="clang++-22"
    elif command -v clang++ >/dev/null 2>&1; then
        # 无版本命令仅在前两项缺失时使用。
        cxxCompiler="clang++"
    else
        # 无 C++ 编译器无法验证标准库头来源。
        printf "error: unable to find MinGW clang++ for libc++ verification\n" >&2
        exit 1
    fi

    local macroOutput
    # 显式目标、sysroot 和 -stdlib 避免继承宿主默认值。
    if ! macroOutput="$(printf "#include <vector>\n" | "${cxxCompiler}" --target=x86_64-w64-windows-gnu --sysroot="${mingwSysroot}" -stdlib=libc++ -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: unable to preprocess libc++ probe with %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    if ! grep -q "_LIBCPP_VERSION" <<<"${macroOutput}"; then
        # 缺少 libc++ 标识表明 sysroot 或 include 搜索路径错误。
        printf "error: MinGW clang++ is not using libc++ headers\n" >&2
        exit 1
    fi
    if grep -q "__GLIBCXX__" <<<"${macroOutput}"; then
        # 同时出现 libstdc++ 标识意味着头文件污染。
        printf "error: MinGW clang++ picked up libstdc++ headers\n" >&2
        exit 1
    fi

    printf "info: MinGW clang C++ runtime verified: libc++ (_LIBCPP_VERSION)\n"
}

# 通过脚本自身位置定位三层之外的项目根。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_clang"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
# compiler tag 进入预编译 ABI 目录。
compilerTag="${MINGW_CLANG_PREBUILT_COMPILER_TAG:-clang64}"
projectLinkage="static"
# 工具前缀决定目标三元组可执行文件名。
toolPrefix="${MINGW_TOOLCHAIN_PREFIX:-x86_64-w64-mingw32}"
llvmMingwRoot="$(detectLlvmMingwRoot)"
mingwSysroot=""
# CMake toolchain 负责把探测结果映射为编译链接参数。
toolchainFile="cmake/toolchain/cross-mingw-clang.cmake"
sourcesBuild="OFF"
# 流程开关分别控制目标集合、停止点和构建树生命周期。
prebuiltTargets=0
configureOnly=0
freshBuild=0

# 在修改 PATH 或构建树前完整解析参数。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 构建目录稍后统一解析为绝对路径。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # 配置名同时用于 CMake 与预编译目录选择。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            # 标签必须与生成依赖归档的工具链集合一致。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --jobs)
            # 显式并行度覆盖 CPU 自动探测值。
            if (( $# < 2 )); then
                printf "error: --jobs requires a value\n" >&2
                exit 1
            fi
            buildJobs="$2"
            shift 2
            ;;
        --linkage)
            # 主项目与 ICE 使用相同链接偏好。
            if (( $# < 2 )); then
                printf "error: --linkage requires a value\n" >&2
                exit 1
            fi
            projectLinkage="$2"
            shift 2
            ;;
        --llvm-mingw-root)
            # 完整工具链根会在解析后加入 PATH。
            if (( $# < 2 )); then
                printf "error: --llvm-mingw-root requires a value\n" >&2
                exit 1
            fi
            llvmMingwRoot="$2"
            shift 2
            ;;
        --prefix)
            # 前缀必须与目标 sysroot 的三元组一致。
            if (( $# < 2 )); then
                printf "error: --prefix requires a value\n" >&2
                exit 1
            fi
            toolPrefix="$2"
            shift 2
            ;;
        --sysroot)
            # 显式 sysroot 跳过自动候选选择。
            if (( $# < 2 )); then
                printf "error: --sysroot requires a value\n" >&2
                exit 1
            fi
            mingwSysroot="$2"
            shift 2
            ;;
        --toolchain)
            # 自定义 toolchain 路径仍以项目根解析。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            toolchainFile="$2"
            shift 2
            ;;
        --sources-build)
            # 源码模式构建 staging 需要的第三方依赖。
            sourcesBuild="ON"
            shift
            ;;
        --prebuilt-targets)
            # 只构建预编译发布清单中的 target。
            prebuiltTargets=1
            shift
            ;;
        --configure-only)
            # 配置成功后不执行实际编译。
            configureOnly=1
            shift
            ;;
        --fresh)
            # 删除动作受后续绝对路径安全检查保护。
            freshBuild=1
            shift
            ;;
        -h | --help)
            # 帮助路径不要求工具链存在。
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

if [[ -n "${llvmMingwRoot}" ]]; then
    # 路径规范化后再验证 bin，避免相对路径依赖当前目录。
    llvmMingwRoot="$(projectPath "${llvmMingwRoot}")"
    if [[ ! -d "${llvmMingwRoot}/bin" ]]; then
        # 完整分发缺少 bin 时不能作为 LLVM_MINGW_ROOT。
        printf "error: LLVM_MINGW_ROOT does not contain a bin directory: %s\n" "${llvmMingwRoot}" >&2
        exit 1
    fi
    export LLVM_MINGW_ROOT="${llvmMingwRoot}"
    # 仅在本脚本进程及子进程中优先选择完整工具链命令。
    export PATH="${llvmMingwRoot}/bin:${PATH}"
fi

if [[ -z "${mingwSysroot}" ]]; then
    # 参数没有固定 sysroot 时才运行多级探测。
    mingwSysroot="$(detectMingwSysroot "${toolPrefix}" "${llvmMingwRoot}")"
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # CMake 并行度仅接受正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    # 链接偏好参与预编译布局和运行库选择。
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${compilerTag}" ]]; then
    # 空标签会打破不同工具链产物隔离。
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

if [[ "${sourcesBuild}" == "OFF" ]]; then
    # 消费模式在 CMake 配置前拉取精确 LFS 对象。
    bash "${scriptDir}/../pull-lfs-for-build.sh" \
        --platform windows \
        --arch x86_64 \
        --toolchain mingw \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}"
fi

buildDir="$(projectPath "${buildDir}")"
# toolchain 与 sysroot 也允许传入项目相对路径。
toolchainFile="$(projectPath "${toolchainFile}")"
mingwSysroot="$(projectPath "${mingwSysroot}")"

if [[ ! -f "${toolchainFile}" ]]; then
    # 缺失 toolchain 时不允许 CMake 猜测宿主编译器。
    printf "error: toolchain file not found: %s\n" "${toolchainFile}" >&2
    exit 1
fi

export MINGW_SYSROOT="${mingwSysroot}"
# 下游 CMake 与 Meson 子构建共享相同目标前缀和 ABI 标签。
export MINGW_TOOLCHAIN_PREFIX="${toolPrefix}"
export MINGW_CLANG_PREBUILT_COMPILER_TAG="${compilerTag}"
export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

requireCommand cmake
if [[ -n "${llvmMingwRoot}" ]]; then
    # 完整分发接受目标前缀命令或版本化宿主命令。
    requireAnyCommand "${toolPrefix}-clang" clang-22 clang
    requireAnyCommand "${toolPrefix}-clang++" clang++-22 clang++
else
    # 非完整分发依赖 CI 固定的 Clang 22 可执行文件。
    requireCommand clang-22
    requireCommand clang++-22
fi
requireAnyCommand "${toolPrefix}-windres" llvm-windres
# 二进制工具允许目标前缀包装器或 LLVM 通用命令。
requireAnyCommand "${toolPrefix}-ar" llvm-ar
requireAnyCommand "${toolPrefix}-ranlib" llvm-ranlib
requireAnyCommand "${toolPrefix}-strip" llvm-strip
requireAnyCommand "${toolPrefix}-objcopy" llvm-objcopy

if [[ ! -d "${MINGW_SYSROOT}" ]]; then
    # 所有自动或显式候选最终都必须解析为现有目录。
    printf "error: MINGW_SYSROOT does not exist: %s\n" "${MINGW_SYSROOT}" >&2
    exit 1
fi
# 运行库和 C++ 标准库验证必须早于 CMake 配置。
verifyLlvmMingwUcrtRuntime "${llvmMingwRoot}" "${MINGW_SYSROOT}"
verifyMingwClangCxxRuntime "${toolPrefix}" "${MINGW_SYSROOT}"

if [[ ! -d "${VULKAN_SDK}" ]]; then
    # Windows 目标必须使用可用的 Windows Vulkan SDK。
    printf "error: VULKAN_SDK does not exist: %s\n" "${VULKAN_SDK}" >&2
    exit 1
fi

if (( freshBuild )); then
    # 拒绝空路径、文件系统根和项目根三个高风险删除目标。
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

# 交叉编译不得写入宿主机的用户配置目录。
# CMake 参数明确传递目标工具链、依赖来源和两级 ABI 标签。
cmake -G "${CMAKE_GENERATOR:-Ninja}" \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchainFile}" \
    -DLLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-}" \
    -DMINGW_SYSROOT="${MINGW_SYSROOT}" \
    -DMINGW_TOOLCHAIN_PREFIX="${toolPrefix}" \
    -DSOURCES_BUILD="${sourcesBuild}" \
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DICE_LINKAGE="${projectLinkage}" \
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DICE_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if (( configureOnly )); then
    # 配置探针成功后立即停止。
    exit 0
fi

if (( prebuiltTargets )); then
    # 清单只覆盖 staging 所需第三方归档。
    cmake --build "${buildDir}" --parallel "${buildJobs}" --target \
        zlib_project \
        lame_project \
        ffmpeg_project \
        fftw_project \
        rubberband_project \
        samplerate \
        IonCachyEngine-static \
        3rd_implot \
        3rd_miniz \
        imgui-static \
        freetype \
        glfw \
        ImGuiFileDialog \
        nfd \
        lunasvg \
        plutovg \
        libcurl_static \
        fmt \
        spdlog \
        OpenAL \
        SDL3-static \
        luajit_build \
        datachannel-static
else
    # 常规模式构建默认 all target。
    cmake --build "${buildDir}" --parallel "${buildJobs}"
fi

# 维护约束：目标平台固定 Windows x86_64，不得继承 Linux 宿主 ABI。
# 维护约束：MinGW Clang 路径必须使用 Windows GNU target triple。
# 维护约束：完整 llvm-mingw 分发必须明确提供 UCRT 运行库。
# 维护约束：libmsvcrt.a 存在时必须是 libucrt.a 的兼容别名。
# 维护约束：运行库校验不得仅依赖文件名或目录名。
# 维护约束：libc++ 探针必须显式包含一个标准库头。
# 维护约束：探针同时拒绝任何意外选中的 libstdc++ 头。
# 维护约束：标准库验证失败时不得继续生成 CMake cache。
# 维护约束：LLVM_MINGW_ROOT 只接受包含 bin 子目录的完整根。
# 维护约束：把工具链 bin 加入 PATH 只影响当前脚本子进程。
# 维护约束：目标前缀必须与 llvm-mingw sysroot 子目录一致。
# 维护约束：显式 MINGW_SYSROOT 拥有高于自动探测的优先级。
# 维护约束：自动探测只返回候选，不替调用方创建目录。
# 维护约束：MSYS2 clang64 sysroot 必须同时含 include 与 lib。
# 维护约束：GCC sysroot 查询仅用于定位，不改变 Clang 编译器选择。
# 维护约束：工具链候选顺序从完整目标命令到通用 LLVM 命令。
# 维护约束：windres、ar、ranlib、strip 与 objcopy 必须来自兼容工具集。
# 维护约束：不得让宿主 GNU binutils 处理 Windows COFF 产物。
# 维护约束：VULKAN_SDK 必须指向 Windows 目标 SDK 而非 Linux SDK。
# 维护约束：WINDOWS_CROSS_ROOT 只提供默认路径，不覆盖显式 SDK。
# 维护约束：toolchain 文件必须在配置前验证为普通文件。
# 维护约束：buildDir、toolchain 和 sysroot 均在项目根规则下解析。
# 维护约束：不同 compiler tag 的构建不得共用 CMake cache。
# 维护约束：源码依赖与预编译消费构建应使用独立目录。
# 维护约束：SOURCES_BUILD 是依赖来源唯一开关，不允许自动回退。
# 维护约束：预编译模式必须在配置前完成精确 LFS 拉取。
# 维护约束：LFS 拉取坐标必须使用 windows/x86_64/mingw。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 必须保持一致。
# 维护约束：shared 模式不得消费 static 配置路径。
# 维护约束：静态模式不得混入 shared 运行时布局。
# 维护约束：交叉构建禁止同步翻译或默认皮肤到宿主配置。
# 维护约束：PGO 插桩和使用在该交叉流程中保持关闭。
# 维护约束：第三方源码依赖构建也不得启用主项目 PGO。
# 维护约束：所有 CMake 参数保持双引号以支持路径空格。
# 维护约束：续行反斜杠之后不得插入注释或额外 token。
# 维护约束：所有删除目标必须先转换为绝对路径。
# 维护约束：删除防护不得放宽到项目根或文件系统根。
# 维护约束：fresh 不应删除 llvm-mingw、sysroot 或 Vulkan SDK。
# 维护约束：configure-only 不应要求任何目标归档已经生成。
# 维护约束：prebuilt-targets 清单需与 staging 清单保持同步。
# 维护约束：清单中的 target 名必须与 CMake 实际导出一致。
# 维护约束：常规模式只请求默认 all target，不额外执行测试。
# 维护约束：构建失败直接传播 CMake 或底层生成器退出状态。
# 维护约束：脚本不执行 strip，调试信息由发布策略处理。
# 维护约束：脚本不复制归档到预编译目录。
# 维护约束：脚本不负责 Git LFS add、commit 或 push。
# 维护约束：编译器升级时需同步默认 compiler tag 和 CI 镜像。
# 维护约束：llvm-mingw 默认版本目录变化时需同步候选列表。
# 维护约束：Vulkan SDK 版本变化时需同步 Windows 交叉环境。
# 维护约束：新增命令行参数必须同步 showUsage 文本。
# 维护约束：错误日志应保留实际工具、sysroot 和目标路径。
# 维护约束：信息日志只报告已完成的运行库验证结论。
# 维护约束：成功退出只表示请求目标构建完成，不表示已 staging。
# 维护约束：正式发布前必须消费 staging 后的预编译集合再构建。
# 维护约束：Windows 可执行文件的运行测试由后续 Windows 环境完成。
# 维护约束：构建目录不得保存与另一种 MinGW 运行库混合的 cache。
# 维护约束：UCRT 兼容性是工具链选择的一部分，不能事后改库补救。
# 维护约束：libc++ 与 libc++abi 版本应来自同一 sysroot 分发。
# 维护约束：完整工具链和 MSYS2 sysroot 路径不得在一次构建中混搭。
# 维护约束：成功完成 runtime probe 才能视为工具链环境可用。
# 维护约束：llvm-mingw 根探测不得把空 HOME 扩展为文件系统根通配。
# 维护约束：自动候选必须以 bin 子目录存在作为最低判断条件。
# 维护约束：目标前缀编译器优先于通用 Clang 以保留分发默认参数。
# 维护约束：通用 Clang 路径必须显式传入 target 和 sysroot。
# 维护约束：libc++ 探针仅预处理，不应产生目标对象或写入构建树。
# 维护约束：预处理诊断被隐藏时仍以命令退出码判定失败。
# 维护约束：UCRT 归档比较必须使用字节级一致性检查。
# 维护约束：兼容 import library 不得通过软性警告绕过内容差异。
# 维护约束：sysroot 根回退仅兼容旧版平铺 llvm-mingw 分发。
# 维护约束：新增分发布局应作为更具体候选插入旧回退之前。
# 维护约束：PATH 注入后所有目标工具检查必须使用同一环境。
# 维护约束：构建过程不得在中途切换 LLVM_MINGW_ROOT。
# 维护约束：MINGW_TOOLCHAIN_PREFIX 应显式传给所有第三方子构建。
# 维护约束：预编译 consumer 与 source producer 必须使用相同 C++ 运行库。
# 维护约束：归档工具选择需与 COFF 长文件名和 CodeView 支持兼容。
# 维护约束：strip 工具仅验证可用，本脚本本身不剥离产物。
# 维护约束：objcopy 工具仅供下游 target 使用，不在编排层改写对象。
# 维护约束：错误的宿主标准库头不能通过增加 include 路径事后掩盖。
# 维护约束：运行库探针变化需同时更新 toolchain 文件的标准库参数。
# 维护约束：预编译 tag clang64 表示工具链集合而非 Clang 主版本。
# 维护约束：若 tag 语义变化需迁移已有预编译目录而非原地覆盖。
# 维护约束：同一 buildDir 的工具链根、sysroot 和 tag 必须保持稳定。
# 维护约束：切换依赖来源或链接方式时应使用 fresh 或新目录。
# 维护约束：构建日志必须保留 UCRT 与 libc++ 两项验证成功信息。
# 维护约束：CI 缓存键应包含工具链版本、sysroot 和构建配置。
# 维护约束：脚本尾部不执行隐式清理，便于失败后检查构建产物。
