#!/usr/bin/env bash
set -euo pipefail

readonly DEFAULT_SOURCE_RELEASE_DIR="/home/xiang/MusicMapMaker-Next/release"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_root="$(cd -- "${script_dir}/../.." && pwd)"

source_release_dir="${DEFAULT_SOURCE_RELEASE_DIR}"
output_dir=""
release_tag=""
release_title=""
release_type="stable"
target_sha=""
repository="${GITHUB_REPOSITORY:-}"
package_only=0

usage() {
    cat <<'EOF'
Usage: scripts/ci/publish-github-release.sh [options]

Options:
  --source-release <dir>  Aggregated CI release directory.
  --output-dir <dir>      Empty directory used to stage GitHub release assets.
  --tag <tag>             Release tag. Default: v<APPVER> from CMakeLists.txt.
  --title <title>         Release title. Default: MusicMapMaker-Next <tag>.
  --type <type>           stable, prerelease, or draft. Default: stable.
  --target-sha <sha>      Commit to tag. Default: current repository HEAD.
  --repository <owner/repo>
                          GitHub repository. Default: GITHUB_REPOSITORY.
  --package-only          Validate and package without publishing to GitHub.
  -h, --help              Show this help.
EOF
}

log() {
    printf '[github-release] %s\n' "$*"
}

fail() {
    printf '[github-release] error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    local command_name="$1"
    command -v "${command_name}" >/dev/null 2>&1 ||
        fail "required command not found: ${command_name}"
}

