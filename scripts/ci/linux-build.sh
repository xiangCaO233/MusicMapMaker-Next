#!/usr/bin/env bash
# 配置并构建原生 Linux 目标，统一管理编译器、预编译依赖和 PGO 开关。
# gcc14 与 clang19 预设同时决定默认可执行文件、工具链目录和 compiler tag。
# 预编译模式先按目标 ABI 拉取 LFS 内容，源码模式则构建可供 staging 的依赖。
# 同一构建目录通过文件锁串行化，避免取消中的 Ninja 与新任务并发写日志。
# --fresh 的删除范围在绝对路径解析后进行安全校验。
set -euo pipefail

# 输出命令行参数和环境覆盖入口，不触发配置。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/linux-build.sh [options]

Configure and build the native Linux target with a selected Debian compiler.

Options:
  --compiler <gcc14|clang19>  Compiler preset. Default: gcc14
  --cc <path>                 C compiler override.
  --cxx <path>                C++ compiler override.
  --build-dir <path>          Build directory. Default: build_linux_<compiler>
  --build-type <type>         CMake build type. Default: RelWithDebInfo
  --toolchain <name>          Prebuilt toolchain directory. Default: preset value
  --compiler-tag <tag>        Prebuilt compiler tag. Default: preset value
  --jobs <count>              Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>            PROJECT_LINKAGE value: static or shared. Default: static
  --vulkan-validation-layers  Enable Vulkan validation layers. Default: disabled.
  --pgo-instrument            Force MMM_PGO_INSTRUMENT=ON.
  --no-pgo-instrument         Force MMM_PGO_INSTRUMENT=OFF.
  --sources-build             Configure with SOURCES_BUILD=ON.
  --prebuilt-targets          Build only third-party targets used for staging.
  --configure-only            Configure and generate, then stop.
  --fresh                     Remove the build directory before configuring.
  -h, --help                  Show this help

Environment overrides:
  LINUX_PREBUILT_COMPILER       Default compiler preset
  LINUX_PREBUILT_TOOLCHAIN      Default prebuilt toolchain directory
  LINUX_PREBUILT_COMPILER_TAG   Default prebuilt compiler tag
  CMAKE_GENERATOR               Default: Ninja
EOF
}

# 取在线处理器数量的约四分之三作为默认并行度。
detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        # GNU/Linux 优先使用 nproc 感知容器 CPU 限制。
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        # POSIX getconf 是缺少 coreutils 时的兼容路径。
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        # 探测异常时保守回退单线程。
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    # 小型 Runner 的整数除法结果至少保持一个任务。
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

# 验证宿主命令存在，缺失时输出统一环境错误。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 构建脚本不尝试安装系统包。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 验证被中断 Ninja 构建的依赖日志可以稳定恢复。
verifyNinjaState() {
    local ninjaOutput

    # 自托管 Runner 被取消时，Ninja 可能只写入半条依赖记录。首次加载会自动截断至
    # 最后一条完整记录；再次加载必须静默，确保恢复已经落盘且不会污染后续 CI 日志。
    if ! ninjaOutput="$(ninja -C "$1" -n 2>&1 >/dev/null)"; then
        # dry-run 失败代表生成文件或依赖数据库不可消费。
        printf "%s\n" "${ninjaOutput}" >&2
        return 1
    fi

    if [[ "${ninjaOutput}" == *"premature end of file; recovering"* ]]; then
        printf "warning: recovered interrupted Ninja dependency log in %s\n" "$1" >&2
        if ! ninjaOutput="$(ninja -C "$1" -n 2>&1 >/dev/null)"; then
            # 第二次读取失败时保留 Ninja 原始诊断。
            printf "%s\n" "${ninjaOutput}" >&2
            return 1
        fi
    fi

    if [[ -n "${ninjaOutput}" ]]; then
        # 非致命诊断也写入 stderr 供 CI 保存。
        printf "%s\n" "${ninjaOutput}" >&2
    fi

    if [[ "${ninjaOutput}" == *"premature end of file; recovering"* ]]; then
        # 重复恢复意味着依赖日志未能稳定落盘。
        printf "error: Ninja dependency log remains truncated after recovery\n" >&2
        return 1
    fi
}

# 将用户提供的相对路径解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径允许构建树位于独立高速磁盘。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用脚本时的工作目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 应用编译器预设，并尊重命令行对单项值的显式覆盖。
applyCompilerPreset() {
    case "${compilerPreset}" in
        gcc14)
            # GCC 预设与 gcc/gcc14 预编译布局配对。
            ccCompiler="${ccCompiler:-gcc-14}"
            cxxCompiler="${cxxCompiler:-g++-14}"
            prebuiltToolchain="${prebuiltToolchain:-gcc}"
            compilerTag="${compilerTag:-gcc14}"
            ;;
        clang19)
            # Clang 预设与 clang/clang19 预编译布局配对。
            ccCompiler="${ccCompiler:-clang-19}"
            cxxCompiler="${cxxCompiler:-clang++-19}"
            prebuiltToolchain="${prebuiltToolchain:-clang}"
            compilerTag="${compilerTag:-clang19}"
            ;;
        *)
            # 未知预设不能可靠推导 ABI 目录。
            printf "error: unsupported --compiler preset: %s\n" "${compilerPreset}" >&2
            exit 1
            ;;
    esac
}

