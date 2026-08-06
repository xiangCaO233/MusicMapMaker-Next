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

if (( freshBuild )); then
    if [[ -z "${buildDir}" || "${buildDir}" == "/" || "${buildDir}" == "${projectRoot}" ]]; then
        printf "error: refusing to remove unsafe build directory: %s\n" "${buildDir}" >&2
        exit 1
    fi
    rm -rf -- "${buildDir}"
fi

export CC="${ccCompiler}"
export CXX="${cxxCompiler}"

cmake -G "${CMAKE_GENERATOR:-Ninja}" \
    -DCMAKE_BUILD_TYPE="${buildType}" \
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