while (( $# > 0 )); do
    case "$1" in
        --source-release)
            (( $# >= 2 )) || fail "--source-release requires a directory"
            source_release_dir="$2"
            shift 2
            ;;
        --output-dir)
            (( $# >= 2 )) || fail "--output-dir requires a directory"
            output_dir="$2"
            shift 2
            ;;
        --tag)
            (( $# >= 2 )) || fail "--tag requires a value"
            release_tag="$2"
            shift 2
            ;;
        --title)
            (( $# >= 2 )) || fail "--title requires a value"
            release_title="$2"
            shift 2
            ;;
        --type)
            (( $# >= 2 )) || fail "--type requires a value"
            release_type="$2"
            shift 2
            ;;
        --target-sha)
            (( $# >= 2 )) || fail "--target-sha requires a commit SHA"
            target_sha="$2"
            shift 2
            ;;
        --repository)
            (( $# >= 2 )) || fail "--repository requires owner/repo"
            repository="$2"
            shift 2
            ;;
        --package-only)
            package_only=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

require_command git
require_command sha256sum
require_command tar
require_command unzip
require_command zip

[[ -d "${source_root}/.git" ]] ||
    fail "script is not running from a MusicMapMaker-Next checkout"
[[ -d "${source_release_dir}" ]] ||
    fail "aggregated CI release directory not found: ${source_release_dir}"
[[ -n "${output_dir}" ]] || fail "--output-dir is required"

source_release_dir="$(cd -- "${source_release_dir}" && pwd)"
if [[ -e "${output_dir}" ]]; then
    output_dir="$(cd -- "${output_dir}" && pwd)"
    [[ -z "$(find "${output_dir}" -mindepth 1 -print -quit)" ]] ||
        fail "output directory must be empty: ${output_dir}"
else
    mkdir -p -- "${output_dir}"
    output_dir="$(cd -- "${output_dir}" && pwd)"
fi

read_app_version() {
    local app_version=""
    local line
    while IFS= read -r line; do
        if [[ "${line}" =~ ^[[:space:]]*set[[:space:]]*\([[:space:]]*APPVER[[:space:]]+\"([^\"]+)\" ]]; then
            app_version="${BASH_REMATCH[1]}"
            break
        fi
    done < "${source_root}/CMakeLists.txt"
    [[ -n "${app_version}" ]] || fail "failed to read APPVER from CMakeLists.txt"
    printf '%s\n' "${app_version}"
}

if [[ -z "${release_tag}" ]]; then
    release_tag="$(read_app_version)"
    if [[ "${release_tag}" != v* ]]; then
        release_tag="v${release_tag}"
    fi
fi

[[ "${release_tag}" =~ ^v?[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
    fail "release tag contains unsupported characters: ${release_tag}"

case "${release_type}" in
    stable | prerelease | draft) ;;
    *) fail "--type must be stable, prerelease, or draft" ;;
esac

if [[ -z "${target_sha}" ]]; then
    target_sha="$(git -C "${source_root}" rev-parse HEAD)"
fi
target_sha="$(git -C "${source_root}" rev-parse --verify "${target_sha}^{commit}")"

local_head="$(git -C "${source_root}" rev-parse HEAD)"
[[ "${local_head}" == "${target_sha}" ]] ||
    fail "checkout HEAD ${local_head} does not match release target ${target_sha}"

if [[ -z "${release_title}" ]]; then
    release_title="MusicMapMaker-Next ${release_tag}"
fi

version_label="${release_tag#v}"
[[ -n "${version_label}" ]] || fail "release tag does not contain a version"

if (( !package_only )); then
    git -C "${source_root}" diff --quiet --ignore-submodules -- ||
        fail "checkout contains unstaged tracked changes"
    git -C "${source_root}" diff --cached --quiet --ignore-submodules -- ||
        fail "checkout contains staged changes"

    require_command gh
    [[ "${repository}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
        fail "--repository must use owner/repo form"
    [[ -n "${GH_TOKEN:-}" ]] || fail "GH_TOKEN is required to publish a release"

    remote_target="$(gh api "repos/${repository}/commits/${target_sha}" --jq .sha)"
    [[ "${remote_target}" == "${target_sha}" ]] ||
        fail "target commit is not available in ${repository}: ${target_sha}"

    if gh release view "${release_tag}" --repo "${repository}" >/dev/null 2>&1; then
        fail "GitHub Release already exists: ${release_tag}"
    fi
    if gh api "repos/${repository}/git/ref/tags/${release_tag}" >/dev/null 2>&1; then
        fail "Git tag already exists: ${release_tag}"
    fi
fi

declare -a windows_variants=(
    "windows-msvc-clang"
    "windows-mingw-gcc"
    "windows-mingw-clang"
)
declare -a linux_variants=(
    "linux-gcc14"
    "linux-clang19"
)

require_file() {
    local file_path="$1"
    [[ -s "${file_path}" ]] || fail "required release file not found or empty: ${file_path}"
}

require_directory() {
    local directory_path="$1"
    [[ -d "${directory_path}" ]] || fail "required release directory not found: ${directory_path}"
    [[ -n "$(find "${directory_path}" -type f -print -quit)" ]] ||
        fail "required release directory is empty: ${directory_path}"
}

for variant in "${windows_variants[@]}"; do
    require_directory "${source_release_dir}/${variant}"
    require_file "${source_release_dir}/${variant}/MusicMapMaker-Next.exe"
done

for variant in "${linux_variants[@]}"; do
    require_directory "${source_release_dir}/${variant}"
    require_file "${source_release_dir}/${variant}/MusicMapMaker-Next"
done

require_directory "${source_release_dir}/assets"
require_directory "${source_release_dir}/macos-arm64"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Next.dmg"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Next.app.zip"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Updater"

package_zip_directory() {
    local source_directory="$1"
    local archive_path="$2"
    log "package ${archive_path##*/}"
    (
        cd -- "${source_directory}"
        zip -9 -q -r "${archive_path}" .
    )
}

package_tar_xz_directory() {
    local source_directory="$1"
    local archive_path="$2"
    log "package ${archive_path##*/}"
    tar -C "${source_directory}" -cJf "${archive_path}" .
}

for variant in "${windows_variants[@]}"; do
    package_zip_directory \
        "${source_release_dir}/${variant}" \
        "${output_dir}/MusicMapMaker-Next-${version_label}-${variant}-x86_64.zip"
done

for variant in "${linux_variants[@]}"; do
    package_tar_xz_directory \
        "${source_release_dir}/${variant}" \
        "${output_dir}/MusicMapMaker-Next-${version_label}-${variant}-x86_64.tar.xz"
done

# assets.zip 保留 assets 顶层目录，解压后可以直接作为程序资源目录使用。
(
    cd -- "${source_release_dir}"
    zip -9 -q -r \
        "${output_dir}/MusicMapMaker-Next-${version_label}-assets.zip" \
        assets
)

cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Next.dmg" \
    "${output_dir}/MusicMapMaker-Next-${version_label}-macos-arm64.dmg"
cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Next.app.zip" \
    "${output_dir}/MusicMapMaker-Next-${version_label}-macos-arm64.app.zip"
cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Updater" \
    "${output_dir}/MusicMapMaker-Updater-${version_label}-macos-arm64"

{
    printf 'tag=%s\n' "${release_tag}"
    printf 'target_sha=%s\n' "${target_sha}"
    printf 'repository=%s\n' "${repository:-not-set}"
    printf 'generated_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${output_dir}/RELEASE_INFO.txt"

for archive_path in "${output_dir}"/*.zip; do
    unzip -tq "${archive_path}" >/dev/null
done
for archive_path in "${output_dir}"/*.tar.xz; do
    tar -tJf "${archive_path}" >/dev/null
done

(
    cd -- "${output_dir}"
    find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%f\n' |
        sort |
        xargs -r sha256sum > SHA256SUMS
    sha256sum -c SHA256SUMS
)

log "release assets are ready"
find "${output_dir}" -maxdepth 1 -type f -printf '%f\t%s bytes\n' | sort

if (( package_only )); then
    exit 0
fi

mapfile -t release_assets < <(
    find "${output_dir}" -maxdepth 1 -type f -print | sort
)
(( ${#release_assets[@]} > 0 )) || fail "no release assets were generated"

declare -a gh_release_args=(
    release create
    "${release_tag}"
    "${release_assets[@]}"
    --repo "${repository}"
    --target "${target_sha}"
    --title "${release_title}"
    --generate-notes
)
if [[ "${release_type}" == "prerelease" ]]; then
    gh_release_args+=(--prerelease)
elif [[ "${release_type}" == "draft" ]]; then
    gh_release_args+=(--draft)
fi

log "create GitHub Release ${release_tag} at ${target_sha}"
gh "${gh_release_args[@]}"

release_url="$(
    gh release view "${release_tag}" \
        --repo "${repository}" \
        --json url \
        --jq .url
)"
log "published ${release_url}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf 'release_url=%s\n' "${release_url}" >> "${GITHUB_OUTPUT}"
    printf 'release_tag=%s\n' "${release_tag}" >> "${GITHUB_OUTPUT}"
fi