# 通过脚本位置定位仓库，支持任意工作目录调用。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

compilerPreset="${LINUX_PREBUILT_COMPILER:-gcc14}"
# 空编译器和布局字段在 applyCompilerPreset 中补齐。
ccCompiler=""
cxxCompiler=""
buildDir=""
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
prebuiltToolchain="${LINUX_PREBUILT_TOOLCHAIN:-}"
compilerTag="${LINUX_PREBUILT_COMPILER_TAG:-}"
projectLinkage="static"
sourcesBuild="OFF"
# 显式传递关闭状态，复用构建目录时不会保留上次的验证层设置。
vulkanValidationLayers="OFF"
# 三个流程开关分别控制目标集合、停止点和构建树生命周期。
prebuiltTargets=0
configureOnly=0
freshBuild=0
pgoInstrument="auto"

# 在产生任何构建副作用前完整解析参数。
while (( $# > 0 )); do
    case "$1" in
        --compiler)
            # 预设提供一致的编译器和 ABI 标签组合。
            if (( $# < 2 )); then
                printf "error: --compiler requires a value\n" >&2
                exit 1
            fi
            compilerPreset="$2"
            shift 2
            ;;
        --cc)
            # C 编译器覆盖不自动改写工具链标签。
            if (( $# < 2 )); then
                printf "error: --cc requires a value\n" >&2
                exit 1
            fi
            ccCompiler="$2"
            shift 2
            ;;
        --cxx)
            # C++ 编译器应与 C 编译器保持 ABI 兼容。
            if (( $# < 2 )); then
                printf "error: --cxx requires a value\n" >&2
                exit 1
            fi
            cxxCompiler="$2"
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
            # 配置名用于 CMake 和预编译目录选择。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --toolchain)
            # 预编译工具链目录可独立覆盖。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            # compiler tag 必须描述实际消费的依赖产物集。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --jobs)
            # 显式并行度覆盖自动探测值。
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
        --pgo-instrument)
            # 显式开启优先于后续 auto 规则。
            pgoInstrument="ON"
            shift
            ;;
        --no-pgo-instrument)
            # 调试或基线构建可强制关闭插桩。
            pgoInstrument="OFF"
            shift
            ;;
        --sources-build)
            # 源码模式不消费预编译 LFS 二进制。
            sourcesBuild="ON"
            shift
            ;;
        --vulkan-validation-layers)
            # 验证层可独立于 CMake 构建类型启用。
            vulkanValidationLayers="ON"
            shift
            ;;
        --prebuilt-targets)
            # 仅构建 staging 清单需要的第三方 target。
            prebuiltTargets=1
            shift
            ;;
        --configure-only)
            # 配置成功后不进入 Ninja 构建。
            configureOnly=1
            shift
            ;;
        --fresh)
            # 删除动作在路径安全检查后执行。
            freshBuild=1
            shift
            ;;
        -h | --help)
            # 帮助成功退出，不验证工具链。
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

# 参数覆盖完成后再补齐预设默认值。
applyCompilerPreset

# 源码依赖构建关闭 Clang LTO，避免第三方工具链插件不一致。
disableClangLto="OFF"
if [[ "${sourcesBuild}" = "ON" ]]; then
    disableClangLto="ON"
fi

if [[ -z "${buildDir}" ]]; then
    # 默认目录编码编译器预设，避免 GCC 与 Clang cache 混用。
    buildDir="build_linux_${compilerPreset,,}"
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    # CMake 并行度仅接受正整数。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    # 运行库与预编译路径依赖这一二值契约。
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    # 空目录层会破坏 ABI 产物隔离。
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
    exit 1
fi

if [[ "${sourcesBuild}" == "OFF" ]]; then
    # 消费模式只拉取当前平台、编译器、配置和链接方式所需对象。
    bash "${scriptDir}/pull-lfs-for-build.sh" \
        --platform linux \
        --arch "$(uname -m)" \
        --toolchain "${prebuiltToolchain}" \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}" \
        --include-tests
fi

if [[ "${pgoInstrument}" == "auto" ]]; then
    # 自动策略只给 Clang RelWithDebInfo 主业务模块启用插桩。
    pgoInstrument="OFF"
    if [[ "${buildType}" == "RelWithDebInfo" && "${prebuiltToolchain}" == "clang" ]]; then
        # 其他编译器或配置保持普通构建。
        pgoInstrument="ON"
    fi
