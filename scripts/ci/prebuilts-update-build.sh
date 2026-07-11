#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/prebuilts-update-build.sh [options]

Configure, build, and stage all source-built prebuilt libraries used by CI.

Default matrix:
  Windows MSVC 2026:    clang-cl 22 + lld-link + MSVC/UCRT
  Windows MinGW clang64: clang 22 llvm-mingw UCRT + libc++
  Windows MinGW ucrt64:  GCC 14 UCRT64
  Linux gcc14:           native GCC 14
  Linux clang19:         native Clang 19

Options:
  --configs <list>             Space/comma separated CMake configs. Default: Debug RelWithDebInfo
  --build-root <path>          Root for generated build directories. Default: build_prebuilts_update
  --jobs <count>               Parallel build jobs passed to child build scripts.
  --linkage <static>           PROJECT_LINKAGE/ICE_LINKAGE value. Default: static.
                               Shared staging is not implemented in this script yet.
  --llvm-mingw-root <path>     Complete llvm-mingw UCRT toolchain root for clang64.
  --configure-only             Run only the configure-only phase.
  --reuse-build-dirs           Do not remove build directories during the configure-only phase.
  --skip-windows               Skip all Windows cross prebuilt libraries.
  --skip-linux                 Skip all Linux native prebuilt libraries.
  --skip-msvc                  Skip Windows msvc/2026 prebuilts.
  --skip-mingw-clang           Skip Windows mingw/clang64 prebuilts.
  --skip-mingw-gcc             Skip Windows mingw/ucrt64 prebuilts.
  --skip-linux-gcc             Skip Linux gcc/gcc14 prebuilts.
  --skip-linux-clang           Skip Linux clang/clang19 prebuilts.
  -h, --help                   Show this help

Environment overrides:
  PREBUILTS_UPDATE_CONFIGS       Default configs list
  PREBUILTS_UPDATE_BUILD_ROOT    Default build root
  PREBUILTS_UPDATE_JOBS          Default parallel jobs
  LLVM_MINGW_ROOT                Default llvm-mingw UCRT toolchain root
  CMAKE_GENERATOR                Passed through to child build scripts. Default: Ninja
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

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

