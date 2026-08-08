#!/usr/bin/env bash

set -o pipefail

loadUserBashrc() {
    local bashrcPath="${HOME:-}/.bashrc"
    if [[ -n "${HOME:-}" && -f "${bashrcPath}" ]]; then
        # shellcheck source=/dev/null
        source "${bashrcPath}"
    fi
}

loadUserBashrc
set -euo pipefail

readonly DEFAULT_SOURCE_RELEASE_DIR="/home/xiang/MusicMapMaker-Next/release"
readonly DEFAULT_WEBSITE_DIR="/home/xiang/mmm-website"
readonly DEFAULT_DEEPSEEK_MODEL="deepseek-v4-flash"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_root="$(cd -- "${script_dir}/../.." && pwd)"

source_release_dir="${DEFAULT_SOURCE_RELEASE_DIR}"
website_dir="${DEFAULT_WEBSITE_DIR}"
website_branch="${MMM_WEBSITE_BRANCH:-}"
deepseek_model="${DEEPSEEK_MODEL:-${DEFAULT_DEEPSEEK_MODEL}}"

# 这里使用前插 PATH，按低优先级到高优先级排列，确保 node24 优先于 node20。
for npm_bin_dir in \
    "/home/xiang/actions-runner/externals/node20/bin" \
    "/home/xiang/actions-runner/externals/node24/bin"; do
    if [[ ":${PATH}:" != *":${npm_bin_dir}:"* && -x "${npm_bin_dir}/npm" ]]; then
        PATH="${npm_bin_dir}:${PATH}"
    fi
done
export PATH

usage() {
    cat <<'EOF'
Usage: scripts/ci/publish-website-release.sh [options]

Options:
  --source-release <dir>  CI 产物目录。默认: /home/xiang/MusicMapMaker-Next/release
  --website-dir <dir>     mmm-website 仓库目录。默认: /home/xiang/mmm-website
  --website-branch <name> 网站仓库发布分支。默认: 当前分支
  --help                  显示帮助

Environment:
  DEEPSEEK_APIKEY / DEEPSEEK_API_KEY  DeepSeek API key，优先使用 DEEPSEEK_APIKEY。
  DEEPSEEK_MODEL                     DeepSeek 模型名，默认 deepseek-v4-flash。
  DEEPSEEK_MAX_TOKENS                changelog 最大输出 token，默认 12000。
  DEEPSEEK_THINKING                  是否启用思考模式，默认 enabled。
  DEEPSEEK_REASONING_EFFORT          思考强度，默认 low。
  MMM_RELEASE_WINDOWS_DIR            网站公开 Windows 下载优先使用的产物目录，默认 windows-msvc-clang。
  MMM_RELEASE_LINUX_DIR              网站公开 Linux 下载优先使用的产物目录，默认 linux-gcc14。
  MMM_RELEASE_MACOS_DIR              网站公开 macOS 下载使用的产物目录，默认 macos-arm64。
  MMM_VERSION_UPDATE_COMMIT          手动指定“上一次版本更新提交”，默认从 CMakeLists.txt 的 APPVER 变更记录推断。
EOF
}

log() {
    printf '[website-release] %s\n' "$*"
}

fail() {
    printf '[website-release] error: %s\n' "$*" >&2
    exit 1
}

