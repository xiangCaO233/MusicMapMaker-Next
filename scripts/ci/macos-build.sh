#!/usr/bin/env bash
set -euo pipefail

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

detectBuildJobs() {
    local maxThreads=1

    if command -v sysctl >/dev/null 2>&1; then
        maxThreads="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
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

normalizeArchitecture() {
    case "$1" in
        arm64 | aarch64)
            printf "arm64\n"
            ;;
        x86_64 | amd64)
            printf "x86_64\n"
            ;;
        *)
            printf "error: unsupported macOS architecture: %s\n" "$1" >&2
            exit 1
            ;;
    esac
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

detectClangCompilerTag() {
    local macroOutput
    if ! macroOutput="$(printf "\n" | "${cxxCompiler}" -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: failed to query Clang predefined macros: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    local clangMajor
    clangMajor="$(awk '$2 == "__clang_major__" { print $3; exit }' <<<"${macroOutput}")"
    if [[ ! "${clangMajor}" =~ ^[0-9]+$ ]]; then
        printf "error: failed to detect Clang major version from: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    printf "clang%s\n" "${clangMajor}"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "error: scripts/ci/macos-build.sh must run on macOS\n" >&2
    exit 1
fi

requireCommand cmake
requireCommand xcrun

targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
ccCompiler=""
cxxCompiler=""
macosSdk="${MACOS_SDK:-}"
deploymentTarget="${MACOSX_DEPLOYMENT_TARGET:-}"
buildDir=""
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
prebuiltToolchain="${MACOS_PREBUILT_TOOLCHAIN:-clang}"
compilerTag="${MACOS_PREBUILT_COMPILER_TAG:-}"
projectLinkage="static"
sourcesBuild="OFF"
prebuiltTargets=0
configureOnly=0
freshBuild=0

while (( $# > 0 )); do
    case "$1" in
        --arch)
            if (( $# < 2 )); then
                printf "error: --arch requires a value\n" >&2
                exit 1
            fi
            targetArch="$2"
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
        --sdk)
            if (( $# < 2 )); then
                printf "error: --sdk requires a value\n" >&2
                exit 1
            fi
            macosSdk="$2"
            shift 2
            ;;
        --deployment-target)
            if (( $# < 2 )); then
                printf "error: --deployment-target requires a value\n" >&2
                exit 1
            fi
            deploymentTarget="$2"
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

targetArch="$(normalizeArchitecture "${targetArch}")"
hostArch="$(normalizeArchitecture "$(uname -m)")"
if [[ "${targetArch}" != "${hostArch}" ]]; then
    printf "error: macOS prebuilt source builds currently require the native host architecture: requested %s, host %s\n" \
        "${targetArch}" "${hostArch}" >&2
    exit 1
fi
if [[ -z "${deploymentTarget}" ]]; then
    deploymentTarget="11.0"
fi

if [[ -z "${ccCompiler}" ]]; then
    ccCompiler="$(xcrun --find clang)"
fi
if [[ -z "${cxxCompiler}" ]]; then
    cxxCompiler="$(xcrun --find clang++)"
fi
if [[ -z "${macosSdk}" ]]; then
    macosSdk="$(xcrun --sdk macosx --show-sdk-path)"
elif [[ ! -d "${macosSdk}" ]]; then
    requestedMacosSdk="${macosSdk}"
    if ! macosSdk="$(xcrun --sdk "${requestedMacosSdk}" --show-sdk-path 2>/dev/null)"; then
        printf "error: failed to resolve macOS SDK: %s\n" "${requestedMacosSdk}" >&2
        exit 1
    fi
fi

requireCommand "${ccCompiler}"
requireCommand "${cxxCompiler}"

if [[ -z "${compilerTag}" ]]; then
    compilerTag="$(detectClangCompilerTag)"
fi

if [[ -z "${buildDir}" ]]; then
    if [[ "${sourcesBuild}" = "ON" ]]; then
        buildDir="build_macos_sources_${targetArch}"
    else
        buildDir="build_macos_${targetArch}"
    fi
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
        --platform macos \
        --arch "${targetArch}" \
        --toolchain "${prebuiltToolchain}" \
        --compiler-tag "${compilerTag}" \
        --build-type "${buildType}" \
        --linkage "${projectLinkage}" \
        --include-tests
fi

disableClangLto="OFF"
if [[ "${sourcesBuild}" = "ON" ]]; then
    disableClangLto="ON"
fi

buildDir="$(projectPath "${buildDir}")"

if (( freshBuild )); then
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

export CC="${ccCompiler}"
export CXX="${cxxCompiler}"
export SDKROOT="${macosSdk}"
export MACOSX_DEPLOYMENT_TARGET="${deploymentTarget}"

cmakeArgs=(
    -G "${CMAKE_GENERATOR:-Ninja}"
    -DCMAKE_BUILD_TYPE="${buildType}"
    -DBUILD_TESTING=ON
    -DCMAKE_OSX_ARCHITECTURES="${targetArch}"
    -DCMAKE_OSX_SYSROOT="${macosSdk}"
    -DSOURCES_BUILD="${sourcesBuild}"
    -DPROJECT_LINKAGE="${projectLinkage}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DPROJECT_PREBUILT_PLATFORM=macos
    -DPROJECT_PREBUILT_ARCH="${targetArch}"
    -DPROJECT_PREBUILT_TOOLCHAIN="${prebuiltToolchain}"
    -DPROJECT_PREBUILT_COMPILER_TAG="${compilerTag}"
    -DICE_PREBUILT_PLATFORM=macos
    -DICE_PREBUILT_ARCH="${targetArch}"
    -DICE_PREBUILT_TOOLCHAIN="${prebuiltToolchain}"
    -DICE_PREBUILT_COMPILER_TAG="${compilerTag}"
    -DICE_LINKAGE="${projectLinkage}"
    -DMMM_DISABLE_CLANG_LTO="${disableClangLto}"
    -DMMM_PGO_INSTRUMENT=OFF
    -DMMM_PGO_USE=OFF
    -DMMM_MACOS_CODESIGN_IDENTITY="${MACOS_CODESIGN_IDENTITY:--}"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${deploymentTarget}"
)
cmakeArgs+=(-S "${projectRoot}" -B "${buildDir}")

cmake "${cmakeArgs[@]}"

if (( configureOnly )); then
    exit 0
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
