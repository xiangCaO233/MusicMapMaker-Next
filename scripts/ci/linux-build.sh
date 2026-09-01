#!/usr/bin/env bash
set -euo pipefail

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

detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 3 / 4 ))
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    printf "%s\n" "${buildJobs}"
}

requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

verifyNinjaState() {
    local ninjaOutput

    # 自托管 Runner 被取消时，Ninja 可能只写入半条依赖记录。首次加载会自动截断至
    # 最后一条完整记录；再次加载必须静默，确保恢复已经落盘且不会污染后续 CI 日志。
    if ! ninjaOutput="$(ninja -C "$1" -n 2>&1 >/dev/null)"; then
        printf "%s\n" "${ninjaOutput}" >&2
        return 1
    fi

    if [[ "${ninjaOutput}" == *"premature end of file; recovering"* ]]; then
        printf "warning: recovered interrupted Ninja dependency log in %s\n" "$1" >&2
        if ! ninjaOutput="$(ninja -C "$1" -n 2>&1 >/dev/null)"; then
            printf "%s\n" "${ninjaOutput}" >&2
            return 1
        fi
    fi

    if [[ -n "${ninjaOutput}" ]]; then
        printf "%s\n" "${ninjaOutput}" >&2
    fi

    if [[ "${ninjaOutput}" == *"premature end of file; recovering"* ]]; then
        printf "error: Ninja dependency log remains truncated after recovery\n" >&2
        return 1
    fi
}

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

applyCompilerPreset() {
    case "${compilerPreset}" in
        gcc14)
            ccCompiler="${ccCompiler:-gcc-14}"
            cxxCompiler="${cxxCompiler:-g++-14}"
            prebuiltToolchain="${prebuiltToolchain:-gcc}"
            compilerTag="${compilerTag:-gcc14}"
            ;;
        clang19)
            ccCompiler="${ccCompiler:-clang-19}"
            cxxCompiler="${cxxCompiler:-clang++-19}"
            prebuiltToolchain="${prebuiltToolchain:-clang}"
            compilerTag="${compilerTag:-clang19}"
            ;;
        *)
            printf "error: unsupported --compiler preset: %s\n" "${compilerPreset}" >&2
            exit 1
            ;;
    esac
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

compilerPreset="${LINUX_PREBUILT_COMPILER:-gcc14}"
ccCompiler=""
cxxCompiler=""
buildDir=""
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
prebuiltToolchain="${LINUX_PREBUILT_TOOLCHAIN:-}"
compilerTag="${LINUX_PREBUILT_COMPILER_TAG:-}"
projectLinkage="static"
sourcesBuild="OFF"
prebuiltTargets=0
configureOnly=0
freshBuild=0
pgoInstrument="auto"

while (( $# > 0 )); do
    case "$1" in
        --compiler)
            if (( $# < 2 )); then
                printf "error: --compiler requires a value\n" >&2
                exit 1
            fi
            compilerPreset="$2"
            shift 2
            ;;
        --cc)
            if (( $# < 2 )); then
                printf "error: --cc requires a value\n" >&2
                exit 1
            fi
            ccCompiler="$2"
            shift 2
            ;;
        --cxx)
            if (( $# < 2 )); then
                printf "error: --cxx requires a value\n" >&2
                exit 1
            fi
            cxxCompiler="$2"
            shift 2
            ;;
        --build-dir)
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --toolchain)
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --jobs)
            if (( $# < 2 )); then
                printf "error: --jobs requires a value\n" >&2
                exit 1
            fi
            buildJobs="$2"
            shift 2
            ;;
        --linkage)
            if (( $# < 2 )); then
                printf "error: --linkage requires a value\n" >&2
                exit 1
            fi
            projectLinkage="$2"
            shift 2
            ;;
        --pgo-instrument)
            pgoInstrument="ON"
            shift
            ;;
        --no-pgo-instrument)
            pgoInstrument="OFF"
            shift
            ;;
        --sources-build)
            sourcesBuild="ON"
            shift
            ;;
        --prebuilt-targets)
            prebuiltTargets=1
            shift
            ;;
        --configure-only)
            configureOnly=1
            shift
            ;;
        --fresh)
            freshBuild=1
            shift
            ;;
        -h | --help)
            showUsage
            exit 0
            ;;
        *)
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

applyCompilerPreset

disableClangLto="OFF"
if [[ "${sourcesBuild}" = "ON" ]]; then
    disableClangLto="ON"
fi

if [[ -z "${buildDir}" ]]; then
    buildDir="build_linux_${compilerPreset,,}"
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
    exit 1
fi

if [[ "${sourcesBuild}" == "OFF" ]]; then
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
    pgoInstrument="OFF"
    if [[ "${buildType}" == "RelWithDebInfo" && "${prebuiltToolchain}" == "clang" ]]; then
        pgoInstrument="ON"
    fi
fi

requireCommand cmake
requireCommand "${ccCompiler}"
requireCommand "${cxxCompiler}"

buildDir="$(projectPath "${buildDir}")"

# cancel-in-progress 可能在上一个 Ninja 进程退出前启动下一轮；按构建目录串行化，
# 避免两个进程并发写入同一个 .ninja_deps。锁文件放在构建目录外，防止 --fresh 删除已持有的锁。
requireCommand flock
requireCommand sha256sum
ninjaLockDir="${projectRoot}/.git/mmm-ci-ninja-locks"
mkdir -p "${ninjaLockDir}"
ninjaLockName="$(printf "%s" "${buildDir}" | sha256sum | awk '{ print $1 }')"
exec 8>"${ninjaLockDir}/${ninjaLockName}.lock"
flock 8

if (( freshBuild )); then
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

export CC="${ccCompiler}"
export CXX="${cxxCompiler}"

# 原生 Linux 构建必须使用系统 Vulkan，避免 Runner 注入的 Windows SDK 污染头文件搜索路径。
unset VULKAN_SDK VK_SDK_PATH

# CI 与预编译构建不得写入 Runner 的用户配置目录。
cmake -U "Vulkan_*" \
    -G "${CMAKE_GENERATOR:-Ninja}" \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DBUILD_TESTING=ON \
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
    exit 0
fi

if [[ -f "${buildDir}/build.ninja" ]]; then
    requireCommand ninja
    verifyNinjaState "${buildDir}"
fi

if (( prebuiltTargets )); then
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
    cmake --build "${buildDir}" --parallel "${buildJobs}"
fi
