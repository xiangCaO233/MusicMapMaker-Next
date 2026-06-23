#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/mingw-gcc-build.sh [options]

Configure and build the Windows MinGW GCC cross target on Linux.

Options:
  --build-dir <path>      Build directory. Default: build_cross_mingw_gcc
  --build-type <type>     CMake build type. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: clang64
  --jobs <count>          Parallel build jobs. Default: 90% of CPU threads
  --linkage <mode>        PROJECT_LINKAGE value: static or shared. Default: static
  --prefix <prefix>       MinGW tool prefix. Default: x86_64-w64-mingw32
  --sysroot <path>        MinGW sysroot. Default: <prefix>-gcc -print-sysroot
  --toolchain <path>      CMake toolchain file. Default: cmake/toolchain/cross-mingw-gcc.cmake
  --configure-only        Configure and generate, then stop
  --fresh                 Remove the build directory before configuring
  -h, --help              Show this help

Environment overrides:
  MINGW_SYSROOT                    MinGW sysroot path
  MINGW_GCC_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
  VULKAN_SDK                       Default: /mnt/windows_c/VulkanSDK/1.4.341.1
  CMAKE_GENERATOR                  Default: Ninja
EOF
}

detectBuildJobs() {
    local maxThreads=1

    if command -v nproc >/dev/null 2>&1; then
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        maxThreads=1
    fi

    local buildJobs=$(( maxThreads * 9 / 10 ))
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

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

detectMingwSysroot() {
    local toolPrefix="$1"

    if [[ -n "${MINGW_SYSROOT:-}" ]]; then
        printf "%s\n" "${MINGW_SYSROOT}"
        return
    fi

    if command -v "${toolPrefix}-gcc" >/dev/null 2>&1; then
        "${toolPrefix}-gcc" -print-sysroot
        return
    fi

    printf "/usr/lib/mingw64-toolchain\n"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_gcc"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
compilerTag="${MINGW_GCC_PREBUILT_COMPILER_TAG:-clang64}"
projectLinkage="static"
toolPrefix="x86_64-w64-mingw32"
mingwSysroot=""
toolchainFile="cmake/toolchain/cross-mingw-gcc.cmake"
configureOnly=0
freshBuild=0

while (( $# > 0 )); do
    case "$1" in
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
        --prefix)
            if (( $# < 2 )); then
                printf "error: --prefix requires a value\n" >&2
                exit 1
            fi
            toolPrefix="$2"
            shift 2
            ;;
        --sysroot)
            if (( $# < 2 )); then
                printf "error: --sysroot requires a value\n" >&2
                exit 1
            fi
            mingwSysroot="$2"
            shift 2
            ;;
        --toolchain)
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            toolchainFile="$2"
            shift 2
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

if [[ -z "${mingwSysroot}" ]]; then
    mingwSysroot="$(detectMingwSysroot "${toolPrefix}")"
fi

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
    exit 1
fi

if [[ -z "${compilerTag}" ]]; then
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

buildDir="$(projectPath "${buildDir}")"
toolchainFile="$(projectPath "${toolchainFile}")"
mingwSysroot="$(projectPath "${mingwSysroot}")"

if [[ ! -f "${toolchainFile}" ]]; then
    printf "error: toolchain file not found: %s\n" "${toolchainFile}" >&2
    exit 1
fi

export MINGW_SYSROOT="${mingwSysroot}"
export MINGW_GCC_PREBUILT_COMPILER_TAG="${compilerTag}"
export VULKAN_SDK="${VULKAN_SDK:-/mnt/windows_c/VulkanSDK/1.4.341.1}"

requireCommand cmake
requireCommand "${toolPrefix}-gcc"
requireCommand "${toolPrefix}-g++"
requireCommand "${toolPrefix}-windres"

if [[ ! -d "${MINGW_SYSROOT}" ]]; then
    printf "error: MINGW_SYSROOT does not exist: %s\n" "${MINGW_SYSROOT}" >&2
    exit 1
fi

if [[ ! -d "${VULKAN_SDK}" ]]; then
    printf "error: VULKAN_SDK does not exist: %s\n" "${VULKAN_SDK}" >&2
    exit 1
fi

if (( freshBuild )); then
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

cmake -G "${CMAKE_GENERATOR:-Ninja}" \
    -DCMAKE_BUILD_TYPE="${buildType}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchainFile}" \
    -DMINGW_SYSROOT="${MINGW_SYSROOT}" \
    -DMINGW_TOOLCHAIN_PREFIX="${toolPrefix}" \
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}" \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if (( configureOnly )); then
    exit 0
fi

cmake --build "${buildDir}" --parallel "${buildJobs}"
