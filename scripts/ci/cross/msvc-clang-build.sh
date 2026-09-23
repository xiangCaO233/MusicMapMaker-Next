#!/usr/bin/env bash
# 在 Linux 上使用 clang-cl 与 MSVC/Windows SDK 布局构建 Windows x86_64 目标。
# toolchain 文件把 clang-cl、lld-link 与 llvm-lib 映射到 MSVC 兼容 ABI。
# 预编译消费模式先拉取精确 LFS 内容，源码模式生成 staging 所需依赖。
# INCLUDE 与 LIB 显式传给 Meson 子构建，补足其不会继承 CMake 参数的问题。
# --fresh 只删除经过绝对路径解析和安全检查的构建目录。
set -euo pipefail

# 输出参数契约和交叉环境覆盖入口。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/msvc-clang-build.sh [options]

Configure and build the Windows MSVC-like clang-cl cross target.

Options:
  --build-dir <path>     Build directory. Default: build_cross_msvc
  --build-type <type>    CMake build type. Default: RelWithDebInfo
  --compiler-tag <tag>   Prebuilt compiler tag. Default: 2026
  --llvm-version <major> LLVM tool suite major version. Default: 22
  --jobs <count>         Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>       PROJECT_LINKAGE value: static or shared. Default: static
  --toolchain <path>     CMake toolchain file. Default: cmake/toolchain/cross-msvc.cmake
  --sources-build        Configure with SOURCES_BUILD=ON.
  --vulkan-validation-layers Enable Vulkan validation layers. Default: disabled.
  --prebuilt-targets     Build only third-party targets used for staging.
  --configure-only       Configure and generate, then stop
  --fresh                Remove the build directory before configuring
  -h, --help             Show this help

