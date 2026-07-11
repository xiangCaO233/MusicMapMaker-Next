#!/usr/bin/env bash
set -euo pipefail

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
    local commandName

    for commandName in "$@"; do
        if command -v "${commandName}" >/dev/null 2>&1; then
            return 0
        fi
    done

    printf "error: none of the required commands were found:" >&2
    for commandName in "$@"; do
        printf " %s" "${commandName}" >&2
    done
    printf "\n" >&2
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

detectLlvmMingwRoot() {
    if [[ -n "${LLVM_MINGW_ROOT:-}" ]]; then
        printf "%s\n" "${LLVM_MINGW_ROOT}"
        return
    fi

    local candidate
    for candidate in "${HOME:-}/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64" "${HOME:-}"/llvm-mingw-*-ucrt-*-x86_64 /opt/llvm-mingw; do
        if [[ -d "${candidate}/bin" ]]; then
            printf "%s\n" "${candidate}"
            return
        fi
    done

    return 0
}

detectMingwSysroot() {
    local toolPrefix="$1"
    local llvmMingwRoot="$2"

    if [[ -n "${MINGW_SYSROOT:-}" ]]; then
        printf "%s\n" "${MINGW_SYSROOT}"
        return
    fi

    if [[ -n "${llvmMingwRoot}" ]]; then
        local detectedSysroot=""
        if command -v "${toolPrefix}-clang" >/dev/null 2>&1; then
            detectedSysroot="$("${toolPrefix}-clang" --print-sysroot 2>/dev/null || true)"
        fi
        if [[ -n "${detectedSysroot}" && -d "${detectedSysroot}" ]]; then
            printf "%s\n" "${detectedSysroot}"
            return
        fi

        local targetSysroot="${llvmMingwRoot}/${toolPrefix}"
        if [[ -d "${targetSysroot}/include" && -d "${targetSysroot}/lib" ]]; then
            printf "%s\n" "${targetSysroot}"
            return
        fi

        printf "%s\n" "${llvmMingwRoot}"
        return
    fi

    local windowsCrossRoot="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
    local msys2Clang64Sysroot="${windowsCrossRoot}/msys64/clang64"
    if [[ -d "${msys2Clang64Sysroot}/include" && -d "${msys2Clang64Sysroot}/lib" ]]; then
        printf "%s\n" "${msys2Clang64Sysroot}"
        return
    fi

    local prefixedSysroot="/usr/${toolPrefix}"
    if [[ -d "${prefixedSysroot}" ]]; then
        printf "%s\n" "${prefixedSysroot}"
        return
    fi

    if command -v "${toolPrefix}-gcc" >/dev/null 2>&1; then
        local detectedSysroot
        detectedSysroot="$("${toolPrefix}-gcc" -print-sysroot)"
        if [[ -n "${detectedSysroot}" ]]; then
            printf "%s\n" "${detectedSysroot}"
            return
        fi
    fi

    printf "/usr/x86_64-w64-mingw32\n"
}

verifyLlvmMingwUcrtRuntime() {
    local llvmMingwRoot="$1"
    local mingwSysroot="$2"

    if [[ -z "${llvmMingwRoot}" ]]; then
        return
    fi

    local msvcrtImport="${mingwSysroot}/lib/libmsvcrt.a"
    local ucrtImport="${mingwSysroot}/lib/libucrt.a"
    if [[ ! -f "${ucrtImport}" ]]; then
        printf "error: llvm-mingw sysroot does not provide UCRT import library: %s\n" "${ucrtImport}" >&2
        exit 1
    fi

    if [[ -f "${msvcrtImport}" ]]; then
        if ! cmp -s "${msvcrtImport}" "${ucrtImport}"; then
            printf "error: llvm-mingw libmsvcrt.a is not the UCRT compatibility import library: %s\n" "${msvcrtImport}" >&2
            exit 1
        fi
        printf "info: llvm-mingw UCRT runtime verified: libmsvcrt.a aliases libucrt.a\n"
        return
    fi

    printf "info: llvm-mingw UCRT runtime verified: %s\n" "${ucrtImport}"
}