run() {
    printf '[website-release] run:'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

require_command() {
    local command_name="$1"
    command -v "${command_name}" > /dev/null 2>&1 || fail "找不到命令: ${command_name}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-release)
            [[ $# -ge 2 ]] || fail "--source-release 需要目录参数"
            source_release_dir="$2"
            shift 2
            ;;
        --website-dir)
            [[ $# -ge 2 ]] || fail "--website-dir 需要目录参数"
            website_dir="$2"
            shift 2
            ;;
        --website-branch)
            [[ $# -ge 2 ]] || fail "--website-branch 需要分支名"
            website_branch="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            fail "未知参数: $1"
            ;;
    esac
done

require_command git
require_command npm
require_command python3

[[ -d "${source_root}/.git" ]] || fail "当前脚本不在 MusicMapMaker-Next 仓库中"
[[ -d "${source_release_dir}" ]] || fail "CI 产物目录不存在: ${source_release_dir}"
[[ -d "${website_dir}/.git" ]] || fail "网站仓库目录不存在或不是 Git 仓库: ${website_dir}"

read_app_version_from_text() {
    local app_version
    app_version=""
    while IFS= read -r line; do
        if [[ "${line}" =~ ^[[:space:]]*set[[:space:]]*\([[:space:]]*APPVER[[:space:]]+\"([^\"]+)\" ]]; then
            app_version="${BASH_REMATCH[1]}"
            break
        fi
    done
    printf '%s\n' "${app_version}"
}

read_release_version() {
    local app_version
    app_version="$(read_app_version_from_text < "${source_root}/CMakeLists.txt")"
    [[ -n "${app_version}" ]] || fail "无法从 CMakeLists.txt 读取 APPVER"

    if [[ "${app_version}" == v* ]]; then
        printf '%s\n' "${app_version}"
    else
        printf 'v%s\n' "${app_version}"
    fi
}

find_version_update_commit() {
    local release_version="$1"
    local normalized_release_version
    local commit_version
    local version_update_commits
    local commit_index

    if [[ -n "${MMM_VERSION_UPDATE_COMMIT:-}" ]]; then
        git -C "${source_root}" rev-parse --verify "${MMM_VERSION_UPDATE_COMMIT}^{commit}"
        return
    fi

    normalized_release_version="${release_version#v}"
    mapfile -t version_update_commits < <(git -C "${source_root}" log --format=%H -G 'APPVER' -- CMakeLists.txt)
    for commit_index in "${!version_update_commits[@]}"; do
        commit_version="$(read_app_version_from_text < <(git -C "${source_root}" show "${version_update_commits[commit_index]}:CMakeLists.txt"))"
        if [[ "${commit_version#v}" == "${normalized_release_version}" ]]; then
            if (( commit_index + 1 >= ${#version_update_commits[@]} )); then
                return
            fi
            printf '%s\n' "${version_update_commits[commit_index + 1]}"
            return
        fi
    done

    printf '%s\n' "${version_update_commits[0]:-}"
}

commit_range_for_base() {
    local base_commit="$1"
    if git -C "${source_root}" rev-parse --verify "${base_commit}^" > /dev/null 2>&1; then
        printf '%s^..HEAD\n' "${base_commit}"
    else
        printf '%s\n' "${base_commit}"
    fi
}

write_branch_context() {
    local context_file="$1"
    local release_version="$2"
    local base_commit="$3"
    local commit_range="$4"
    local commit_count="$5"
    local upstream
    local origin_url

    upstream="$(git -C "${source_root}" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    origin_url="$(git -C "${source_root}" config --get remote.origin.url || true)"

    {
        printf '# MusicMapMaker-Next 分支发布上下文\n\n'
        printf -- '- 生成时间: %s\n' "$(date -Iseconds)"
        printf -- '- 仓库: %s\n' "${origin_url:-未设置}"
        printf -- '- 分支: %s\n' "$(git -C "${source_root}" branch --show-current)"
        printf -- '- 上游: %s\n' "${upstream:-未设置}"
        printf -- '- HEAD: %s\n' "$(git -C "${source_root}" rev-parse HEAD)"
        printf -- '- 发布版本: %s\n' "${release_version}"
        printf -- '- 版本声明: CMakeLists.txt 中的 APPVER\n'
        printf -- '- 上一次版本更新提交: %s\n' "${base_commit}"
        printf -- '- 提交范围: %s\n' "${commit_range}"
        printf -- '- 提交数量: %s\n\n' "${commit_count}"

        printf '## 提交列表\n\n'
        git -C "${source_root}" log --reverse --date=iso-strict \
            --format='- %H | %ad | %an <%ae> | %s%d' "${commit_range}"

        printf '\n## 提交详细信息\n'
        while IFS= read -r commit_hash; do
            printf '\n### %s\n\n' "${commit_hash}"
            git -C "${source_root}" show --no-ext-diff --find-renames --find-copies \
                --stat --summary --name-status --date=iso-strict \
                --format='commit %H%nAuthor: %an <%ae>%nAuthorDate: %aI%nCommit: %cn <%ce>%nCommitDate: %cI%nSubject: %s%n%nBody:%n%b' \
                "${commit_hash}"
        done < <(git -C "${source_root}" rev-list --reverse "${commit_range}")
    } > "${context_file}"
}

generate_changelog() {
    local context_file="$1"
    local changelog_file="$2"
    local release_version="$3"
    local api_key

    api_key="${DEEPSEEK_APIKEY:-${DEEPSEEK_API_KEY:-}}"
    [[ -n "${api_key}" ]] || fail "未设置 DEEPSEEK_APIKEY 或 DEEPSEEK_API_KEY"

    log "使用 DeepSeek 生成 ${changelog_file}"
    DEEPSEEK_EFFECTIVE_API_KEY="${api_key}" \
    DEEPSEEK_EFFECTIVE_MODEL="${deepseek_model}" \
    python3 - "${context_file}" "${changelog_file}" "${release_version}" <<'PY'
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

context_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
release_version = sys.argv[3]

api_key = os.environ["DEEPSEEK_EFFECTIVE_API_KEY"]
model = os.environ["DEEPSEEK_EFFECTIVE_MODEL"]
max_tokens = int(os.environ.get("DEEPSEEK_MAX_TOKENS", "12000"))
temperature = float(os.environ.get("DEEPSEEK_TEMPERATURE", "0.2"))
thinking_type = os.environ.get("DEEPSEEK_THINKING", "enabled")
reasoning_effort = os.environ.get("DEEPSEEK_REASONING_EFFORT", "low")
timeout = int(os.environ.get("DEEPSEEK_TIMEOUT", "300"))

context = context_path.read_text(encoding="utf-8")
system_prompt = (
    "你是 MusicMapMaker-Next 项目的发布维护者。"
    "请只依据用户提供的 Git 分支发布上下文生成面向用户的中文 Markdown changelog，"
    "不要臆造上下文中没有的信息。"
    "不要逐提交展开长篇推理，只做必要的归类合并，尽快直接输出最终 Markdown。"
)
user_prompt = f"""请为 {release_version} 生成 changelog.md。

要求：
1. 输出纯 Markdown，不要包裹代码块。
2. 一级标题必须是 "# Changelog"。
3. 使用 "## {release_version} - YYYY-MM-DD" 作为版本标题，日期按上下文生成时间所在日期。
4. 按实际内容组织为“新增”“优化”“修复”“构建”“测试”“维护”“行为变化”等小节；没有内容的小节不要出现。
5. 面向普通用户优先描述功能、稳定性、兼容性和可下载产物相关变化；内部实现细节只在会影响用户或发布质量时提及。
6. 必须覆盖上下文中从上一次版本更新提交到 HEAD 的所有提交，不要遗漏明显的修复、构建和测试变化。
7. 不要列 commit hash。
8. 快速生成：不要复述分析过程，不要逐条解释提交；先合并同类变化，再直接写最终稿。
9. 正文控制在约 600 至 1600 个中文字符、最多 24 个列表项；优先保留用户可感知的重要变化。

以下是完整上下文：

{context}
"""

payload = {
    "model": model,
    "messages": [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ],
    "stream": False,
    "temperature": temperature,
    "max_tokens": max_tokens,
    "thinking": {"type": thinking_type},
    "reasoning_effort": reasoning_effort,
}

request = urllib.request.Request(
    "https://api.deepseek.com/chat/completions",
    data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
    headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    },
    method="POST",
)

try:
    with urllib.request.urlopen(request, timeout=timeout) as response:
        data = json.loads(response.read().decode("utf-8"))
except urllib.error.HTTPError as error:
    body = error.read().decode("utf-8", errors="replace")
    print(f"DeepSeek HTTP {error.code}: {body}", file=sys.stderr)
    sys.exit(1)
except urllib.error.URLError as error:
    print(f"DeepSeek request failed: {error}", file=sys.stderr)
    sys.exit(1)

try:
    choice = data["choices"][0]
    content = choice["message"]["content"]
except (KeyError, IndexError, TypeError) as error:
    print(f"Unexpected DeepSeek response: {json.dumps(data, ensure_ascii=False)[:4000]}", file=sys.stderr)
    raise SystemExit(1) from error

finish_reason = choice.get("finish_reason")
if finish_reason != "stop":
    usage = json.dumps(data.get("usage", {}), ensure_ascii=False)
    print(
        f"DeepSeek changelog generation stopped unexpectedly: "
        f"finish_reason={finish_reason}, usage={usage}",
        file=sys.stderr,
    )
    raise SystemExit(1)
if not isinstance(content, str):
    print("DeepSeek changelog response content is not a string", file=sys.stderr)
    raise SystemExit(1)

content = content.strip()
if content.startswith("```"):
    lines = content.splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    if lines and lines[-1].strip() == "```":
        lines = lines[:-1]
    content = "\n".join(lines).strip()

if not content.startswith("# Changelog"):
    content = f"# Changelog\n\n{content}"

expected_version_heading = f"## {release_version} - "
if len(content) < 200 or expected_version_heading not in content:
    print(
        f"DeepSeek changelog response is incomplete: "
        f"length={len(content)}, expected_heading={expected_version_heading!r}",
        file=sys.stderr,
    )
    raise SystemExit(1)

output_path.write_text(f"{content.rstrip()}\n", encoding="utf-8")
PY
}

create_assets_zip() {
    local assets_dir="$1"
    local assets_zip="$2"
    [[ -d "${assets_dir}" ]] || fail "资源目录不存在，无法生成 assets.zip: ${assets_dir}"

    run python3 - "${assets_dir}" "${assets_zip}" <<'PY'
import pathlib
import sys
import zipfile

assets_dir = pathlib.Path(sys.argv[1]).resolve()
assets_zip = pathlib.Path(sys.argv[2]).resolve()
assets_zip.parent.mkdir(parents=True, exist_ok=True)
if assets_zip.exists():
    assets_zip.unlink()

with zipfile.ZipFile(assets_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(assets_dir.rglob("*")):
        if not path.is_file():
            continue
        archive.write(path, pathlib.Path("assets") / path.relative_to(assets_dir))
PY
}

find_file_by_name() {
    local preferred_dir="$1"
    local file_name="$2"
    local fallback_dir="$3"

    if [[ -f "${preferred_dir}/${file_name}" ]]; then
        printf '%s\n' "${preferred_dir}/${file_name}"
        return
    fi

    local matches=()
    mapfile -t matches < <(find "${fallback_dir}" -type f -name "${file_name}" | sort)
    if (( ${#matches[@]} > 0 )); then
        printf '%s\n' "${matches[0]}"
    fi
}

clean_website_generated_paths() {
    if [[ -n "$(git -C "${website_dir}" status --porcelain -- release public src/config/app.ts)" ]]; then
        log "清理上次失败发布遗留的生成物"
        run git -C "${website_dir}" restore --staged --worktree -- release public src/config/app.ts
        run git -C "${website_dir}" clean -fd -- release public
    fi
}

prepare_website_git() {
    log "同步网站仓库"
    if [[ -z "${website_branch}" ]]; then
        website_branch="$(git -C "${website_dir}" branch --show-current)"
    fi
    [[ -n "${website_branch}" ]] || fail "无法确定网站仓库分支"

    run git -C "${website_dir}" fetch --prune origin
    run git -C "${website_dir}" checkout "${website_branch}"
    clean_website_generated_paths
    run git -C "${website_dir}" pull --ff-only origin "${website_branch}"
    clean_website_generated_paths

    if [[ -n "$(git -C "${website_dir}" status --porcelain)" ]]; then
        git -C "${website_dir}" status --short
        fail "网站仓库在发布前存在未提交改动"
    fi
}

copy_release_to_website() {
    local website_release_dir="$1"
    log "复制 CI 产物到 ${website_release_dir}"
    run rm -rf "${website_release_dir}"
    run mkdir -p "${website_release_dir}"
    run cp -a "${source_release_dir}/." "${website_release_dir}/"
    run find "${website_release_dir}" -type f -name "*.dbg" -delete
    create_assets_zip "${website_release_dir}/assets" "${website_release_dir}/assets.zip"
}

build_and_deploy_website() {
    local website_release_dir="$1"
    local release_version="$2"
    local changelog_file="$3"
    local preferred_windows_dir="${MMM_RELEASE_WINDOWS_DIR:-windows-msvc-clang}"
    local preferred_linux_dir="${MMM_RELEASE_LINUX_DIR:-linux-gcc14}"
    local preferred_macos_dir="${MMM_RELEASE_MACOS_DIR:-macos-arm64}"
    local windows_exe
    local windows_updater
    local windows_pdb
    local linux_exe
    local macos_dmg
    local macos_app_zip
    local macos_updater
    local release_args

    windows_exe="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Next.exe" "${website_release_dir}")"
    windows_updater="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Updater.exe" "${website_release_dir}")"
    windows_pdb="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Next.pdb" "${website_release_dir}")"
    linux_exe="$(find_file_by_name "${website_release_dir}/${preferred_linux_dir}" "MusicMapMaker-Next" "${website_release_dir}")"
    macos_dmg="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Next.dmg" "${website_release_dir}")"
    macos_app_zip="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Next.app.zip" "${website_release_dir}")"
    macos_updater="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Updater" "${website_release_dir}")"

    [[ -n "${windows_exe}" ]] || fail "找不到 Windows 主程序产物"
    [[ -n "${windows_updater}" ]] || fail "找不到 Windows 更新器产物"
    [[ -n "${windows_pdb}" ]] || fail "找不到 Windows PDB 产物"
    [[ -n "${macos_dmg}" ]] || fail "找不到 macOS DMG 产物"
    [[ -n "${macos_app_zip}" ]] || fail "找不到 macOS App ZIP 产物"
    [[ -n "${macos_updater}" ]] || fail "找不到 macOS 更新器产物"
    [[ -f "${website_release_dir}/assets.zip" ]] || fail "找不到 assets.zip"

    release_args=(
        --release-dir "${website_release_dir}"
        --version "${release_version}"
        --assets-version "${release_version}"
        --changelog-file "${changelog_file}"
        --windows-exe "${windows_exe}"
        --windows-updater "${windows_updater}"
        --windows-pdb "${windows_pdb}"
        --macos-dmg "${macos_dmg}"
        --macos-app-zip "${macos_app_zip}"
        --macos-updater "${macos_updater}"
        --assets "${website_release_dir}/assets.zip"
    )
    if [[ -n "${linux_exe}" ]]; then
        release_args+=(--linux-exe "${linux_exe}")
    fi

    log "安装网站依赖"
    run npm --prefix "${website_dir}" ci

    log "执行网站完整构建和部署"
    (
        cd "${website_dir}"
        run npm run deploy -- "${release_args[@]}"
    )
}

commit_and_push_website() {
    local release_version="$1"
    log "提交并推送网站仓库变更"
    run git -C "${website_dir}" add release public src/config/app.ts

    if git -C "${website_dir}" diff --cached --quiet; then
        log "网站仓库没有需要提交的变更"
        return
    fi

    run git -C "${website_dir}" commit -m "chore(release): 发布 MusicMapMaker-Next ${release_version}"
    run git -C "${website_dir}" push origin "HEAD:${website_branch}"
}

main() {
    local release_version
    local version_update_commit
    local commit_range
    local commit_count
    local website_release_dir
    local changelog_file
    local generated_changelog_file
    local context_file
    local context_dir

    source_release_dir="$(cd -- "${source_release_dir}" && pwd)"
    if [[ ! -d "${source_release_dir}/assets" && -d "${source_release_dir}/release/assets" ]]; then
        source_release_dir="$(cd -- "${source_release_dir}/release" && pwd)"
    fi
    website_dir="$(cd -- "${website_dir}" && pwd)"
    release_version="$(read_release_version)"
    version_update_commit="$(find_version_update_commit "${release_version}")"
    [[ -n "${version_update_commit}" ]] || fail "无法定位上一次版本更新提交"
    commit_range="$(commit_range_for_base "${version_update_commit}")"
    commit_count="$(git -C "${source_root}" rev-list --count "${commit_range}")"
    [[ "${commit_count}" -gt 0 ]] || fail "提交范围为空: ${commit_range}"

    log "发布版本: ${release_version}"
    log "版本更新提交: ${version_update_commit}"
    log "提交范围: ${commit_range} (${commit_count} commits)"

    context_dir="$(mktemp -d)"
    context_file="${context_dir}/branch-details.md"
    generated_changelog_file="${context_dir}/changelog.md"
    trap "rm -rf -- '${context_dir}'" EXIT
    write_branch_context "${context_file}" "${release_version}" "${version_update_commit}" "${commit_range}" "${commit_count}"
    generate_changelog "${context_file}" "${generated_changelog_file}" "${release_version}"

    prepare_website_git

    website_release_dir="${website_dir}/release"
    changelog_file="${website_release_dir}/changelog.md"
    copy_release_to_website "${website_release_dir}"
    run cp "${generated_changelog_file}" "${changelog_file}"

    build_and_deploy_website "${website_release_dir}" "${release_version}" "${changelog_file}"
    commit_and_push_website "${release_version}"
}

main "$@"