Environment overrides:
  MSVC_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
  MSVC_LLVM_VERSION      Default LLVM tool suite major version
  WINDOWS_CROSS_ROOT     Default: /mnt/cross/windows
  VULKAN_SDK             Default: ${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0
  CMAKE_GENERATOR        Default: Ninja
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

# 验证指定宿主工具可从 PATH 调用。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 脚本不自动安装 Clang 或 Windows SDK。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 从已选 clang-cl 所在目录解析配套 LLVM 工具，避免混用不同主版本。
# 输入只接受工具的标准无版本名称，版本入口由上层统一定位。
# 输出保留同目录路径，不再经过 PATH 搜索或系统 alternatives。
# 这样既兼容 Debian 的版本化命令，也兼容 Gentoo 目录内的无版本工具。
resolveLlvmTool() {
    local toolName="$1"
    local toolPath="${llvmBinDir}/${toolName}"

    if [[ ! -x "${toolPath}" ]]; then
        # LLVM 安装必须在同一 bin 目录提供完整的交叉构建工具集。
        printf "error: LLVM %s tool not found or not executable: %s\n" "${llvmVersion}" "${toolPath}" >&2
        exit 1
    fi

    printf "%s\n" "${toolPath}"
}

# 将相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许构建树位于外部磁盘。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用者当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 通过脚本自身位置定位项目根和同目录 helper。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_msvc"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
# compiler tag 对应预编译目录中的 MSVC 工具集版本。
compilerTag="${MSVC_PREBUILT_COMPILER_TAG:-2026}"
# LLVM 主版本只选择宿主工具，不改变预编译目录的 MSVC ABI 标签。
llvmVersion="${MSVC_LLVM_VERSION:-22}"
projectLinkage="static"
toolchainFile="cmake/toolchain/cross-msvc.cmake"
sourcesBuild="OFF"
# 复用构建目录时仍显式采用本次请求的验证层状态。
vulkanValidationLayers="OFF"
# 流程开关分别控制目标集合、停止点和构建树生命周期。
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
            # 配置名同时用于 CMake 与预编译目录选择。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            # 标签必须与 MSVC 头库版本及依赖归档一致。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --llvm-version)
            # 同一主版本同时选择 clang-cl、clang、lld-link 与 LLVM binutils。
            if (( $# < 2 )); then
                printf "error: --llvm-version requires a value\n" >&2
                exit 1
            fi
            llvmVersion="$2"
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
            # 同一选项适用于 Debug 和发布配置。
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
            # 删除动作受后续绝对路径安全检查保护。
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

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # CMake 并行度仅接受正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    # 链接偏好参与预编译布局与 MSVC 运行库选择。
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${compilerTag}" ]]; then
    # 空标签会破坏不同 MSVC 工具集产物隔离。
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

if [[ ! "${llvmVersion}" =~ ^[1-9][0-9]*$ ]]; then
    # 主版本参与版本化 clang-cl 入口选择，只接受正整数。
    printf "error: --llvm-version must be a positive integer\n" >&2
    exit 1
fi

requireCommand cmake
requireCommand readlink
requireCommand "clang-cl-${llvmVersion}"

# 版本化 clang-cl 可能是系统链接；解析真实目录后只使用同目录工具。
# 编译器保留版本化入口，避免默认 LLVM 22 使现有 CI cache 发生无意义切换。
clangClEntry="$(command -v "clang-cl-${llvmVersion}")"
llvmBinDir="$(dirname "$(readlink -f "${clangClEntry}")")"
clangCl="${clangClEntry}"
clangC="$(resolveLlvmTool clang)"
lldLink="$(resolveLlvmTool lld-link)"
llvmLib="$(resolveLlvmTool llvm-lib)"
llvmRc="$(resolveLlvmTool llvm-rc)"
llvmMt="$(resolveLlvmTool llvm-mt)"
llvmRanlib="$(resolveLlvmTool llvm-ranlib)"
llvmStrip="$(resolveLlvmTool llvm-strip)"
llvmNm="$(resolveLlvmTool llvm-nm)"
llvmObjcopy="$(resolveLlvmTool llvm-objcopy)"

# Make、Meson 与 ICE 外部项目通过环境变量复用 CMake 选择的同一套工具。
# MMM_* 服务主项目 LuaJIT 包装器，ICE_* 服务引擎内的外部依赖包装器。
# 路径写入当前构建进程环境，源码依赖模式不会回退到 wrapper 的 LLVM 22 默认值。
export MMM_CLANG_CL="${clangCl}"
export MMM_CLANG_C="${clangC}"
export MMM_LLD_LINK="${lldLink}"
export MMM_LLVM_LIB="${llvmLib}"
export ICE_CLANG_CL="${clangCl}"
export ICE_CLANG_C="${clangC}"
export ICE_LLD_LINK="${lldLink}"
export ICE_LLVM_LIB="${llvmLib}"

printf "Using LLVM %s tools from %s\n" "${llvmVersion}" "${llvmBinDir}"

if [[ "${sourcesBuild}" == "OFF" ]]; then
    # 消费模式在配置前拉取 windows/x86_64/msvc 精确对象。
    bash "${scriptDir}/../pull-lfs-for-build.sh" \
        --platform windows \
        --arch x86_64 \
        --toolchain msvc \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}"
fi

buildDir="$(projectPath "${buildDir}")"
# toolchain 路径同样支持项目相对值。
toolchainFile="$(projectPath "${toolchainFile}")"

if [[ ! -f "${toolchainFile}" ]]; then
    # 缺失 toolchain 时禁止 CMake 回退宿主 ABI。
    printf "error: toolchain file not found: %s\n" "${toolchainFile}" >&2
    exit 1
fi

export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
# Vulkan、MSVC 与 WinSDK 默认路径均从同一资源根派生。
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"
export MSVC_BASE="${MSVC_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231}"
export WINSDK_BASE="${WINSDK_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10}"
export WINSDK_VER="${WINSDK_VER:-10.0.26100.0}"

# Meson 的 clang-cl 依赖扫描不会继承 CMake 的 -imsvc 与 /libpath 参数，需提供标准 MSVC 环境变量。
# INCLUDE 顺序先 MSVC 标准库，再 UCRT 与 Windows SDK 头。
export INCLUDE="${MSVC_BASE}/include;${MSVC_BASE}/atlmfc/include;${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt;${WINSDK_BASE}/Include/${WINSDK_VER}/shared;${WINSDK_BASE}/Include/${WINSDK_VER}/um;${WINSDK_BASE}/Include/${WINSDK_VER}/winrt"
# LIB 顺序覆盖 MSVC、ATL/MFC、UCRT、系统库与项目代理库。
export LIB="${MSVC_BASE}/lib/x64;${MSVC_BASE}/atlmfc/lib/x64;${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64;${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64;${projectRoot}/lib_proxy"

"${scriptDir}/list-msvc-toolchain-layout.sh" --max-entries "${MMM_MSVC_LAYOUT_MAX_ENTRIES:-120}"

if [[ ! -d "${VULKAN_SDK}" ]]; then
    # Windows 目标必须使用现有 Windows Vulkan SDK。
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
# CMake 参数固定目标平台、依赖来源和两级 ABI 标签。
# 验证层作用于目标程序运行时，由此次配置而非 Debug 类型决定。
cmake -G "${CMAKE_GENERATOR:-Ninja}" \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DMMM_ENABLE_VULKAN_VALIDATION_LAYERS="${vulkanValidationLayers}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchainFile}" \
    -DMMM_CLANG_CL:FILEPATH="${clangCl}" \
    -DMMM_LLD_LINK:FILEPATH="${lldLink}" \
    -DMMM_LLVM_LIB:FILEPATH="${llvmLib}" \
    -DMMM_LLVM_RC:FILEPATH="${llvmRc}" \
    -DMMM_LLVM_MT:FILEPATH="${llvmMt}" \
    -DMMM_LLVM_RANLIB:FILEPATH="${llvmRanlib}" \
    -DMMM_LLVM_STRIP:FILEPATH="${llvmStrip}" \
    -DMMM_LLVM_NM:FILEPATH="${llvmNm}" \
    -DMMM_LLVM_OBJCOPY:FILEPATH="${llvmObjcopy}" \
    -DSOURCES_BUILD="${sourcesBuild}" \
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DICE_LINKAGE="${projectLinkage}" \
    -DPROJECT_PREBUILT_PLATFORM=windows \
    -DPROJECT_PREBUILT_TOOLCHAIN=msvc \
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DICE_PREBUILT_PLATFORM=windows \
    -DICE_PREBUILT_TOOLCHAIN=msvc \
    -DICE_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DMMM_DISABLE_CLANG_LTO=ON \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if [[ "${sourcesBuild}" == "ON" ]]; then
    # ICE 当前把 Windows 空设备 NUL 作为 Meson native file；Linux 交叉构建需在工作目录提供同名空文件。
    # 只在源码依赖模式创建该兼容文件。
    mkdir -p "${buildDir}/rb_bld"
    : >"${buildDir}/rb_bld/NUL"
fi

if (( configureOnly )); then
    # 配置探针成功后不执行实际构建。
    exit 0
fi

if (( prebuiltTargets )); then
    # 清单只包含后续 staging 所需第三方归档。
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
# 维护约束：clang-cl、clang、lld-link 与 LLVM binutils 必须来自同一目录。
# 维护约束：LLVM 主版本默认保持 CI 的 22，调用方可显式选择其他已安装版本。
# 维护约束：LLVM 主版本只接受无前导零的正整数。
# 维护约束：版本化 clang-cl 是定位工具集目录的唯一 PATH 入口。
# 维护约束：工具集目录缺少任一必需工具时不得回退到其他 PATH 目录。
# 维护约束：CMake cache 必须记录本次选择的每一个 LLVM 工具绝对路径。
# 维护约束：外部 Make 与 ICE 子构建必须继承同一 clang 和归档器路径。
# 维护约束：切换 LLVM 主版本时应使用独立构建目录或显式 fresh。
# 维护约束：LLVM 主版本选择不得隐式改写 MSVC 预编译包的 compiler tag。
# 维护约束：资源、清单与二进制检查工具必须跟随编译器整套切换。
# 维护约束：MSVC_BASE 必须同时提供 include 与 x64 lib 布局。
# 维护约束：WINSDK_BASE 与 WINSDK_VER 必须指向同一 SDK 安装。
# 维护约束：INCLUDE 路径顺序不得让宿主 Linux 头优先。
# 维护约束：LIB 路径顺序不得引入另一版本 MSVC 或 WinSDK 库。
# 维护约束：lib_proxy 只承载明确需要的兼容导入库代理。
# 维护约束：布局诊断发生在配置前，输出上限由环境变量控制。
# 维护约束：VULKAN_SDK 必须指向 Windows 目标 SDK。
# 维护约束：WINDOWS_CROSS_ROOT 只提供默认值，不覆盖显式路径。
# 维护约束：compiler tag 必须对应实际 MSVC 兼容工具集版本。
# 维护约束：不同 compiler tag 构建不得共用 CMake cache。
# 维护约束：toolchain 文件必须在配置前验证存在。
# 维护约束：源码依赖与预编译消费构建应使用独立目录。
# 维护约束：SOURCES_BUILD 是依赖来源唯一开关，不允许自动回退。
# 维护约束：预编译模式必须在 CMake 配置前完成 LFS 拉取。
# 维护约束：LFS 拉取坐标必须使用 windows/x86_64/msvc。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 必须保持一致。
# 维护约束：static 模式需要匹配静态 MSVC 运行库配置。
# 维护约束：shared 模式需要匹配动态 MSVC 运行库配置。
# 维护约束：交叉构建禁止同步翻译或默认皮肤到宿主配置。
# 维护约束：Clang LTO 在此工具链流程中保持显式关闭。
# 维护约束：PGO 插桩和使用在交叉流程中保持关闭。
# 维护约束：Meson NUL 兼容文件不得创建到源码目录。
# 维护约束：NUL 文件只在 SOURCES_BUILD=ON 的构建树内创建。
# 维护约束：所有续行参数必须保持双引号和反斜杠结构。
# 维护约束：续行反斜杠之后不得插入注释。
# 维护约束：所有删除目标必须先转换为绝对路径。
# 维护约束：删除防护不得放宽到项目根或文件系统根。
# 维护约束：fresh 不应删除 Windows SDK、Vulkan SDK 或 MSVC 工具集。
# 维护约束：configure-only 不应要求任何目标归档已经生成。
# 维护约束：prebuilt-targets 清单需与 MSVC staging 清单同步。
# 维护约束：第三方 target 名必须与 CMake 实际导出一致。
# 维护约束：常规模式不额外执行测试、打包或 staging。
# 维护约束：构建失败直接传播 CMake 或生成器退出状态。
# 维护约束：脚本不执行 PDB 复制或符号策略选择。
# 维护约束：脚本不负责 Git LFS add、commit 或 push。
# 维护约束：LLVM 默认版本升级时需同步 CI 镜像和帮助文本。
# 维护约束：MSVC 或 WinSDK 升级时需同步默认目录版本。
# 维护约束：Vulkan SDK 升级时需同步交叉环境默认值。
# 维护约束：新增参数必须同步 showUsage 文本。
# 维护约束：成功退出只表示请求目标构建完成，不表示已 staging。
# 维护约束：正式发布前必须消费 staging 后的依赖集合再构建。
# 维护约束：Windows 可执行文件运行验证由目标环境后续完成。
# 维护约束：MSVC 头、库和工具版本必须作为单一工具链快照维护。
# 维护约束：ATLMFC 路径保留是为了兼容依赖的条件性探测。
# 维护约束：UCRT、shared、um 与 winrt 头目录顺序需保持稳定。
# 维护约束：UCRT 与 um 库目录必须使用相同 WINSDK_VER。
# 维护约束：路径中的 Program Files 空格必须始终由双引号保护。
# 维护约束：INCLUDE 与 LIB 使用分号分隔以符合 MSVC 工具约定。
# 维护约束：工具链布局诊断不应递归输出无限条目。
# 维护约束：llvm-rc 与 llvm-mt 是资源和 manifest 生成的强制依赖。
# 维护约束：lld-link wrapper 不得意外解析到宿主 ELF 链接器。
# 维护约束：llvm-lib wrapper 必须产生 MSVC 可消费的 COFF 库。
# 维护约束：构建缓存键应包含 LLVM、MSVC、WinSDK 和 compiler tag。
# 维护约束：成功后调用方负责 staging、符号策略和消费验证。
