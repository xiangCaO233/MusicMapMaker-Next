#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/mingw-clang-build.sh [options]

Configure and build the Windows MinGW clang cross target on Linux.

Options:
  --build-dir <path>     Build directory. Default: build_cross_mingw_clang
  --build-type <type>    CMake build type. Default: RelWithDebInfo
  --jobs <count>         Parallel build jobs. Default: 90% of CPU threads
  --linkage <mode>       PROJECT_LINKAGE value: static or shared. Default: static
  --sysroot <path>       MinGW sysroot. Default: ${WINDOWS_CROSS_ROOT}/msys64/clang64
  --toolchain <path>     CMake toolchain file. Default: cmake/toolchain/cross-mingw-clang.cmake
  --configure-only       Configure and generate, then stop
  --fresh                Remove the build directory before configuring
  -h, --help             Show this help

Environment overrides:
  MINGW_SYSROOT          MinGW sysroot path
  WINDOWS_CROSS_ROOT     Default: /mnt/cross/windows
  VULKAN_SDK             Default: ${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0
  CMAKE_GENERATOR        Default: Ninja
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

requireAnyCommand() {
    local label="$1"
    shift

    local commandName
    for commandName in "$@"; do
        if command -v "${commandName}" >/dev/null 2>&1; then
            return
        fi
    done

    printf "error: required command not found: %s (tried: %s)\n" "${label}" "$*" >&2
    exit 1
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
    if [[ -n "${MINGW_SYSROOT:-}" ]]; then
        printf "%s\n" "${MINGW_SYSROOT}"
        return
    fi

    local windowsCrossRoot="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
    local msys2Clang64Sysroot="${windowsCrossRoot}/msys64/clang64"
    if [[ -d "${msys2Clang64Sysroot}/include" && -d "${msys2Clang64Sysroot}/lib" ]]; then
        printf "%s\n" "${msys2Clang64Sysroot}"
        return
    fi

    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        x86_64-w64-mingw32-gcc -print-sysroot
        return
    fi

    printf "/usr/x86_64-w64-mingw32\n"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_clang"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
projectLinkage="static"
mingwSysroot="$(detectMingwSysroot)"
toolchainFile="cmake/toolchain/cross-mingw-clang.cmake"
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

if [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); then
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi

if [[ "${projectLinkage}" != "static" && "${projectLinkage}" != "shared" ]]; then
    printf "error: --linkage must be 'static' or 'shared'\n" >&2
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
export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

requireCommand cmake
requireAnyCommand clang clang-22 clang-21 clang-20 clang
requireAnyCommand clang++ clang++-22 clang++-21 clang++-20 clang++
requireCommand x86_64-w64-mingw32-windres
requireCommand x86_64-w64-mingw32-ar
requireCommand x86_64-w64-mingw32-ranlib
requireCommand x86_64-w64-mingw32-strip
requireCommand x86_64-w64-mingw32-objcopy

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
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if (( configureOnly )); then
    exit 0
fi

cmake --build "${buildDir}" --parallel "${buildJobs}"