setConfigs() {
    local rawConfigs="${1//,/ }"

    read -r -a configs <<<"${rawConfigs}"
    if (( ${#configs[@]} == 0 )); then
        printf "error: --configs must not be empty\n" >&2
        exit 1
    fi
}

validateConfig() {
    case "$1" in
        Debug | Release | RelWithDebInfo | MinSizeRel)
            ;;
        *)
            printf "error: unsupported CMake config: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

runCommand() {
    local label="$1"
    shift

    printf "\n==> %s\n" "${label}"
    printf "    "
    printf "%q " "$@"
    printf "\n"
    "$@"
}

msvcBuildDir() {
    printf "%s/windows-msvc-2026-%s\n" "${buildRoot}" "$1"
}

mingwClangBuildDir() {
    printf "%s/windows-mingw-clang64-%s\n" "${buildRoot}" "$1"
}

mingwGccBuildDir() {
    printf "%s/windows-mingw-ucrt64-%s\n" "${buildRoot}" "$1"
}

linuxGccBuildDir() {
    printf "%s/linux-gcc14-%s\n" "${buildRoot}" "$1"
}

linuxClangBuildDir() {
    printf "%s/linux-clang19-%s\n" "${buildRoot}" "$1"
}

runMsvcConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(msvcBuildDir "${config}")"

    local command=(
        bash
        "${crossScriptDir}/msvc-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag 2026
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        command+=(--fresh)
    fi

    runCommand "configure windows msvc/2026 ${config}" "${command[@]}"
}

runMsvcBuild() {
    local config="$1"
    local buildDir
    buildDir="$(msvcBuildDir "${config}")"

    runCommand "build windows msvc/2026 ${config}" \
        bash \
        "${crossScriptDir}/msvc-clang-build.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag 2026 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --sources-build \
        --prebuilt-targets
    runCommand "stage windows msvc/2026 ${config}" \
        bash \
        "${crossScriptDir}/stage-msvc-clang-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag 2026 \
        --strict-symbols
}

runMingwClangConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(mingwClangBuildDir "${config}")"

    local command=(
        bash
        "${crossScriptDir}/mingw-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag clang64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if [[ -n "${llvmMingwRoot}" ]]; then
        command+=(--llvm-mingw-root "${llvmMingwRoot}")
    fi
    if (( freshConfigure )); then
        command+=(--fresh)
    fi

    runCommand "configure windows mingw/clang64 ${config}" "${command[@]}"
}

runMingwClangBuild() {
    local config="$1"
    local buildDir
    buildDir="$(mingwClangBuildDir "${config}")"

    local command=(
        bash
        "${crossScriptDir}/mingw-clang-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag clang64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
    )
    if [[ -n "${llvmMingwRoot}" ]]; then
        command+=(--llvm-mingw-root "${llvmMingwRoot}")
    fi

    runCommand "build windows mingw/clang64 ${config}" "${command[@]}"
    runCommand "stage windows mingw/clang64 ${config}" \
        bash \
        "${crossScriptDir}/stage-mingw-clang-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag clang64
}

runMingwGccConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(mingwGccBuildDir "${config}")"

    local command=(
        bash
        "${crossScriptDir}/mingw-gcc-build.sh"
        --build-dir "${buildDir}"
        --build-type "${config}"
        --compiler-tag ucrt64
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        command+=(--fresh)
    fi

    runCommand "configure windows mingw/ucrt64 ${config}" "${command[@]}"
}

runMingwGccBuild() {
    local config="$1"
    local buildDir
    buildDir="$(mingwGccBuildDir "${config}")"

    runCommand "build windows mingw/ucrt64 ${config}" \
        bash \
        "${crossScriptDir}/mingw-gcc-build.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag ucrt64 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --sources-build \
        --prebuilt-targets
    runCommand "stage windows mingw/ucrt64 ${config}" \
        bash \
        "${crossScriptDir}/stage-mingw-gcc-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --compiler-tag ucrt64
}

runLinuxGccConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(linuxGccBuildDir "${config}")"

    local command=(
        bash
        "${ciScriptDir}/linux-build.sh"
        --compiler gcc14
        --build-dir "${buildDir}"
        --build-type "${config}"
        --toolchain gcc
        --compiler-tag gcc14
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --no-pgo-instrument
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        command+=(--fresh)
    fi

    runCommand "configure linux gcc/gcc14 ${config}" "${command[@]}"
}

runLinuxGccBuild() {
    local config="$1"
    local buildDir
    buildDir="$(linuxGccBuildDir "${config}")"

    runCommand "build linux gcc/gcc14 ${config}" \
        bash \
        "${ciScriptDir}/linux-build.sh" \
        --compiler gcc14 \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain gcc \
        --compiler-tag gcc14 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --no-pgo-instrument \
        --sources-build \
        --prebuilt-targets
    runCommand "stage linux gcc/gcc14 ${config}" \
        bash \
        "${ciScriptDir}/stage-linux-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain gcc \
        --compiler-tag gcc14
}

runLinuxClangConfigure() {
    local config="$1"
    local buildDir
    buildDir="$(linuxClangBuildDir "${config}")"

    local command=(
        bash
        "${ciScriptDir}/linux-build.sh"
        --compiler clang19
        --build-dir "${buildDir}"
        --build-type "${config}"
        --toolchain clang
        --compiler-tag clang19
        --jobs "${buildJobs}"
        --linkage "${projectLinkage}"
        --no-pgo-instrument
        --sources-build
        --prebuilt-targets
        --configure-only
    )
    if (( freshConfigure )); then
        command+=(--fresh)
    fi

    runCommand "configure linux clang/clang19 ${config}" "${command[@]}"
}

runLinuxClangBuild() {
    local config="$1"
    local buildDir
    buildDir="$(linuxClangBuildDir "${config}")"

    runCommand "build linux clang/clang19 ${config}" \
        bash \
        "${ciScriptDir}/linux-build.sh" \
        --compiler clang19 \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain clang \
        --compiler-tag clang19 \
        --jobs "${buildJobs}" \
        --linkage "${projectLinkage}" \
        --no-pgo-instrument \
        --sources-build \
        --prebuilt-targets
    runCommand "stage linux clang/clang19 ${config}" \
        bash \
        "${ciScriptDir}/stage-linux-prebuilts.sh" \
        --build-dir "${buildDir}" \
        --build-type "${config}" \
        --toolchain clang \
        --compiler-tag clang19
}

runConfigurePhase() {
    local config
    for config in "${configs[@]}"; do
        validateConfig "${config}"
        if (( enableMsvc )); then
            runMsvcConfigure "${config}"
        fi
        if (( enableMingwClang )); then
            runMingwClangConfigure "${config}"
        fi
        if (( enableMingwGcc )); then
            runMingwGccConfigure "${config}"
        fi
        if (( enableLinuxGcc )); then
            runLinuxGccConfigure "${config}"
        fi
        if (( enableLinuxClang )); then
            runLinuxClangConfigure "${config}"
        fi
    done
}

runBuildPhase() {
    local config
    for config in "${configs[@]}"; do
        if (( enableMsvc )); then
            runMsvcBuild "${config}"
        fi
        if (( enableMingwClang )); then
            runMingwClangBuild "${config}"
        fi
        if (( enableMingwGcc )); then
            runMingwGccBuild "${config}"
        fi
        if (( enableLinuxGcc )); then
            runLinuxGccBuild "${config}"
        fi
        if (( enableLinuxClang )); then
            runLinuxClangBuild "${config}"
        fi
    done
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ciScriptDir="${scriptDir}"
crossScriptDir="${ciScriptDir}/cross"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

defaultConfigs="${PREBUILTS_UPDATE_CONFIGS:-Debug RelWithDebInfo}"
configs=()
setConfigs "${defaultConfigs}"

buildRoot="${PREBUILTS_UPDATE_BUILD_ROOT:-build_prebuilts_update}"
buildJobs="${PREBUILTS_UPDATE_JOBS:-$(detectBuildJobs)}"
projectLinkage="static"
llvmMingwRoot="${LLVM_MINGW_ROOT:-}"
configureOnly=0
freshConfigure=1
enableMsvc=1
enableMingwClang=1
enableMingwGcc=1
enableLinuxGcc=1
enableLinuxClang=1

while (( $# > 0 )); do
    case "$1" in
        --configs)
            if (( $# < 2 )); then
                printf "error: --configs requires a value\n" >&2
                exit 1
            fi
            setConfigs "$2"
            shift 2
            ;;
        --build-root)
            if (( $# < 2 )); then
                printf "error: --build-root requires a value\n" >&2
                exit 1
            fi
            buildRoot="$2"
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
        --configure-only)
            configureOnly=1
            shift
            ;;
        --reuse-build-dirs)
            freshConfigure=0
            shift
            ;;
        --skip-windows)
            enableMsvc=0
            enableMingwClang=0
            enableMingwGcc=0
            shift
            ;;
        --skip-linux)
            enableLinuxGcc=0
            enableLinuxClang=0
            shift
            ;;
        --skip-msvc)
            enableMsvc=0
            shift
            ;;
        --skip-mingw-clang)
            enableMingwClang=0
            shift
            ;;
        --skip-mingw-gcc)
            enableMingwGcc=0
            shift
            ;;
        --skip-linux-gcc)
            enableLinuxGcc=0
            shift
            ;;
        --skip-linux-clang)
            enableLinuxClang=0
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

