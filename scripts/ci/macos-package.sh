#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/macos-package.sh [options]

Build the native macOS app bundle and package it as a CPack DragNDrop DMG.

Options:
  --arch <arm64|x86_64>      Target architecture. Default: host architecture
  --build-dir <path>         Build directory. Default: build_macos_package_<arch>
  --package-dir <path>       DMG output directory. Default: <build-dir>/packages
  --build-type <type>        CMake build type. Default: RelWithDebInfo
  --deployment-target <ver>  Minimum macOS deployment target. Default: 11.0
  --jobs <count>             Parallel build jobs passed to macos-build.sh
  --codesign-identity <name> Code-signing identity. Default: - (ad-hoc)
  --no-codesign              Do not sign the generated app bundle
  --package-only             Skip configure/build and package an existing build
  --fresh                    Remove the build directory before configuring
  -h, --help                 Show this help

Environment overrides:
  MACOS_PREBUILT_ARCH       Default target architecture
  MACOSX_DEPLOYMENT_TARGET  Default minimum macOS deployment target
  MACOS_CODESIGN_IDENTITY   Default code-signing identity
EOF
}

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
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

requireOptionValue() {
    local optionName="$1"
    local argumentCount="$2"

    if (( argumentCount < 2 )); then
        printf "error: %s requires a value\n" "${optionName}" >&2
        exit 1
    fi
}

requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "error: scripts/ci/macos-package.sh must run on macOS\n" >&2
    exit 1
fi

requireCommand cpack

targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
buildDir=""
packageDir=""
buildType="RelWithDebInfo"
deploymentTarget="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
buildJobs=""
codesignIdentity="${MACOS_CODESIGN_IDENTITY:--}"
packageOnly=0
freshBuild=0

while (( $# > 0 )); do
    case "$1" in
        --arch)
            requireOptionValue "$1" "$#"
            targetArch="$2"
            shift 2
            ;;
        --build-dir)
            requireOptionValue "$1" "$#"
            buildDir="$2"
            shift 2
            ;;
        --package-dir)
            requireOptionValue "$1" "$#"
            packageDir="$2"
            shift 2
            ;;
        --build-type)
            requireOptionValue "$1" "$#"
            buildType="$2"
            shift 2
            ;;
        --deployment-target)
            requireOptionValue "$1" "$#"
            deploymentTarget="$2"
            shift 2
            ;;
        --jobs)
            requireOptionValue "$1" "$#"
            buildJobs="$2"
            shift 2
            ;;
        --codesign-identity)
            requireOptionValue "$1" "$#"
            codesignIdentity="$2"
            shift 2
            ;;
        --no-codesign)
            codesignIdentity=""
            shift
            ;;
        --package-only)
            packageOnly=1
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
if [[ -n "${buildJobs}" ]] &&
    { [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); }; then
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi
if (( packageOnly && freshBuild )); then
    printf "error: --package-only and --fresh cannot be used together\n" >&2
    exit 1
fi

if [[ -z "${buildDir}" ]]; then
    buildDir="build_macos_package_${targetArch}"
fi
buildDir="$(projectPath "${buildDir}")"

if [[ -z "${packageDir}" ]]; then
    packageDir="${buildDir}/packages"
else
    packageDir="$(projectPath "${packageDir}")"
fi

if (( !packageOnly )); then
    buildArgs=(
        --arch "${targetArch}"
        --build-dir "${buildDir}"
        --build-type "${buildType}"
        --deployment-target "${deploymentTarget}"
        --linkage static
    )
    if [[ -n "${buildJobs}" ]]; then
        buildArgs+=(--jobs "${buildJobs}")
    fi
    if (( freshBuild )); then
        buildArgs+=(--fresh)
    fi

    MACOS_CODESIGN_IDENTITY="${codesignIdentity}" \
        "${scriptDir}/macos-build.sh" "${buildArgs[@]}"
fi

cpackConfig="${buildDir}/CPackConfig.cmake"
if [[ ! -f "${cpackConfig}" ]]; then
    printf "error: CPack configuration not found: %s\n" "${cpackConfig}" >&2
    printf "hint: omit --package-only to configure and build first\n" >&2
    exit 1
fi

mkdir -p "${packageDir}"
cpack --config "${cpackConfig}" \
    -G DragNDrop \
    -C "${buildType}" \
    -B "${packageDir}"

printf "macOS package output:\n"
find "${packageDir}" -maxdepth 1 -type f -name '*.dmg' -print | sort
