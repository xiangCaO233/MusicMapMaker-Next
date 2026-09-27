#!/usr/bin/env bash
# 汇总多平台 CI 产物，生成校验过的 GitHub Release 资产并可选择发布。
# 打包阶段只读取聚合目录，所有新文件写入调用方提供的空输出目录。
# 正式发布前验证工作树、目标提交、远端提交、标签和 Release 均符合前置条件。
# --package-only 保留完整打包与校验，但不要求 GitHub 凭据或修改远端。
# 每个平台采用固定归档格式和命名，供用户与自动更新流程稳定消费。
set -euo pipefail

# 默认路径对应自托管 Runner 的聚合发布目录。
readonly DEFAULT_SOURCE_RELEASE_DIR="/home/xiang/MusicMapMaker-Next/release"

# 通过脚本自身位置定位当前源码仓库。
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_root="$(cd -- "${script_dir}/../.." && pwd)"

source_release_dir="${DEFAULT_SOURCE_RELEASE_DIR}"
# 输出目录必须由调用方明确提供并保持为空。
output_dir=""
release_tag=""
release_title=""
release_type="stable"
target_sha=""
# GitHub Actions 环境可直接提供 owner/repo。
repository="${GITHUB_REPOSITORY:-}"
package_only=0

# 输出发布参数契约，不读取仓库或产物目录。
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

# 写入带稳定前缀的普通进度日志。
log() {
    printf '[github-release] %s\n' "$*"
}

# 输出统一错误并立即终止发布事务。
fail() {
    printf '[github-release] error: %s\n' "$*" >&2
    exit 1
}

# 验证宿主命令存在，脚本不自动安装发布工具。
require_command() {
    local command_name="$1"
    command -v "${command_name}" >/dev/null 2>&1 ||
        fail "required command not found: ${command_name}"
}