if [[ "${projectLinkage}" != "static" ]]; then
    printf "error: --linkage currently supports only 'static'; shared staging needs separate bin/libs/symbols layout support\n" >&2
    exit 1
fi

if (( ! enableMsvc && ! enableMingwClang && ! enableMingwGcc && ! enableLinuxGcc && ! enableLinuxClang )); then
    printf "error: no prebuilt target is enabled\n" >&2
    exit 1
fi

buildRoot="$(projectPath "${buildRoot}")"
if [[ -n "${llvmMingwRoot}" ]]; then
    llvmMingwRoot="$(projectPath "${llvmMingwRoot}")"
fi

printf "Prebuilt update matrix:\n"
printf "  configs: %s\n" "${configs[*]}"
printf "  build root: %s\n" "${buildRoot}"
printf "  jobs: %s\n" "${buildJobs}"
printf "  linkage: %s\n" "${projectLinkage}"
printf "  fresh configure: %s\n" "${freshConfigure}"
printf "  targets:"
if (( enableMsvc )); then
    printf " windows-msvc-2026"
fi
if (( enableMingwClang )); then
    printf " windows-mingw-clang64"
fi
if (( enableMingwGcc )); then
    printf " windows-mingw-ucrt64"
fi
if (( enableLinuxGcc )); then
    printf " linux-gcc14"
fi
if (( enableLinuxClang )); then
    printf " linux-clang19"
fi
printf "\n"

runConfigurePhase

if (( configureOnly )); then
    printf "\nconfigure-only phase completed.\n"
    exit 0
fi

runBuildPhase
