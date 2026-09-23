#!/usr/bin/env bash
# 配置并构建原生 macOS 目标，统一管理架构、SDK、部署下限和预编译布局。
# 脚本当前只允许宿主原生架构，避免把单架构源码依赖误当作交叉产物。
# AppleClang 主版本用于派生 compiler tag，确保预编译依赖按 ABI 分层。
# 预编译消费模式先拉取精确 LFS 内容，源码模式构建 staging 所需依赖。
# --fresh 删除构建树前会验证解析后的绝对路径范围。
set -euo pipefail

# 输出参数契约和环境覆盖入口，不执行工具链探测。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/macos-build.sh [options]

Configure and build the native macOS target with AppleClang.

Options:
  --arch <arm64|x86_64>      Native target architecture. Default: host architecture
  --cc <path>                C compiler override. Default: xcrun clang
  --cxx <path>               C++ compiler override. Default: xcrun clang++
  --sdk <path|name>          macOS SDK path or CMake SDK name. Default: active macOS SDK
  --deployment-target <ver>  Minimum macOS deployment target. Default: 11.0
  --build-dir <path>         Build directory. Default: build_macos_<arch>, or
                             build_macos_sources_<arch> with --sources-build
  --build-type <type>        CMake build type. Default: RelWithDebInfo
  --toolchain <name>         Prebuilt toolchain directory. Default: clang
  --compiler-tag <tag>       Prebuilt compiler tag. Default: detected clang major
  --jobs <count>             Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>           PROJECT_LINKAGE value: static or shared. Default: static
  --vulkan-validation-layers Enable Vulkan validation layers. Default: disabled.
  --sources-build            Configure with SOURCES_BUILD=ON.
  --prebuilt-targets         Build only third-party targets used for staging.
  --configure-only           Configure and generate, then stop.
  --fresh                    Remove the build directory before configuring.
  -h, --help                 Show this help

Environment overrides:
  MACOS_PREBUILT_ARCH          Default target architecture
  MACOS_PREBUILT_TOOLCHAIN     Default prebuilt toolchain directory
  MACOS_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
  MACOS_SDK                    Default macOS SDK path or CMake SDK name
  MACOSX_DEPLOYMENT_TARGET     Default minimum macOS deployment target
  MACOS_CODESIGN_IDENTITY      CPack app signing identity. Default: - (ad-hoc)
  CMAKE_GENERATOR              Default: Ninja
EOF
}

# 使用约四分之三逻辑处理器作为默认构建并行度。
detectBuildJobs() {
    local maxThreads=1

    if command -v sysctl >/dev/null 2>&1; then
        # macOS 优先通过 sysctl 获取逻辑 CPU 数量。
        maxThreads="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
    elif command -v getconf >/dev/null 2>&1; then
        # POSIX getconf 作为兼容回退。
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        # 探测异常时保守使用单线程。
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    # 小型 Runner 的整数除法结果至少保留一个任务。
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

# 规范化 CMake 与预编译目录共同使用的架构名称。
normalizeArchitecture() {
    case "$1" in
        arm64 | aarch64)
            # Apple Silicon 别名统一为 arm64。
            printf "arm64\n"
            ;;
        x86_64 | amd64)
            # Intel 64 位别名统一为 x86_64。
            printf "x86_64\n"
            ;;
        *)
            # 未知架构无法匹配预编译布局，立即失败。
            printf "error: unsupported macOS architecture: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

# 确认所需宿主命令或绝对可执行文件可调用。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 构建脚本不尝试安装 Xcode 或其他系统工具。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 将相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许构建树位于工作区外。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用脚本时的当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 从实际 C++ 编译器预定义宏生成 clang<major> 标签。
detectClangCompilerTag() {
    local macroOutput
    # 宏探测比解析带 Apple 版本号的说明文本更稳定。
    if ! macroOutput="$(printf "\n" | "${cxxCompiler}" -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: failed to query Clang predefined macros: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    local clangMajor
    # 只接受纯数字主版本，避免生成异常路径层级。
    clangMajor="$(awk '$2 == "__clang_major__" { print $3; exit }' <<<"${macroOutput}")"
    if [[ ! "${clangMajor}" =~ ^[0-9]+$ ]]; then
        printf "error: failed to detect Clang major version from: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    printf "clang%s\n" "${clangMajor}"
}