fi

requireCommand cmake
# 编译器可能是绝对路径，command -v 同样可以验证可执行性。
requireCommand "${ccCompiler}"
requireCommand "${cxxCompiler}"

buildDir="$(projectPath "${buildDir}")"

# cancel-in-progress 可能在上一个 Ninja 进程退出前启动下一轮；按构建目录串行化，
# 避免两个进程并发写入同一个 .ninja_deps。锁文件放在构建目录外，防止 --fresh 删除已持有的锁。
requireCommand flock
requireCommand sha256sum
ninjaLockDir="${projectRoot}/.git/mmm-ci-ninja-locks"
# 锁位于 Git 元数据下，不会进入构建产物或被 --fresh 删除。
mkdir -p "${ninjaLockDir}"
# 哈希绝对构建目录得到稳定且无路径分隔符的锁名。
ninjaLockName="$(printf "%s" "${buildDir}" | sha256sum | awk '{ print $1 }')"
# 独立文件描述符的 flock 生命周期覆盖后续完整构建。
exec 8>"${ninjaLockDir}/${ninjaLockName}.lock"
flock 8

if (( freshBuild )); then
    # 拒绝空路径、文件系统根和项目根三个高风险目标。
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

# 显式编译器环境保证 CMake 首次配置选择请求工具链。
export CC="${ccCompiler}"
export CXX="${cxxCompiler}"

# 原生 Linux 构建必须使用系统 Vulkan，避免 Runner 注入的 Windows SDK 污染头文件搜索路径。
unset VULKAN_SDK VK_SDK_PATH

# CI 与预编译构建不得写入 Runner 的用户配置目录。
# 清除旧 Vulkan cache 项，避免构建目录复用遗留 Windows SDK 路径。
# 每次配置都覆盖缓存中的验证层状态，确保诊断开关按本次参数生效。
cmake -U "Vulkan_*" \
    -G "${CMAKE_GENERATOR:-Ninja}" \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DBUILD_TESTING=ON \
    -DMMM_ENABLE_VULKAN_VALIDATION_LAYERS="${vulkanValidationLayers}" \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DSOURCES_BUILD="${sourcesBuild}" \
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DPROJECT_PREBUILT_PLATFORM="linux" \
    -DPROJECT_PREBUILT_TOOLCHAIN="${prebuiltToolchain}" \
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DICE_PREBUILT_PLATFORM="linux" \
    -DICE_PREBUILT_TOOLCHAIN="${prebuiltToolchain}" \
    -DICE_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DICE_LINKAGE="${projectLinkage}" \
    -DMMM_DISABLE_CLANG_LTO="${disableClangLto}" \
    -DMMM_PGO_INSTRUMENT="${pgoInstrument}" \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if (( configureOnly )); then
    # 配置探针成功后立即退出，不要求生成任何目标文件。
    exit 0
fi

if [[ -f "${buildDir}/build.ninja" ]]; then
    # 仅 Ninja 生成器具备需要恢复的依赖日志。
    requireCommand ninja
    verifyNinjaState "${buildDir}"
fi

if (( prebuiltTargets )); then
    # 清单与 staging 脚本保持同步，避免构建无关业务目标。
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