verifyMingwClangCxxRuntime() {
    local toolPrefix="$1"
    local mingwSysroot="$2"
    local cxxCompiler=""

    if command -v "${toolPrefix}-clang++" >/dev/null 2>&1; then
        cxxCompiler="${toolPrefix}-clang++"
    elif command -v clang++-22 >/dev/null 2>&1; then
        cxxCompiler="clang++-22"
    elif command -v clang++ >/dev/null 2>&1; then
        cxxCompiler="clang++"
    else
        printf "error: unable to find MinGW clang++ for libc++ verification\n" >&2
        exit 1
    fi

    local macroOutput
    if ! macroOutput="$(printf "#include <vector>\n" | "${cxxCompiler}" --target=x86_64-w64-windows-gnu --sysroot="${mingwSysroot}" -stdlib=libc++ -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: unable to preprocess libc++ probe with %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    if ! grep -q "_LIBCPP_VERSION" <<<"${macroOutput}"; then
        printf "error: MinGW clang++ is not using libc++ headers\n" >&2
        exit 1
    fi
    if grep -q "__GLIBCXX__" <<<"${macroOutput}"; then
        printf "error: MinGW clang++ picked up libstdc++ headers\n" >&2
        exit 1
    fi

    printf "info: MinGW clang C++ runtime verified: libc++ (_LIBCPP_VERSION)\n"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_clang"
buildType="RelWithDebInfo"
buildJobs="$(detectBuildJobs)"
compilerTag="${MINGW_CLANG_PREBUILT_COMPILER_TAG:-clang64}"
projectLinkage="static"
toolPrefix="${MINGW_TOOLCHAIN_PREFIX:-x86_64-w64-mingw32}"
llvmMingwRoot="$(detectLlvmMingwRoot)"
mingwSysroot=""
toolchainFile="cmake/toolchain/cross-mingw-clang.cmake"
sourcesBuild="OFF"
prebuiltTargets=0
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
        --llvm-mingw-root)
            if (( $# < 2 )); then
                printf "error: --llvm-mingw-root requires a value\n" >&2
                exit 1
            fi
            llvmMingwRoot="$2"
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

if [[ -n "${llvmMingwRoot}" ]]; then
    llvmMingwRoot="$(projectPath "${llvmMingwRoot}")"
    if [[ ! -d "${llvmMingwRoot}/bin" ]]; then
        printf "error: LLVM_MINGW_ROOT does not contain a bin directory: %s\n" "${llvmMingwRoot}" >&2
        exit 1
    fi
    export LLVM_MINGW_ROOT="${llvmMingwRoot}"
    export PATH="${llvmMingwRoot}/bin:${PATH}"
fi

if [[ -z "${mingwSysroot}" ]]; then
    mingwSysroot="$(detectMingwSysroot "${toolPrefix}" "${llvmMingwRoot}")"
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
export MINGW_TOOLCHAIN_PREFIX="${toolPrefix}"
export MINGW_CLANG_PREBUILT_COMPILER_TAG="${compilerTag}"
export WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
export VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

requireCommand cmake
if [[ -n "${llvmMingwRoot}" ]]; then
    requireAnyCommand "${toolPrefix}-clang" clang-22 clang
    requireAnyCommand "${toolPrefix}-clang++" clang++-22 clang++
else
    requireCommand clang-22
    requireCommand clang++-22
fi
requireAnyCommand "${toolPrefix}-windres" llvm-windres
requireAnyCommand "${toolPrefix}-ar" llvm-ar
requireAnyCommand "${toolPrefix}-ranlib" llvm-ranlib
requireAnyCommand "${toolPrefix}-strip" llvm-strip
requireAnyCommand "${toolPrefix}-objcopy" llvm-objcopy

if [[ ! -d "${MINGW_SYSROOT}" ]]; then
    printf "error: MINGW_SYSROOT does not exist: %s\n" "${MINGW_SYSROOT}" >&2
    exit 1
fi
verifyLlvmMingwUcrtRuntime "${llvmMingwRoot}" "${MINGW_SYSROOT}"
verifyMingwClangCxxRuntime "${toolPrefix}" "${MINGW_SYSROOT}"

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
        luajit_build
else
    cmake --build "${buildDir}" --parallel "${buildJobs}"
fi