# 脚本路径用于定位仓库及同目录 LFS helper。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    # SDK、xcrun 与原生产物要求 Darwin 宿主。
    printf "error: scripts/ci/macos-build.sh must run on macOS\n" >&2
    exit 1
fi

requireCommand cmake
requireCommand xcrun

# 架构默认跟随环境覆盖或当前宿主。
targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
# 编译器空值在参数解析后通过 xcrun 补齐。
ccCompiler=""
cxxCompiler=""
macosSdk="${MACOS_SDK:-}"
deploymentTarget="${MACOSX_DEPLOYMENT_TARGET:-}"
# 构建目录依赖架构和依赖来源，稍后派生。
buildDir=""
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
prebuiltToolchain="${MACOS_PREBUILT_TOOLCHAIN:-clang}"
compilerTag="${MACOS_PREBUILT_COMPILER_TAG:-}"
projectLinkage="static"
sourcesBuild="OFF"
# 显式关闭默认值，避免复用构建目录时沿用旧缓存。
vulkanValidationLayers="OFF"
# 三个流程开关分别控制目标集合、停止点和目录生命周期。
prebuiltTargets=0
configureOnly=0
freshBuild=0

# 在探测 SDK 或修改构建树前完整解析选项。
while (( $# > 0 )); do
    case "$1" in
        --arch)
            # 架构值稍后与宿主规范名称进行比较。
            if (( $# < 2 )); then
                printf "error: --arch requires a value\n" >&2
                exit 1
            fi
            targetArch="$2"
            shift 2
            ;;
        --cc)
            # 显式 C 编译器不会自动改变 compiler tag。
            if (( $# < 2 )); then
                printf "error: --cc requires a value\n" >&2
                exit 1
            fi
            ccCompiler="$2"
            shift 2
            ;;
        --cxx)
            # C++ 编译器同时用于 Clang 主版本探测。
            if (( $# < 2 )); then
                printf "error: --cxx requires a value\n" >&2
                exit 1
            fi
            cxxCompiler="$2"
            shift 2
            ;;
        --sdk)
            # 值可以是现有路径或 xcrun 可解析的 SDK 名称。
            if (( $# < 2 )); then
                printf "error: --sdk requires a value\n" >&2
                exit 1
            fi
            macosSdk="$2"
            shift 2
            ;;
        --deployment-target)
            # 部署下限进入环境和 CMake cache。
            if (( $# < 2 )); then
                printf "error: --deployment-target requires a value\n" >&2
                exit 1
            fi
            deploymentTarget="$2"
            shift 2
            ;;
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
            # 配置名同时用于 CMake 和预编译依赖选择。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --toolchain)
            # 目录标签必须描述所消费预编译 ABI 家族。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            # 显式标签覆盖 AppleClang 自动探测值。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --jobs)
            # 显式并行度覆盖宿主 CPU 自动探测。
            if (( $# < 2 )); then
                printf "error: --jobs requires a value\n" >&2
                exit 1
            fi
            buildJobs="$2"
            shift 2
            ;;
        --linkage)
            # PROJECT_LINKAGE 与 ICE_LINKAGE 始终同步。
            if (( $# < 2 )); then
                printf "error: --linkage requires a value\n" >&2
                exit 1
            fi
            projectLinkage="$2"
            shift 2
            ;;
        --sources-build)
            # 源码模式不从预编译目录消费依赖。
            sourcesBuild="ON"
            shift
            ;;
        --vulkan-validation-layers)
            # 验证层选项与依赖来源和构建类型互不绑定。
            vulkanValidationLayers="ON"
            shift
            ;;
        --prebuilt-targets)
            # 仅构建后续 staging 清单需要的 target。
            prebuiltTargets=1
            shift
            ;;
        --configure-only)
            # 配置成功后不进入实际编译。
            configureOnly=1
            shift
            ;;
        --fresh)
            # 删除动作在绝对路径安全验证后执行。
            freshBuild=1
            shift
            ;;
        -h | --help)
            # 帮助成功退出且不解析 SDK。
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

# 架构比较采用规范值，避免别名造成假不匹配。
targetArch="$(normalizeArchitecture "${targetArch}")"
hostArch="$(normalizeArchitecture "$(uname -m)")"
if [[ "${targetArch}" != "${hostArch}" ]]; then
    # 当前源码依赖流程不生成跨架构或 Universal Binary。
    printf "error: macOS prebuilt source builds currently require the native host architecture: requested %s, host %s\n" \
        "${targetArch}" "${hostArch}" >&2
    exit 1
fi
if [[ -z "${deploymentTarget}" ]]; then
    # 缺省下限与打包脚本保持一致。
    deploymentTarget="11.0"
fi

if [[ -z "${ccCompiler}" ]]; then
    # xcrun 尊重当前 DEVELOPER_DIR 和 Xcode 选择。
    ccCompiler="$(xcrun --find clang)"
fi
if [[ -z "${cxxCompiler}" ]]; then
    # C 与 C++ 编译器来自同一活动 toolchain。
    cxxCompiler="$(xcrun --find clang++)"
fi
if [[ -z "${macosSdk}" ]]; then
    # 未指定时使用活动 Xcode 的默认 macOS SDK 路径。
    macosSdk="$(xcrun --sdk macosx --show-sdk-path)"
elif [[ ! -d "${macosSdk}" ]]; then
    # 非目录值按 SDK 名称交给 xcrun 解析。
    requestedMacosSdk="${macosSdk}"
    if ! macosSdk="$(xcrun --sdk "${requestedMacosSdk}" --show-sdk-path 2>/dev/null)"; then
        # 不对未知 SDK 名称回退，避免静默改变目标环境。
        printf "error: failed to resolve macOS SDK: %s\n" "${requestedMacosSdk}" >&2
        exit 1
    fi
fi

requireCommand "${ccCompiler}"
requireCommand "${cxxCompiler}"

if [[ -z "${compilerTag}" ]]; then
    # 标签只在调用方没有固定值时自动探测。
    compilerTag="$(detectClangCompilerTag)"
fi

if [[ -z "${buildDir}" ]]; then
    # 源码与预编译消费模式使用不同默认构建树。
    if [[ "${sourcesBuild}" = "ON" ]]; then
        # staging 脚本默认消费 sources 前缀目录。
        buildDir="build_macos_sources_${targetArch}"
    else
        # 常规应用构建使用简短平台和架构名称。
        buildDir="build_macos_${targetArch}"
    fi
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # CMake 并行度仅接受正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    # 链接方式参与预编译目录和运行库选择。
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    # 空路径层会破坏 ABI 产物隔离。
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
    exit 1
fi

if [[ "${sourcesBuild}" == "OFF" ]]; then
    # 消费模式按完整 ABI 坐标拉取 LFS 对象。
    bash "${scriptDir}/pull-lfs-for-build.sh" \
        --platform macos \
        --arch "${targetArch}" \
        --toolchain "${prebuiltToolchain}" \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}" \
        --include-tests
fi

# 第三方源码 target 关闭 Clang LTO，避免工具链插件兼容问题。
disableClangLto="OFF"
if [[ "${sourcesBuild}" = "ON" ]]; then
    disableClangLto="ON"
fi

buildDir="$(projectPath "${buildDir}")"

if (( freshBuild )); then
    # 拒绝空路径、文件系统根和项目根三个高风险删除目标。
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

# 环境变量确保 CMake 与第三方子构建使用同一编译器和 SDK。
export CC="${ccCompiler}"
export CXX="${cxxCompiler}"
export SDKROOT="${macosSdk}"
export MACOSX_DEPLOYMENT_TARGET="${deploymentTarget}"

# 使用数组保存 CMake 参数，完整支持路径中的空格。
cmakeArgs=(
    -G "${CMAKE_GENERATOR:-Ninja}"
    -DCMAKE_BUILD_TYPE="${buildType}"
    -DBUILD_TESTING=ON
    # 配置数组保留完整参数边界，验证层状态每次都显式写入缓存。
    -DMMM_ENABLE_VULKAN_VALIDATION_LAYERS="${vulkanValidationLayers}"
    # CI 与打包构建不得写入 Runner 的用户配置目录。
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF
    -DCMAKE_OSX_ARCHITECTURES="${targetArch}"
    # SDK 和架构显式写入 cache，避免复用目录时跟随宿主漂移。
    -DCMAKE_OSX_SYSROOT="${macosSdk}"
    -DSOURCES_BUILD="${sourcesBuild}"
    -DPROJECT_LINKAGE="${projectLinkage}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DPROJECT_PREBUILT_PLATFORM=macos
    # 主项目与 ICE 使用完全一致的预编译 ABI 坐标。
    -DPROJECT_PREBUILT_ARCH="${targetArch}"
    -DPROJECT_PREBUILT_TOOLCHAIN="${prebuiltToolchain}"
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}"
    -DICE_PREBUILT_PLATFORM=macos
    -DICE_PREBUILT_ARCH="${targetArch}"
    -DICE_PREBUILT_TOOLCHAIN="${prebuiltToolchain}"
    -DICE_PREBUILT_COMPILER_TAG="${compilerTag}"
    -DICE_LINKAGE="${projectLinkage}"
    # PGO 在 macOS 发布构建中保持关闭。
    -DMMM_DISABLE_CLANG_LTO="${disableClangLto}"
    -DMMM_PGO_INSTRUMENT=OFF
    -DMMM_PGO_USE=OFF
    -DMMM_MACOS_CODESIGN_IDENTITY="${MACOS_CODESIGN_IDENTITY:--}"
    # 部署下限同时影响系统 API 可用性和链接器 load command。
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${deploymentTarget}"
)
cmakeArgs+=(-S "${projectRoot}" -B "${buildDir}")

# 一次性展开数组，避免字符串 eval 或二次分词。
cmake "${cmakeArgs[@]}"

if (( configureOnly )); then
    # 配置探针不要求生成任何目标文件。
    exit 0
fi

if (( prebuiltTargets )); then
    # 清单与 staging 脚本保持同步，只构建依赖归档。
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

# 维护约束：本脚本只运行原生架构，不承担 macOS 交叉编译。
# 维护约束：Universal Binary 应由独立合并和验证流程生成。
# 维护约束：arm64 与 x86_64 构建必须使用不同构建目录。
# 维护约束：源码依赖与预编译消费不得复用同一 CMake cache。
# 维护约束：SDK 路径应来自当前 Xcode 或调用方明确指定值。
# 维护约束：SDK 名称解析失败时禁止回退到活动默认 SDK。
# 维护约束：部署下限必须与所有静态依赖的最低版本兼容。
# 维护约束：构建和后续 CPack 必须使用相同部署下限。
# 维护约束：CC 与 CXX 应来自同一个 AppleClang toolchain。
# 维护约束：compiler tag 必须对应 C++ 编译器的实际 Clang 主版本。
# 维护约束：显式 compiler tag 的准确性由 CI 调用方负责。
# 维护约束：toolchain 与 compiler tag 必须匹配预编译目录内容。
# 维护约束：SOURCES_BUILD 是依赖来源唯一开关，不允许自动回退。
# 维护约束：预编译模式必须在 CMake 配置前完成 LFS 拉取。
# 维护约束：include-tests 确保测试目标依赖的归档同步可用。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 不得产生不同运行库选择。
# 维护约束：shared 模式必须由对应 shared 预编译布局支持。
# 维护约束：源码依赖构建不启用主项目 PGO 或第三方 PGO。
# 维护约束：配置同步开关保持关闭以保护 Runner 用户目录。
# 维护约束：代码签名身份只写入 CMake 配置，不在此脚本直接签名。
# 维护约束：正式签名、打包和公证由后续 macos-package 流程执行。
# 维护约束：所有删除目标必须先转换为项目根相关的绝对路径。
# 维护约束：删除防护不得放宽到项目根或文件系统根。
# 维护约束：预编译 target 清单变更时需同步 staging 脚本。
# 维护约束：第三方 target 名必须与 CMake 导出的真实名称一致。
# 维护约束：configure-only 不应要求任何静态库已经生成。
# 维护约束：构建失败直接传播 CMake 或底层生成器退出状态。
# 维护约束：脚本不执行测试、staging、strip 或产物上传。
# 维护约束：RelWithDebInfo 产物必须保留后续调试所需信息。
# 维护约束：成功退出只表示请求目标完成，不表示已生成发布包。
# 维护约束：AppleClang 版本探测必须使用最终选定的 C++ 编译器。
# 维护约束：预定义宏读取失败时不得从路径名猜测 compiler tag。
# 维护约束：架构别名只能规范化为 arm64 或 x86_64。
# 维护约束：宿主架构检查必须发生在构建目录派生之前。
# 维护约束：非原生架构请求失败时不得创建或清理任何构建目录。
# 维护约束：活动 SDK 选择应尊重调用方预设的 DEVELOPER_DIR。
# 维护约束：xcrun 返回的编译器路径必须在使用前验证可执行。
# 维护约束：xcrun 返回的 SDK 路径必须作为 SDKROOT 传给子构建。
# 维护约束：CMAKE_OSX_SYSROOT 与 SDKROOT 应保持同一个实际 SDK。
# 维护约束：CMAKE_OSX_ARCHITECTURES 必须与 staging 的 arch 层一致。
# 维护约束：PROJECT_PREBUILT_ARCH 与 ICE_PREBUILT_ARCH 必须相同。
# 维护约束：主仓和 ICE 的 platform、toolchain、tag 必须同步。
# 维护约束：不同 SDK 或部署下限不应复用未经重新配置的 cache。
# 维护约束：默认构建目录名称必须继续编码目标架构。
# 维护约束：源码构建目录名称必须保留 sources 标识供 staging 查找。
# 维护约束：预编译 consumer 不应意外配置第三方源码 fallback。
# 维护约束：源码 producer 不应从旧预编译目录补齐缺失 target。
# 维护约束：CMAKE_POSITION_INDEPENDENT_CODE 对静态依赖保持开启。
# 维护约束：共享链接模式仍需验证应用包内运行时依赖路径。
# 维护约束：静态链接模式仍可能依赖 macOS 系统 frameworks。
# 维护约束：签名身份中的空格必须通过数组参数保持为单一值。
# 维护约束：空签名身份的禁用语义由打包流程显式选择。
# 维护约束：默认连字符身份表示 ad-hoc，不等同于关闭签名。
# 维护约束：构建脚本不解锁钥匙串或导入签名证书。
# 维护约束：构建脚本不执行 codesign 验证或公证提交。
# 维护约束：构建配置变化时 CPackConfig 必须由本次 CMake 重新生成。
# 维护约束：预编译 target 清单不包含主应用和更新器目标。
# 维护约束：staging 只消费来源一致的单架构静态归档。
# 维护约束：测试执行必须使用隔离的 MMM_CONFIG_ROOT。
# 维护约束：本脚本本身不创建或修改用户配置目录。
# 维护约束：生成器覆盖值必须与现有构建目录保持一致。
# 维护约束：非 Ninja 生成器也必须支持 cmake --build --parallel。
# 维护约束：路径中的空格不得通过字符串拼接或 eval 展开。
# 维护约束：环境变量只导出给当前脚本及其子进程。
# 维护约束：脚本结束后不修改调用者 shell 的 CC、CXX 或 SDKROOT。
# 维护约束：构建产物权限和 bundle 布局由 CMake target 决定。
# 维护约束：脚本不移动、复制或重命名构建结果。
# 维护约束：失败后的构建树保留用于诊断，除非调用方下次使用 fresh。
# 维护约束：CI cache 键应包含架构、SDK、部署下限和 compiler tag。
# 维护约束：升级最低系统版本时需同步打包脚本默认值。
# 维护约束：升级 Xcode 时需重新生成并验证对应预编译依赖。
# 维护约束：新增 CMake ABI 坐标必须同时传给主项目和 ICE。
# 维护约束：构建日志应保留实际编译器、SDK 和 CMake 配置诊断。
# 维护约束：后续打包必须使用同一 buildDir 和 buildType。
# 维护约束：发布前应在目标架构真实设备上验证启动。
# 维护约束：架构验证不能由文件名或目录名代替。
# 维护约束：任何 Universal Binary 合并都必须在本脚本之外完成。
# 维护约束：本脚本不拉取与目标 ABI 无关的 LFS 目录。
# 维护约束：脚本尾部不隐式清理，便于失败后检查产物。
# 维护约束：成功后调用方负责测试、staging 或打包的下一阶段。
# 维护约束：新增平台参数必须同步 showUsage 和 macos-package 调用方。
