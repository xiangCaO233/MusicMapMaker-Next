#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/pull-lfs-for-build.sh [options]

Pull only the Git LFS objects required by one prebuilt toolchain.

Required options:
  --platform <windows|linux|macos>
  --arch <arch>
  --toolchain <name>
  --compiler-tag <tag>
  --build-type <type>
  --linkage <static|shared>

Optional:
  --include-tests    Include tests/data objects.
  --dry-run          Print the exact include list without pulling.
  -h, --help         Show this help.
EOF
}

requireValue() {
    if (( $# < 2 )); then
        printf "error: %s requires a value\n" "$1" >&2
        exit 1
    fi
}

validateSelector() {
    local selectorName="$1"
    local selectorValue="$2"

    if [[ "${selectorValue}" == "." || "${selectorValue}" == ".." || \
        ! "${selectorValue}" =~ ^[A-Za-z0-9._+-]+$ ]]; then
        printf "error: invalid %s selector: %s\n" "${selectorName}" "${selectorValue}" >&2
        exit 1
    fi
}

normalizeArchitecture() {
    case "$1" in
        x86_64 | amd64)
            printf "x86_64\n"
            ;;
        arm64 | aarch64)
            printf "arm64\n"
            ;;
        *)
            printf "%s\n" "$1"
            ;;
    esac
}

platform=""
architecture=""
toolchain=""
compilerTag=""
buildType=""
linkage=""
includeTests=0
dryRun=0

while (( $# > 0 )); do
    case "$1" in
        --platform)
            requireValue "$@"
            platform="$2"
            shift 2
            ;;
        --arch)
            requireValue "$@"
            architecture="$2"
            shift 2
            ;;
        --toolchain)
            requireValue "$@"
            toolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            requireValue "$@"
            compilerTag="$2"
            shift 2
            ;;
        --build-type)
            requireValue "$@"
            buildType="$2"
            shift 2
            ;;
        --linkage)
            requireValue "$@"
            linkage="$2"
            shift 2
            ;;
        --include-tests)
            includeTests=1
            shift
            ;;
        --dry-run)
            dryRun=1
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

for requiredValue in platform architecture toolchain compilerTag buildType linkage; do
    if [[ -z "${!requiredValue}" ]]; then
        printf "error: --%s is required\n" "${requiredValue}" >&2
        exit 1
    fi
done

case "${platform}" in
    windows | linux | macos) ;;
    *)
        printf "error: unsupported platform: %s\n" "${platform}" >&2
        exit 1
        ;;
esac

case "${linkage}" in
    static | shared) ;;
    *)
        printf "error: unsupported linkage: %s\n" "${linkage}" >&2
        exit 1
        ;;
esac

case "${buildType}" in
    Debug | Release | RelWithDebInfo | MinSizeRel) ;;
    *)
        printf "error: unsupported prebuilt build type: %s\n" "${buildType}" >&2
        exit 1
        ;;
esac

architecture="$(normalizeArchitecture "${architecture}")"
validateSelector platform "${platform}"
validateSelector architecture "${architecture}"
validateSelector toolchain "${toolchain}"
validateSelector compiler-tag "${compilerTag}"

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if ! git -C "${projectRoot}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf "error: project root is not a Git working tree: %s\n" "${projectRoot}" >&2
    exit 1
fi

prebuiltLibraryPattern() {
    local configName="$1"

    if [[ "${linkage}" == "shared" ]]; then
        printf "3rdpty/prebuilts/binaries/%s/*/libs/%s/%s/%s/shared/%s/**\n" \
            "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" "${configName}"
    else
        printf "3rdpty/prebuilts/binaries/%s/*/libs/%s/%s/%s/%s/**\n" \
            "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" "${configName}"
    fi
}

hasTrackedPrebuiltConfig() {
    local configName="$1"
    local libraryPattern
    local firstMatch
    libraryPattern="$(prebuiltLibraryPattern "${configName}")"
    firstMatch="$(git -C "${projectRoot}" ls-files -- ":(glob)${libraryPattern}" | sed -n '1p')"
    [[ -n "${firstMatch}" ]]
}

# 发布型配置优先使用 RelWithDebInfo，仅在仓库确实缺失时回退到 Release。
if [[ "${buildType}" == "Debug" ]]; then
    prebuiltConfig="Debug"
elif hasTrackedPrebuiltConfig RelWithDebInfo; then
    prebuiltConfig="RelWithDebInfo"
else
    prebuiltConfig="Release"
fi

if ! hasTrackedPrebuiltConfig "${prebuiltConfig}"; then
    printf "error: no tracked prebuilt objects for %s/%s/%s/%s/%s/%s\n" \
        "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" \
        "${linkage}" "${prebuiltConfig}" >&2
    exit 1
fi

includes=(
    "3rdpty/prebuilts/headers/**"
    "assets/**"
    "Modules/Main/src/logo.svg"
)

if (( includeTests )); then
    includes+=("tests/data/**")
fi

if [[ "${linkage}" == "shared" ]]; then
    includes+=(
        "3rdpty/prebuilts/binaries/${platform}/*/libs/${architecture}/${toolchain}/${compilerTag}/shared/${prebuiltConfig}/**"
        "3rdpty/prebuilts/binaries/${platform}/*/bin/${architecture}/${toolchain}/${compilerTag}/shared/${prebuiltConfig}/**"
    )
else
    includes+=(
        "3rdpty/prebuilts/binaries/${platform}/*/libs/${architecture}/${toolchain}/${compilerTag}/${prebuiltConfig}/**"
    )
fi

includeList="$(IFS=,; printf "%s" "${includes[*]}")"

if (( dryRun )); then
    printf "%s\n" "${includeList}"
    exit 0
fi

if ! git lfs version >/dev/null 2>&1; then
    printf "error: Git LFS is required for prebuilt mode\n" >&2
    exit 1
fi

printf "Git LFS: pulling %s/%s/%s/%s (%s, %s)\n" \
    "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" \
    "${linkage}" "${prebuiltConfig}"
git -C "${projectRoot}" lfs pull "--include=${includeList}" "--exclude="