# 维护约束：新增编译器预设必须同时定义 CC、CXX、toolchain 和 tag。
# 维护约束：显式 CC/CXX 覆盖不会自动猜测新的预编译 ABI 标签。
# 维护约束：工具链标签和 compiler tag 必须与依赖二进制实际来源一致。
# 维护约束：GCC 与 Clang 构建树不得共用 CMake cache。
# 维护约束：源码构建与预编译消费构建应使用独立目录。
# 维护约束：SOURCES_BUILD 是依赖来源唯一开关，不允许自动回退。
# 维护约束：PROJECT_LINKAGE 与 ICE_LINKAGE 必须保持相同值。
# 维护约束：shared 模式不得继承静态运行库链接参数。
# 维护约束：预编译模式必须在配置前完成精确 LFS 拉取。
# 维护约束：include-tests 确保测试链接需要的归档同时可用。
# 维护约束：PGO 插桩不得作用于第三方源码依赖构建。
# 维护约束：PGO 使用阶段由独立流程提供 profile，不在此处启用。
# 维护约束：Clang 源码依赖构建继续关闭跨 target LTO。
# 维护约束：系统 Vulkan 头和库必须优先于外部 Windows SDK。
# 维护约束：配置同步开关保持关闭以保护 Runner 用户目录。
# 维护约束：所有构建目录在删除前必须转换为绝对路径。
# 维护约束：删除防护不得放宽到允许项目根或文件系统根。
# 维护约束：Ninja 锁必须在 fresh 删除前获取并保持到脚本退出。
# 维护约束：同一绝对构建目录在所有 CI 任务中映射同一锁文件。
# 维护约束：不同构建目录可以并行执行而不共享锁。
# 维护约束：Ninja 恢复探针不得真正执行编译命令。
# 维护约束：依赖日志恢复后必须再次 dry-run 验证稳定状态。
# 维护约束：非 Ninja 生成器不运行 Ninja 专属恢复逻辑。
# 维护约束：预编译 target 清单变更时需同步各平台 staging 清单。
# 维护约束：第三方 target 名必须与 CMake 实际导出名称一致。
# 维护约束：configure-only 不应要求任何编译产物已经存在。
# 维护约束：构建失败直接传播 CMake 或 Ninja 的退出码。
# 维护约束：脚本不负责执行测试，测试由后续 CI 阶段调用。
# 维护约束：脚本不执行 strip，RelWithDebInfo 产物必须保留调试段。
# 维护约束：成功退出只表示请求目标构建完成，不表示已 staging。
# 维护约束：GCC 预设必须与 gcc14 可执行文件和目录标签成套更新。
# 维护约束：Clang 预设必须与 clang19 可执行文件和目录标签成套更新。
# 维护约束：覆盖单个编译器时调用方负责保证 C/C++ ABI 一致。
# 维护约束：编译器可执行文件在获取锁之前完成参数推导。
# 维护约束：构建锁按绝对 buildDir 而不是配置名称生成。
# 维护约束：锁文件名使用完整 SHA-256，避免路径字符和碰撞问题。
# 维护约束：锁目录位于 .git 下，不参与源码发布或安装。
# 维护约束：获取 flock 后不得提前关闭文件描述符 8。
# 维护约束：被取消的 Runner 释放进程后系统自动释放文件锁。
# 维护约束：fresh 删除发生在持锁状态，避免与旧构建并发。
# 维护约束：Ninja dry-run 只允许修复内部日志，不应执行编译。
# 维护约束：恢复提示的匹配文本变化时需同步 Ninja 版本验证。
# 维护约束：第二次仍出现恢复提示必须作为一致性错误处理。
# 维护约束：非恢复类 Ninja 输出继续保留到 stderr。
# 维护约束：CMake 配置成功后才检查 build.ninja 是否存在。
# 维护约束：系统 Vulkan 查找不得从 WINDOWS_CROSS_ROOT 派生。
# 维护约束：Vulkan cache 清理只匹配 Vulkan_ 前缀变量。
# 维护约束：BUILD_TESTING 保持开启以验证预编译测试依赖完整性。
# 维护约束：主项目与 ICE 的 Linux ABI 坐标必须完全一致。
# 维护约束：PROJECT_PREBUILT_PLATFORM 与 ICE 平台固定为 linux。
# 维护约束：宿主架构由 LFS helper 单独读取并验证。
# 维护约束：当前 staging 布局固定 x86_64 时 CI 必须使用匹配 Runner。
# 维护约束：CMAKE_POSITION_INDEPENDENT_CODE 对静态依赖保持开启。
# 维护约束：RelWithDebInfo Clang 自动 PGO 仅作用于业务模块。
# 维护约束：显式 PGO 开关必须覆盖 auto 判定。
# 维护约束：PGO_USE 始终关闭，profile 使用由独立构建阶段负责。
# 维护约束：切换编译器、linkage 或 SOURCES_BUILD 时应使用新目录。
# 维护约束：构建类型变化应重新验证预编译配置映射。
# 维护约束：生成器覆盖值必须与已有构建目录一致。
# 维护约束：测试执行必须使用隔离的 MMM_CONFIG_ROOT。
# 维护约束：本脚本不直接运行 ctest 或写入用户配置。
# 维护约束：构建产物不会由脚本复制到预编译目录。
# 维护约束：失败构建树默认保留供诊断，不自动清理。
# 维护约束：CI cache 键应包含编译器、tag、配置和 linkage。
# 维护约束：预编译依赖升级时需同步 LFS 指针和 Find 模块。
# 维护约束：新增 target 必须评估是否进入 prebuilt-targets 清单。
# 维护约束：参数中的路径始终使用双引号保留空格。
# 维护约束：续行反斜杠之后不得插入注释。
# 维护约束：环境变量 CC/CXX 只影响脚本子进程。
# 维护约束：脚本尾部不执行隐式 strip、install 或上传。
# 维护约束：正式发布前需消费 staging 后的预编译集合再构建。
# 维护约束：成功后调用方负责测试、打包或 staging 下一阶段。
# 维护约束：新增编译器族必须先扩展预编译目录与 CI 支持。
# 维护约束：脚本错误应保留底层 CMake、Ninja 和编译器诊断。
