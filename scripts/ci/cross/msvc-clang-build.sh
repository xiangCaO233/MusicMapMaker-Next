#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/msvc-clang-build.sh [options]

Configure and build the Windows MSVC-like clang-cl cross target.

Options:
  --build-dir <path>     Build directory. Default: build_cross_msvc
  --build-type <type>    CMake build type. Default: RelWithDebInfo
  --jobs <count>         Parallel build jobs. Default: 75% of CPU threads
  --linkage <mode>       PROJECT_LINKAGE value: static or shared. Default: static
  --toolchain <path>     CMake toolchain file. Default: cmake/toolchain/cross-msvc.cmake
  --configure-only       Configure and generate, then stop
  --fresh                Remove the build directory before configuring
  -h, --help             Show this help

Environment overrides:
  WINDOWS_CROSS_ROOT     Default: /mnt/cross/windows
  VCPKG_ROOT             Default: ${WINDOWS_CROSS_ROOT}/vcpkg
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

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_msvc"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
projectLinkage="static"
toolchainFile="cmake/toolchain/cross-msvc.cmake"
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

if [[ ! -f "${toolchainFile}" ]]; then
    printf "error: toolchain file not found: %s\n" "${toolchainFile}" >&2
    exit 1
fi

export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
export VCPKG_ROOT="${VCPKG_ROOT:-${WINDOWS_CROSS_ROOT}/vcpkg}"
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

requireCommand cmake
requireAnyCommand clang-cl clang-cl-22 clang-cl-21 clang-cl-20 clang-cl
requireAnyCommand lld-link lld-link-22 lld-link-21 lld-link-20 lld-link

if [[ ! -d "${VCPKG_ROOT}" ]]; then
    printf "error: VCPKG_ROOT does not exist: %s\n" "${VCPKG_ROOT}" >&2
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
    -DPROJECT_LINKAGE="${projectLinkage}" \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S "${projectRoot}" \
    -B "${buildDir}"

if (( configureOnly )); then
    exit 0
fi

cmake --build "${buildDir}" --parallel "${buildJobs}"
