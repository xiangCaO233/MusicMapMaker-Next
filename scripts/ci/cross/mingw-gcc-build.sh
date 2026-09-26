#!/usr/bin/env bash
# 在 Linux 上配置并构建 Windows MinGW GCC x86_64 目标。
# 工具前缀、sysroot、compiler tag 与预编译目录共同定义目标 ABI。
# 预编译模式先拉取精确 LFS 内容，源码模式生成 staging 所需依赖。
# 所有工具和 SDK 在 CMake 配置前验证，避免生成不可用 cache。
# --fresh 只删除经过绝对路径解析和安全检查的构建目录。
set -euo pipefail

# 输出参数契约和环境覆盖入口。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/mingw-gcc-build.sh [options]

Configure and build the Windows MinGW GCC cross target on Linux.

Options:
  --build-dir <path>      Build directory. Default: build_cross_mingw_gcc
  --build-type <type>     CMake build type. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: ucrt64
  --jobs <count>          Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>        PROJECT_LINKAGE value: static or shared. Default: static
  --prefix <prefix>       MinGW tool prefix. Default: x86_64-w64-mingw32ucrt
  --sysroot <path>        MinGW sysroot. Default: <prefix>-gcc -print-sysroot, then /usr/<prefix>
  --toolchain <path>      CMake toolchain file. Default: cmake/toolchain/cross-mingw-gcc.cmake
  --sources-build         Configure with SOURCES_BUILD=ON.
  --vulkan-validation-layers Enable Vulkan validation layers. Default: disabled.
  --prebuilt-targets      Build only third-party targets used for staging.
  --configure-only        Configure and generate, then stop
  --fresh                 Remove the build directory before configuring
  -h, --help              Show this help

Environment overrides:
  MINGW_SYSROOT                    MinGW sysroot path
  MINGW_GCC_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
  WINDOWS_CROSS_ROOT               Default: /mnt/cross/windows
  VULKAN_SDK                       Default: ${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0
  CMAKE_GENERATOR                  Default: Ninja
EOF
}

# 使用在线处理器约四分之三作为默认并行度。
detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        # Linux 优先使用 nproc 感知容器限制。
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        # POSIX getconf 作为兼容回退。
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        # 混合 Runner 可使用 Windows 风格环境变量。
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        # 非法探测结果保守回退单线程。
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    # 整数除法后至少保留一个任务。
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

# 验证指定构建命令可从 PATH 调用。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 脚本不尝试隐式安装交叉工具链。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 将相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许使用外部 sysroot 或构建磁盘。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用者当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 按环境、编译器报告和发行版惯例选择 MinGW sysroot。
detectMingwSysroot() {
    local toolPrefix="$1"

    if [[ -n "${MINGW_SYSROOT:-}" ]]; then
        # 显式环境覆盖拥有最高优先级。
        printf "%s\n" "${MINGW_SYSROOT}"
        return
    fi

    if command -v "${toolPrefix}-gcc" >/dev/null 2>&1; then
        # 优先询问目标编译器自身配置的 sysroot。
        local detectedSysroot
        detectedSysroot="$("${toolPrefix}-gcc" -print-sysroot)"
        if [[ -n "${detectedSysroot}" ]]; then
            # 非空结果稍后还要经过目录存在性验证。
            printf "%s\n" "${detectedSysroot}"
            return
        fi
    fi

    local prefixedSysroot="/usr/${toolPrefix}"
    # 发行版交叉工具链通常采用目标三元组目录。
    if [[ -d "${prefixedSysroot}" ]]; then
        # 现有前缀目录优先于固定兼容回退。
        printf "%s\n" "${prefixedSysroot}"
        return
    fi

    printf "/usr/x86_64-w64-mingw32ucrt\n"
}

# 通过脚本自身位置定位项目根和 CI helper。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_gcc"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
compilerTag="${MINGW_GCC_PREBUILT_COMPILER_TAG:-ucrt64}"
projectLinkage="static"
# 工具前缀决定编译器、binutils 和默认 sysroot 名称。
toolPrefix="x86_64-w64-mingw32ucrt"
mingwSysroot=""
toolchainFile="cmake/toolchain/cross-mingw-gcc.cmake"
sourcesBuild="OFF"
# 每次配置都写入验证层状态，避免旧 CMake 缓存残留。
vulkanValidationLayers="OFF"
# 流程开关分别控制目标集合、停止点和目录生命周期。
prebuiltTargets=0
configureOnly=0
freshBuild=0