# 在任何打包或远端访问前完整解析选项。
while (( $# > 0 )); do
    case "$1" in
        --source-release)
            # 聚合目录可由 CI artifact 下载位置覆盖。
            (( $# >= 2 )) || fail "--source-release requires a directory"
            source_release_dir="$2"
            shift 2
            ;;
        --output-dir)
            # 输出目录必须专用于当前发布，避免混入旧资产。
            (( $# >= 2 )) || fail "--output-dir requires a directory"
            output_dir="$2"
            shift 2
            ;;
        --tag)
            # 显式标签覆盖 CMake APPVER 推导。
            (( $# >= 2 )) || fail "--tag requires a value"
            release_tag="$2"
            shift 2
            ;;
        --title)
            # 标题只影响 GitHub 展示，不影响资产文件名。
            (( $# >= 2 )) || fail "--title requires a value"
            release_title="$2"
            shift 2
            ;;
        --type)
            # 发布类型稍后限制为三个受支持状态。
            (( $# >= 2 )) || fail "--type requires a value"
            release_type="$2"
            shift 2
            ;;
        --target-sha)
            # 目标必须最终解析为本地 commit 对象。
            (( $# >= 2 )) || fail "--target-sha requires a commit SHA"
            target_sha="$2"
            shift 2
            ;;
        --repository)
            # 仓库采用 GitHub owner/repo 格式。
            (( $# >= 2 )) || fail "--repository requires owner/repo"
            repository="$2"
            shift 2
            ;;
        --package-only)
            # 本地模式跳过所有 gh 调用和工作树洁净要求。
            package_only=1
            shift
            ;;
        -h | --help)
            # 帮助成功退出，不验证目录和工具。
            usage
            exit 0
            ;;
        *)
            # 未知参数通过统一 fail 路径处理。
            fail "unknown option: $1"
            ;;
    esac
done

require_command git
# 压缩、解压验证和哈希工具都是打包阶段硬依赖。
require_command sha256sum
require_command tar
require_command unzip
require_command zip

[[ -d "${source_root}/.git" ]] ||
    # 防止从复制出的孤立脚本误发布其他目录内容。
    fail "script is not running from a MusicMapMaker-Next checkout"
[[ -d "${source_release_dir}" ]] ||
    fail "aggregated CI release directory not found: ${source_release_dir}"
[[ -n "${output_dir}" ]] || fail "--output-dir is required"

# 规范化源目录以稳定后续相对关系和日志。
source_release_dir="$(cd -- "${source_release_dir}" && pwd)"
if [[ -e "${output_dir}" ]]; then
    # 现有输出目录必须为空，避免哈希清单包含陈旧资产。
    output_dir="$(cd -- "${output_dir}" && pwd)"
    [[ -z "$(find "${output_dir}" -mindepth 1 -print -quit)" ]] ||
        fail "output directory must be empty: ${output_dir}"
else
    # 不存在时创建后再转换为绝对路径。
    mkdir -p -- "${output_dir}"
    output_dir="$(cd -- "${output_dir}" && pwd)"
fi

# 从顶层 CMake set(APPVER ...) 声明读取版本文本。
read_app_version() {
    local app_version=""
    local line
    while IFS= read -r line; do
        # 只匹配明确变量名和双引号值，不执行 CMake。
        if [[ "${line}" =~ ^[[:space:]]*set[[:space:]]*\([[:space:]]*APPVER[[:space:]]+\"([^\"]+)\" ]]; then
            app_version="${BASH_REMATCH[1]}"
            # 首个规范声明即为项目版本来源。
            break
        fi
    done < "${source_root}/CMakeLists.txt"
    [[ -n "${app_version}" ]] || fail "failed to read APPVER from CMakeLists.txt"
    printf '%s\n' "${app_version}"
}

if [[ -z "${release_tag}" ]]; then
    # 默认标签由当前 checkout 的 APPVER 派生。
    release_tag="$(read_app_version)"
    if [[ "${release_tag}" != v* ]]; then
        # Git 标签统一包含 v 前缀。
        release_tag="v${release_tag}"
    fi
fi

[[ "${release_tag}" =~ ^v?[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
    # 限制字符集以保证文件名、API 路径和 Git ref 安全。
    fail "release tag contains unsupported characters: ${release_tag}"

case "${release_type}" in
    stable | prerelease | draft)
        # 三个值直接映射 GitHub Release 状态。
        ;;
    *) fail "--type must be stable, prerelease, or draft" ;;
esac

if [[ -z "${target_sha}" ]]; then
    # 默认发布当前 checkout 精确 HEAD。
    target_sha="$(git -C "${source_root}" rev-parse HEAD)"
fi
target_sha="$(git -C "${source_root}" rev-parse --verify "${target_sha}^{commit}")"

# checkout 与目标必须一致，确保打包版本和远端 tag 指向同一提交。
local_head="$(git -C "${source_root}" rev-parse HEAD)"
[[ "${local_head}" == "${target_sha}" ]] ||
    fail "checkout HEAD ${local_head} does not match release target ${target_sha}"

if [[ -z "${release_title}" ]]; then
    # 默认标题保留项目名和完整标签。
    release_title="MusicMapMaker-Next ${release_tag}"
fi

version_label="${release_tag#v}"
# 文件名使用不带前缀的版本段。
[[ -n "${version_label}" ]] || fail "release tag does not contain a version"

if (( !package_only )); then
    # 正式发布要求 tracked 工作树和暂存区均保持洁净。
    git -C "${source_root}" diff --quiet --ignore-submodules -- ||
        fail "checkout contains unstaged tracked changes"
    git -C "${source_root}" diff --cached --quiet --ignore-submodules -- ||
        fail "checkout contains staged changes"

    require_command gh
    # 远端仓库名先做本地格式校验，再调用 API。
    [[ "${repository}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
        fail "--repository must use owner/repo form"
    [[ -n "${GH_TOKEN:-}" ]] || fail "GH_TOKEN is required to publish a release"

    # 目标提交必须已推送到指定 GitHub 仓库。
    remote_target="$(gh api "repos/${repository}/commits/${target_sha}" --jq .sha)"
    [[ "${remote_target}" == "${target_sha}" ]] ||
        fail "target commit is not available in ${repository}: ${target_sha}"

    if gh release view "${release_tag}" --repo "${repository}" >/dev/null 2>&1; then
        # 不覆盖现有 Release，发布重试需先人工核对远端状态。
        fail "GitHub Release already exists: ${release_tag}"
    fi
    if gh api "repos/${repository}/git/ref/tags/${release_tag}" >/dev/null 2>&1; then
        # 即使尚无 Release，现有同名 tag 也禁止复用。
        fail "Git tag already exists: ${release_tag}"
    fi
fi

# Windows 三套工具链产物分别打包，保留用户选择空间。
declare -a windows_variants=(
    "windows-msvc-clang"
    "windows-mingw-gcc"
    "windows-mingw-clang"
)
# Linux 两套编译器产物分别打包。
declare -a linux_variants=(
    "linux-gcc14"
    "linux-clang19"
)

# 要求关键发布文件存在且非空。
require_file() {
    local file_path="$1"
    [[ -s "${file_path}" ]] || fail "required release file not found or empty: ${file_path}"
}

# 要求目录存在并至少包含一个普通文件。
require_directory() {
    local directory_path="$1"
    [[ -d "${directory_path}" ]] || fail "required release directory not found: ${directory_path}"
    [[ -n "$(find "${directory_path}" -type f -print -quit)" ]] ||
        # 空目录不能形成有效平台资产。
        fail "required release directory is empty: ${directory_path}"
}

# 每个 Windows 变体必须包含主程序。
for variant in "${windows_variants[@]}"; do
    require_directory "${source_release_dir}/${variant}"
    require_file "${source_release_dir}/${variant}/MusicMapMaker-Next.exe"
done

# 每个 Linux 变体必须包含主程序。
for variant in "${linux_variants[@]}"; do
    require_directory "${source_release_dir}/${variant}"
    require_file "${source_release_dir}/${variant}/MusicMapMaker-Next"
done

# 通用 assets 与 macOS 三件套是完整发布的强制组成。
require_directory "${source_release_dir}/assets"
require_directory "${source_release_dir}/macos-arm64"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Next.dmg"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Next.app.zip"
require_file "${source_release_dir}/macos-arm64/MusicMapMaker-Updater"

# 在源目录内递归创建最高压缩级别 ZIP。
package_zip_directory() {
    local source_directory="$1"
    local archive_path="$2"
    log "package ${archive_path##*/}"
    (
        # 子 shell 隔离工作目录变化。
        cd -- "${source_directory}"
        zip -9 -q -r "${archive_path}" .
    )
}

# 使用 tar.xz 保存 Linux 文件权限和目录结构。
package_tar_xz_directory() {
    local source_directory="$1"
    local archive_path="$2"
    log "package ${archive_path##*/}"
    tar -C "${source_directory}" -cJf "${archive_path}" .
}

# Windows 每个工具链变体生成独立 ZIP。
for variant in "${windows_variants[@]}"; do
    package_zip_directory \
        "${source_release_dir}/${variant}" \
        "${output_dir}/MusicMapMaker-Next-${version_label}-${variant}-x86_64.zip"
done

# Linux 每个编译器变体生成独立 tar.xz。
for variant in "${linux_variants[@]}"; do
    package_tar_xz_directory \
        "${source_release_dir}/${variant}" \
        "${output_dir}/MusicMapMaker-Next-${version_label}-${variant}-x86_64.tar.xz"
done

# assets.zip 保留 assets 顶层目录，解压后可以直接作为程序资源目录使用。
# 在聚合目录中打包，确保归档根包含 assets 名称。
(
    cd -- "${source_release_dir}"
    zip -9 -q -r \
        "${output_dir}/MusicMapMaker-Next-${version_label}-assets.zip" \
        assets
)

# macOS 已打包产物只做版本化重命名复制，不重新压缩。
cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Next.dmg" \
    "${output_dir}/MusicMapMaker-Next-${version_label}-macos-arm64.dmg"
cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Next.app.zip" \
    "${output_dir}/MusicMapMaker-Next-${version_label}-macos-arm64.app.zip"
cp -- \
    "${source_release_dir}/macos-arm64/MusicMapMaker-Updater" \
    "${output_dir}/MusicMapMaker-Updater-${version_label}-macos-arm64"

# 写入机器可读的发布来源元数据。
{
    printf 'tag=%s\n' "${release_tag}"
    printf 'target_sha=%s\n' "${target_sha}"
    printf 'repository=%s\n' "${repository:-not-set}"
    printf 'generated_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${output_dir}/RELEASE_INFO.txt"

# 所有 ZIP 在发布前执行完整目录和 CRC 测试。
for archive_path in "${output_dir}"/*.zip; do
    unzip -tq "${archive_path}" >/dev/null
done
# 所有 tar.xz 在发布前完整遍历目录表。
for archive_path in "${output_dir}"/*.tar.xz; do
    tar -tJf "${archive_path}" >/dev/null
done

(
    # 哈希清单不包含自身，并按文件名排序保证稳定输出。
    cd -- "${output_dir}"
    find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%f\n' |
        sort |
        xargs -r sha256sum > SHA256SUMS
    sha256sum -c SHA256SUMS
)

# 到此才宣布本地发布资产准备完成。
log "release assets are ready"
find "${output_dir}" -maxdepth 1 -type f -printf '%f\t%s bytes\n' | sort

if (( package_only )); then
    # 本地验证模式不解析远端发布参数。
    exit 0
fi

# 所有生成文件都作为 Release assets 上传，包括校验和与元数据。
mapfile -t release_assets < <(
    find "${output_dir}" -maxdepth 1 -type f -print | sort
)
(( ${#release_assets[@]} > 0 )) || fail "no release assets were generated"

# 使用数组保留标题、路径和其他参数中的空格。
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
    # 预发布映射到 gh 的 prerelease 标志。
    gh_release_args+=(--prerelease)
elif [[ "${release_type}" == "draft" ]]; then
    # 草稿映射到 gh 的 draft 标志。
    gh_release_args+=(--draft)
fi

log "create GitHub Release ${release_tag} at ${target_sha}"
# 单次 gh 调用创建 tag、Release 并上传完整资产集合。
gh "${gh_release_args[@]}"

# 创建成功后从远端读取规范 Release URL。
release_url="$(
    gh release view "${release_tag}" \
        --repo "${repository}" \
        --json url \
        --jq .url
)"
log "published ${release_url}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    # GitHub Actions 中向后续步骤导出 URL 和标签。
    printf 'release_url=%s\n' "${release_url}" >> "${GITHUB_OUTPUT}"
    printf 'release_tag=%s\n' "${release_tag}" >> "${GITHUB_OUTPUT}"
fi

# 维护约束：聚合目录必须来自同一目标提交的完整 CI 运行。
# 维护约束：不同平台变体不得在聚合时互相覆盖文件。
# 维护约束：输出目录必须专用于当前版本且初始为空。
# 维护约束：package-only 可以在无 GH_TOKEN 环境中复现资产。
# 维护约束：正式发布不得在 tracked 工作树脏时继续。
# 维护约束：未跟踪文件不参与源码一致性判断或发布资产收集。
# 维护约束：目标提交必须是本地可解析的 commit 对象。
# 维护约束：目标提交必须与当前 checkout HEAD 完全一致。
# 维护约束：远端仓库必须已包含目标提交对象。
# 维护约束：已有 tag 或 Release 都不得被脚本覆盖。
# 维护约束：版本标签字符集必须兼容 Git ref 和文件名。
# 维护约束：资产版本段只移除单个开头 v 前缀。
# 维护约束：Windows ZIP 应保留可直接解压运行的目录内容。
# 维护约束：Linux tar.xz 必须保留主程序可执行权限。
# 维护约束：assets.zip 顶层必须继续包含 assets 目录。
# 维护约束：macOS DMG 与 App ZIP 必须来自同一次应用构建。
# 维护约束：macOS 更新器必须作为独立可执行资产发布。
# 维护约束：每个必需文件都必须非空，不能只检查路径存在。
# 维护约束：每个必需目录都必须至少包含一个普通文件。
# 维护约束：归档验证必须在 SHA256SUMS 生成之前完成。
# 维护约束：哈希清单必须覆盖所有最终上传文件但排除自身。
# 维护约束：哈希生成和校验必须在同一个输出目录执行。
# 维护约束：RELEASE_INFO 的时间使用 UTC ISO 格式。
# 维护约束：RELEASE_INFO 的 target_sha 必须与 GitHub tag 目标一致。
# 维护约束：发布标题不参与版本或资产名称推导。
# 维护约束：stable 发布不附加 prerelease 或 draft 标志。
# 维护约束：gh 参数保持数组形式，禁止字符串拼接后 eval。
# 维护约束：Release 创建失败时不得伪造或导出 URL。
# 维护约束：GITHUB_OUTPUT 只在环境提供路径时追加写入。
# 维护约束：脚本不执行 git push，tag 创建由 gh release 完成。
# 维护约束：脚本不删除或回滚已有远端发布对象。
# 维护约束：发布重试前必须人工核对是否已产生部分远端状态。
# 维护约束：新增平台变体需同步存在性检查、归档循环和命名规则。
# 维护约束：新增资产类型需加入完整性验证和哈希清单。
# 维护约束：压缩工具升级后仍需验证跨平台解压兼容性。
# 维护约束：产物上传顺序由排序后的绝对路径稳定决定。
# 维护约束：成功退出表示本地校验完成且可选远端创建成功。
# 维护约束：默认源目录仅是 Runner 约定，显式参数应优先。
# 维护约束：输出目录解析后不得与聚合源目录相同。
# 维护约束：调用方应使用全新临时目录满足空目录前置条件。
# 维护约束：文件名中的 variant 必须与 CI artifact 目录名一致。
# 维护约束：Windows 与 Linux 架构后缀当前固定 x86_64。
# 维护约束：macOS 公开资产当前固定 arm64，不隐式发布 x86_64。
# 维护约束：APPVER 读取失败时不得从 Git tag 历史猜测版本。
# 维护约束：显式 tag 与 APPVER 一致性由发布调用方审核。
# 维护约束：压缩内容使用目录内的点路径，不嵌入绝对源路径。
# 维护约束：归档 helper 在子 shell 中切换目录后必须自动恢复。
# 维护约束：macOS 文件复制必须保留原始可执行权限。
# 维护约束：SHA256SUMS 每一行使用标准 sha256sum 输出格式。
# 维护约束：发布资产列表不得包含子目录或临时文件。
# 维护约束：Release URL 查询必须针对刚创建的同一 repository。
# 维护约束：远端 API 校验和创建均必须使用显式 repository。