# 在拉取依赖或修改构建树前完整解析参数。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 路径稍后统一解析为绝对路径。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # 配置名同时用于 CMake 和预编译目录选择。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            # 标签必须与目标归档的 ABI 产物集一致。
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
            # 主项目与 ICE 始终采用相同链接偏好。
            if (( $# < 2 )); then
                printf "error: --linkage requires a value\n" >&2
                exit 1
            fi
            projectLinkage="$2"
            shift 2
            ;;
        --prefix)
            # 自定义前缀必须同时提供对应的所有 binutils。
            if (( $# < 2 )); then
                printf "error: --prefix requires a value\n" >&2
                exit 1
            fi
            toolPrefix="$2"
            shift 2
            ;;
        --sysroot)
            # 显式路径跳过自动 sysroot 探测。
            if (( $# < 2 )); then
                printf "error: --sysroot requires a value\n" >&2
                exit 1
            fi
            mingwSysroot="$2"
            shift 2
            ;;
        --toolchain)
            # 自定义 toolchain 仍按项目根解析。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            toolchainFile="$2"
            shift 2
            ;;
        --sources-build)
            # 源码模式构建 staging 所需第三方 target。
            sourcesBuild="ON"
            shift
            ;;
        --vulkan-validation-layers)
            # 显式启用目标程序的 Vulkan 验证层。
            vulkanValidationLayers="ON"
            shift
            ;;
        --prebuilt-targets)
            # 只请求依赖归档，不构建业务程序。
            prebuiltTargets=1
            shift
            ;;
        --configure-only)
            # 配置成功后立即停止。
            configureOnly=1
            shift
            ;;
        --fresh)
            # 删除动作受后续路径安全检查保护。
            freshBuild=1
            shift
            ;;
        -h | --help)
            # 帮助成功退出且不验证工具链。
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

if [[ -z "${mingwSysroot}" ]]; then
    # 仅在参数没有固定 sysroot 时运行自动探测。
    mingwSysroot="$(detectMingwSysroot "${toolPrefix}")"
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # CMake 并行度仅接受正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    # 链接偏好参与预编译布局与运行库选择。
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${compilerTag}" ]]; then
    # 空标签会破坏不同工具链产物隔离。
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

if [[ "${sourcesBuild}" == "OFF" ]]; then
    # 消费模式在配置前拉取 windows/x86_64/mingw 精确对象。
    bash "${scriptDir}/../pull-lfs-for-build.sh" \
        --platform windows \
        --arch x86_64 \
        --toolchain mingw \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}"
fi

buildDir="$(projectPath "${buildDir}")"
# toolchain 和 sysroot 同样支持项目相对路径。
toolchainFile="$(projectPath "${toolchainFile}")"
mingwSysroot="$(projectPath "${mingwSysroot}")"

if [[ ! -f "${toolchainFile}" ]]; then
    # 缺失 toolchain 时禁止 CMake 回退到宿主编译器。
    printf "error: toolchain file not found: %s\n" "${toolchainFile}" >&2
    exit 1
fi

export MINGW_SYSROOT="${mingwSysroot}"
# 下游 CMake 与第三方子构建共享同一 ABI 标签。
export MINGW_GCC_PREBUILT_COMPILER_TAG="${compilerTag}"
export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

requireCommand cmake
# 编译、资源和归档工具必须全部来自相同目标前缀。
requireCommand "${toolPrefix}-gcc"
requireCommand "${toolPrefix}-g++"
requireCommand "${toolPrefix}-windres"
requireCommand "${toolPrefix}-ar"
requireCommand "${toolPrefix}-ranlib"
requireCommand "${toolPrefix}-strip"
requireCommand "${toolPrefix}-objcopy"

if [[ ! -d "${MINGW_SYSROOT}" ]]; then
    # 自动或显式 sysroot 最终都必须解析为现有目录。
    printf "error: MINGW_SYSROOT does not exist: %s\n" "${MINGW_SYSROOT}" >&2
    exit 1
fi

if [[ ! -d "${VULKAN_SDK}" ]]; then
    # Windows 目标使用外部 Windows Vulkan SDK。
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
# CMake 参数明确固定目标 sysroot、依赖来源和两级 ABI 标签。
# 每次配置明确覆盖旧缓存，避免上次诊断构建影响默认产物。
cmake -G "${CMAKE_GENERATOR:-Ninja}" \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DMMM_ENABLE_VULKAN_VALIDATION_LAYERS="${vulkanValidationLayers}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchainFile}" \
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
    # 配置探针成功后不执行实际构建。
    exit 0
fi

if (( prebuiltTargets )); then
    # 清单只包含后续 staging 所需第三方归档。
    # Xiph 归档必须与当前 ucrt64 工具链和配置一起构建。
    cmake --build "${buildDir}" --parallel "${buildJobs}" --target \
        zlib_project \
        lame_project \
        ogg_project \
        vorbis_project \
        opus_project \
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
# 维护约束：工具前缀必须对应 UCRT MinGW 分发。
# 维护约束：编译器和全部 binutils 必须使用同一目标前缀。
# 维护约束：不得让宿主 ar、strip 或 objcopy 处理 COFF 产物。
# 维护约束：显式 MINGW_SYSROOT 优先于编译器和目录自动探测。
# 维护约束：自动探测只返回路径，不创建或修复 sysroot。
# 维护约束：compiler tag 必须与预编译归档实际来源一致。
# 维护约束：不同 compiler tag 构建不得共用 CMake cache。
# 维护约束：VULKAN_SDK 必须指向 Windows 目标 SDK。
# 维护约束：WINDOWS_CROSS_ROOT 只提供默认值，不覆盖显式 SDK。
# 维护约束：toolchain 文件必须在配置前验证存在。
# 维护约束：源码依赖与预编译消费构建应使用独立目录。
# 维护约束：SOURCES_BUILD 是依赖来源唯一开关，不允许自动回退。
# 维护约束：预编译模式必须在 CMake 配置前完成 LFS 拉取。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 必须保持一致。
# 维护约束：shared 模式不得消费 static 预编译目录。
# 维护约束：静态模式不得混入 shared 运行时布局。
# 维护约束：交叉构建禁止同步翻译或默认皮肤到宿主配置。
# 维护约束：PGO 插桩和使用在该交叉流程中保持关闭。
# 维护约束：所有续行参数必须保持双引号和反斜杠结构。
# 维护约束：续行反斜杠之后不得插入注释。
# 维护约束：所有删除目标必须先转换为绝对路径。
# 维护约束：删除防护不得放宽到项目根或文件系统根。
# 维护约束：fresh 不应删除 sysroot、toolchain 或 Vulkan SDK。
# 维护约束：configure-only 不应要求任何目标归档已生成。
# 维护约束：prebuilt-targets 清单需与 staging 清单保持同步。
# 维护约束：第三方 target 名必须与 CMake 实际导出一致。
# 维护约束：常规模式不额外执行测试或打包。
# 维护约束：构建失败直接传播 CMake 或生成器退出状态。
# 维护约束：脚本不执行 strip，调试信息由发布流程管理。
# 维护约束：脚本不负责 staging、LFS add、commit 或 push。
# 维护约束：编译器升级时需同步默认前缀、tag 和 CI 镜像。
# 维护约束：Vulkan SDK 版本变化时需同步交叉环境默认值。
# 维护约束：新增参数必须同步 showUsage 文本。
# 维护约束：成功退出只表示请求目标构建完成，不表示已发布。
# 维护约束：正式发布前必须消费 staging 后的依赖集合再构建。
# 维护约束：Windows 可执行文件运行测试由后续目标环境完成。
# 维护约束：构建目录不得混用 UCRT 与旧 MSVCRT 运行库配置。
# 维护约束：sysroot、工具前缀和 compiler tag 是不可拆分的 ABI 坐标。
# 维护约束：任何默认路径变更都需同步文档和 CI workflow。
# 维护约束：默认前缀末尾 ucrt 标识是运行库契约的一部分。
# 维护约束：工具前缀覆盖不会自动改写 compiler tag。
# 维护约束：compiler tag 覆盖不会自动改写工具前缀。
# 维护约束：调用方必须成套提供自定义 prefix、sysroot 和 tag。
# 维护约束：编译器返回空 sysroot 时继续执行目录候选回退。
# 维护约束：固定兼容回退仍需通过后续目录存在性检查。
# 维护约束：sysroot 路径中的 include 和 lib 布局由 toolchain 验证。
# 维护约束：MINGW_SYSROOT 环境同时传递给第三方子构建。
# 维护约束：MINGW_GCC_PREBUILT_COMPILER_TAG 只描述产物目录标签。
# 维护约束：资源编译器 windres 必须支持目标 Windows 资源格式。
# 维护约束：ranlib 必须能更新同一前缀 ar 生成的 COFF 索引。
# 维护约束：strip 与 objcopy 只供下游使用，编排层不改写产物。
# 维护约束：CMake toolchain 负责固定 C 与 C++ 目标编译器。
# 维护约束：主脚本不得通过 CC/CXX 环境覆盖交叉 toolchain。
# 维护约束：构建树 cache 必须记录请求的 sysroot 与工具前缀。
# 维护约束：切换 sysroot 后应使用 fresh 或全新构建目录。
# 维护约束：切换 UCRT 工具链版本后必须重新构建全部依赖。
# 维护约束：预编译产物应保留 COFF CodeView 调试信息。
# 维护约束：MinGW 静态归档不要求旁路 MSVC PDB。
# 维护约束：staging 脚本应验证归档格式与架构。
# 维护约束：CI cache 键应包含 prefix、sysroot、tag 与 buildType。
# 维护约束：发布日志应保留实际 sysroot 和工具链命令版本。
# 维护约束：后续测试需在 Windows 或兼容运行环境执行。
# 维护约束：脚本尾部不隐式清理，便于失败后检查产物。
# 维护约束：成功后调用方负责 staging 或普通构建验证。
# 维护约束：新增工具候选必须保持同一目标前缀优先。
# 维护约束：默认路径变更不得绕过工具链存在性检查。
# 维护约束：所有交叉构建输入应可由 CI 环境明确复现。
