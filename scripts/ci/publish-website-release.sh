#!/usr/bin/env bash
# 将聚合 CI 产物和自动生成的 changelog 发布到独立网站仓库。
# 流程同步网站分支、复制发布文件、构建部署内容，并提交推送生成结果。
# changelog 仅依据版本区间内的 Git 上下文调用 DeepSeek 生成。
# 发布前要求两个仓库和产物目录有效，网站仓库除可清理生成物外必须洁净。
# 临时上下文保存在 mktemp 目录，并在脚本退出时统一清理。

# 加载用户环境前保留 pipefail，避免配置文件中的管道错误被忽略。
set -o pipefail

# 加载自托管 Runner 用户的命令和密钥环境。
loadUserBashrc() {
    local bashrcPath="${HOME:-}/.bashrc"
    if [[ -n "${HOME:-}" && -f "${bashrcPath}" ]]; then
        # shellcheck source=/dev/null
        # 文件存在且 HOME 非空时才 source，避免意外解析根目录路径。
        source "${bashrcPath}"
    fi
}

# 用户环境加载完成后启用严格未定义变量和立即失败模式。
loadUserBashrc
set -euo pipefail

readonly DEFAULT_SOURCE_RELEASE_DIR="/home/xiang/MusicMapMaker-Next/release"
readonly DEFAULT_WEBSITE_DIR="/home/xiang/mmm-website"
readonly DEFAULT_DEEPSEEK_MODEL="deepseek-v4-flash"

# 通过脚本自身位置定位 MusicMapMaker-Next 仓库。
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_root="$(cd -- "${script_dir}/../.." && pwd)"

source_release_dir="${DEFAULT_SOURCE_RELEASE_DIR}"
website_dir="${DEFAULT_WEBSITE_DIR}"
# 分支空值表示稍后采用网站仓当前分支。
website_branch="${MMM_WEBSITE_BRANCH:-}"
deepseek_model="${DEEPSEEK_MODEL:-${DEFAULT_DEEPSEEK_MODEL}}"

# 这里使用前插 PATH，按低优先级到高优先级排列，确保 node24 优先于 node20。
# 仅当目录含可执行 npm 且尚未位于 PATH 时才插入。
for npm_bin_dir in \
    "/home/xiang/actions-runner/externals/node20/bin" \
    "/home/xiang/actions-runner/externals/node24/bin"; do
    if [[ ":${PATH}:" != *":${npm_bin_dir}:"* && -x "${npm_bin_dir}/npm" ]]; then
        # 后处理的 node24 前插到 node20 之前。
        PATH="${npm_bin_dir}:${PATH}"
    fi
done
export PATH

# 输出发布参数、凭据和产物目录覆盖入口。
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

# 写入带稳定前缀的普通进度日志。
log() {
    printf '[website-release] %s\n' "$*"
}

# 输出统一错误并终止发布事务。
fail() {
    printf '[website-release] error: %s\n' "$*" >&2
    exit 1
}

# 回显经过 shell 转义的命令后原样执行。
run() {
    printf '[website-release] run:'
    # %q 保留参数边界且不泄漏未作为参数传入的环境变量。
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

# 验证宿主命令存在，脚本不自动安装工具。
require_command() {
    local command_name="$1"
    command -v "${command_name}" > /dev/null 2>&1 || fail "找不到命令: ${command_name}"
}

# 在同步仓库或复制文件前完整解析选项。
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-release)
            # 聚合目录可由 CI artifact 下载位置覆盖。
            [[ $# -ge 2 ]] || fail "--source-release 需要目录参数"
            source_release_dir="$2"
            shift 2
            ;;
        --website-dir)
            # 网站仓库可以位于自托管 Runner 的任意绝对位置。
            [[ $# -ge 2 ]] || fail "--website-dir 需要目录参数"
            website_dir="$2"
            shift 2
            ;;
        --website-branch)
            # 显式分支覆盖当前 checkout 分支推导。
            [[ $# -ge 2 ]] || fail "--website-branch 需要分支名"
            website_branch="$2"
            shift 2
            ;;
        --help|-h)
            # 帮助成功退出且不要求凭据。
            usage
            exit 0
            ;;
        *)
            # 未知参数通过统一 fail 路径处理。
            fail "未知参数: $1"
            ;;
    esac
done

require_command git
# npm 负责网站依赖与部署，Python 负责归档和 API 调用。
require_command npm
require_command python3

[[ -d "${source_root}/.git" ]] || fail "当前脚本不在 MusicMapMaker-Next 仓库中"
# 三个根目录必须在任何外部修改前完成验证。
[[ -d "${source_release_dir}" ]] || fail "CI 产物目录不存在: ${source_release_dir}"
[[ -d "${website_dir}/.git" ]] || fail "网站仓库目录不存在或不是 Git 仓库: ${website_dir}"

# 从输入文本解析首个 CMake set(APPVER ...) 值。
read_app_version_from_text() {
    local app_version
    app_version=""
    while IFS= read -r line; do
        # 只匹配明确变量名与双引号值，不执行 CMake。
        if [[ "${line}" =~ ^[[:space:]]*set[[:space:]]*\([[:space:]]*APPVER[[:space:]]+\"([^\"]+)\" ]]; then
            app_version="${BASH_REMATCH[1]}"
            # 首个规范声明是当前文本的版本来源。
            break
        fi
    done
    printf '%s\n' "${app_version}"
}

# 读取当前版本并统一添加 v 前缀。
read_release_version() {
    local app_version
    app_version="$(read_app_version_from_text < "${source_root}/CMakeLists.txt")"
    [[ -n "${app_version}" ]] || fail "无法从 CMakeLists.txt 读取 APPVER"

    if [[ "${app_version}" == v* ]]; then
        # 已有前缀保持原值。
        printf '%s\n' "${app_version}"
    else
        # 网站版本标识始终采用 v 前缀。
        printf 'v%s\n' "${app_version}"
    fi
}

# 查找当前版本更新之前的上一版本更新提交作为 changelog 基线。
find_version_update_commit() {
    local release_version="$1"
    local normalized_release_version
    local commit_version
    local version_update_commits
    local commit_index

    if [[ -n "${MMM_VERSION_UPDATE_COMMIT:-}" ]]; then
        # 显式基线必须解析为 commit 对象。
        git -C "${source_root}" rev-parse --verify "${MMM_VERSION_UPDATE_COMMIT}^{commit}"
        return
    fi

    normalized_release_version="${release_version#v}"
    # 只枚举实际修改过 APPVER 文本的提交。
    mapfile -t version_update_commits < <(git -C "${source_root}" log --format=%H -G 'APPVER' -- CMakeLists.txt)
    for commit_index in "${!version_update_commits[@]}"; do
        # 从历史提交的 CMakeLists 精确读取当时版本。
        commit_version="$(read_app_version_from_text < <(git -C "${source_root}" show "${version_update_commits[commit_index]}:CMakeLists.txt"))"
        if [[ "${commit_version#v}" == "${normalized_release_version}" ]]; then
            # 找到当前版本更新后，取更旧一项作为范围起点。
            if (( commit_index + 1 >= ${#version_update_commits[@]} )); then
                # 没有更旧版本时返回空，交由主流程明确失败。
                return
            fi
            printf '%s\n' "${version_update_commits[commit_index + 1]}"
            return
        fi
    done

    printf '%s\n' "${version_update_commits[0]:-}"
}

# 为有父提交和仓库根提交分别构造可被 Git 接受的范围。
commit_range_for_base() {
    local base_commit="$1"
    if git -C "${source_root}" rev-parse --verify "${base_commit}^" > /dev/null 2>&1; then
        # 常规提交包含基线自身到 HEAD 的完整变化。
        printf '%s^..HEAD\n' "${base_commit}"
    else
        # 根提交没有父项，只能直接使用单提交 revision。
        printf '%s\n' "${base_commit}"
    fi
}

# 将发布基线、分支信息、提交列表和逐提交详情写入模型上下文。
write_branch_context() {
    local context_file="$1"
    local release_version="$2"
    local base_commit="$3"
    local commit_range="$4"
    local commit_count="$5"
    local upstream
    local origin_url

    # 没有上游或 origin 时保留空值，并在文档中标记未设置。
    upstream="$(git -C "${source_root}" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    origin_url="$(git -C "${source_root}" config --get remote.origin.url || true)"

    # 文档头记录可复核的仓库和版本元数据。
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
        # 先按时间正序生成紧凑提交索引。
        git -C "${source_root}" log --reverse --date=iso-strict \
            --format='- %H | %ad | %an <%ae> | %s%d' "${commit_range}"

        printf '\n## 提交详细信息\n'
        # 再写入每个提交的作者、正文、统计和文件状态。
        while IFS= read -r commit_hash; do
            printf '\n### %s\n\n' "${commit_hash}"
            git -C "${source_root}" show --no-ext-diff --find-renames --find-copies \
                --stat --summary --name-status --date=iso-strict \
                --format='commit %H%nAuthor: %an <%ae>%nAuthorDate: %aI%nCommit: %cn <%ce>%nCommitDate: %cI%nSubject: %s%n%nBody:%n%b' \
                "${commit_hash}"
        done < <(git -C "${source_root}" rev-list --reverse "${commit_range}")
    } > "${context_file}"
}

# 调用 DeepSeek 生成经过结构和完整性最低校验的中文 changelog。
generate_changelog() {
    local context_file="$1"
    local changelog_file="$2"
    local release_version="$3"
    local api_key

    api_key="${DEEPSEEK_APIKEY:-${DEEPSEEK_API_KEY:-}}"
    # 支持历史环境变量名，同时优先使用无下划线形式。
    [[ -n "${api_key}" ]] || fail "未设置 DEEPSEEK_APIKEY 或 DEEPSEEK_API_KEY"

    log "使用 DeepSeek 生成 ${changelog_file}"
    # 密钥和模型通过单次命令环境传入嵌入式 Python。
    DEEPSEEK_EFFECTIVE_API_KEY="${api_key}" \
    DEEPSEEK_EFFECTIVE_MODEL="${deepseek_model}" \
    python3 - "${context_file}" "${changelog_file}" "${release_version}" <<'PY'
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

# 三个位置参数分别固定输入、输出和期望版本标题。
context_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
release_version = sys.argv[3]

api_key = os.environ["DEEPSEEK_EFFECTIVE_API_KEY"]
model = os.environ["DEEPSEEK_EFFECTIVE_MODEL"]
# 生成长度、随机性、思考方式和网络超时均可由 CI 覆盖。
max_tokens = int(os.environ.get("DEEPSEEK_MAX_TOKENS", "12000"))
temperature = float(os.environ.get("DEEPSEEK_TEMPERATURE", "0.2"))
thinking_type = os.environ.get("DEEPSEEK_THINKING", "enabled")
reasoning_effort = os.environ.get("DEEPSEEK_REASONING_EFFORT", "low")
timeout = int(os.environ.get("DEEPSEEK_TIMEOUT", "300"))

# 明确使用 UTF-8 保存中文提交和提示词。
context = context_path.read_text(encoding="utf-8")
# system prompt 限制模型只使用已提供的 Git 证据。
system_prompt = (
    "你是 MusicMapMaker-Next 项目的发布维护者。"
    "请只依据用户提供的 Git 分支发布上下文生成面向用户的中文 Markdown changelog，"
    "不要臆造上下文中没有的信息。"
    "不要逐提交展开长篇推理，只做必要的归类合并，尽快直接输出最终 Markdown。"
)
# user prompt 规定面向用户的格式、覆盖范围和篇幅。
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

# 请求使用非流式响应，便于一次性校验完整 Markdown。
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

# API key 只进入 Authorization header，不写入上下文或磁盘。
request = urllib.request.Request(
    "https://api.deepseek.com/chat/completions",
    data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
    headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    },
    method="POST",
)

# 区分 HTTP 响应错误与网络连接错误以保留诊断。
try:
    with urllib.request.urlopen(request, timeout=timeout) as response:
        data = json.loads(response.read().decode("utf-8"))
except urllib.error.HTTPError as error:
    # 截取服务返回正文交给 CI 日志定位请求问题。
    body = error.read().decode("utf-8", errors="replace")
    print(f"DeepSeek HTTP {error.code}: {body}", file=sys.stderr)
    sys.exit(1)
except urllib.error.URLError as error:
    # 网络层失败不生成空 changelog。
    print(f"DeepSeek request failed: {error}", file=sys.stderr)
    sys.exit(1)

# 响应必须包含首个 choice 的 message.content。
try:
    choice = data["choices"][0]
    content = choice["message"]["content"]
except (KeyError, IndexError, TypeError) as error:
    # 异常结构最多打印前 4000 字符，避免日志无限膨胀。
    print(f"Unexpected DeepSeek response: {json.dumps(data, ensure_ascii=False)[:4000]}", file=sys.stderr)
    raise SystemExit(1) from error

# 非 stop 状态通常表示截断或服务端中止，不能作为正式 changelog。
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
    # JSON 非字符串内容无法写入 Markdown 文件。
    print("DeepSeek changelog response content is not a string", file=sys.stderr)
    raise SystemExit(1)

# 清理响应外围空白和模型可能附加的代码围栏。
content = content.strip()
if content.startswith("```"):
    lines = content.splitlines()
    if lines and lines[0].startswith("```"):
        # 首行允许 markdown 等语言标签。
        lines = lines[1:]
    if lines and lines[-1].strip() == "```":
        # 只移除成对出现在末尾的围栏行。
        lines = lines[:-1]
    content = "\n".join(lines).strip()

if not content.startswith("# Changelog"):
    # 缺少一级标题时补齐统一入口。
    content = f"# Changelog\n\n{content}"

expected_version_heading = f"## {release_version} - "
# 长度与版本标题共同拦截明显截断或错版本响应。
if len(content) < 200 or expected_version_heading not in content:
    print(
        f"DeepSeek changelog response is incomplete: "
        f"length={len(content)}, expected_heading={expected_version_heading!r}",
        file=sys.stderr,
    )
    raise SystemExit(1)

# 最终文件统一以单个换行结束。
output_path.write_text(f"{content.rstrip()}\n", encoding="utf-8")
PY
}

# 以确定顺序生成保留 assets 顶层目录的高压缩 ZIP。
create_assets_zip() {
    local assets_dir="$1"
    local assets_zip="$2"
    [[ -d "${assets_dir}" ]] || fail "资源目录不存在，无法生成 assets.zip: ${assets_dir}"

    # 使用 Python zipfile 保证跨 Runner 的归档行为一致。
    run python3 - "${assets_dir}" "${assets_zip}" <<'PY'
import pathlib
import sys
import zipfile

# 输入输出都转为绝对路径，避免更改工作目录产生歧义。
assets_dir = pathlib.Path(sys.argv[1]).resolve()
assets_zip = pathlib.Path(sys.argv[2]).resolve()
assets_zip.parent.mkdir(parents=True, exist_ok=True)
# 覆盖前只删除解析后的单一目标归档。
if assets_zip.exists():
    assets_zip.unlink()

# 固定 DEFLATED 和最高压缩级别以控制网站下载体积。
with zipfile.ZipFile(assets_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    # 排序遍历提供稳定的归档成员顺序。
    for path in sorted(assets_dir.rglob("*")):
        if not path.is_file():
            # 目录条目由文件路径隐式表达，不单独写入。
            continue
        # 每个成员统一挂在 assets/ 下，匹配程序资源目录结构。
        archive.write(path, pathlib.Path("assets") / path.relative_to(assets_dir))
PY
}

# 优先从指定平台目录查找文件，缺失时回退到聚合目录全局首项。
find_file_by_name() {
    local preferred_dir="$1"
    local file_name="$2"
    local fallback_dir="$3"

    if [[ -f "${preferred_dir}/${file_name}" ]]; then
        # 精确首选项可避免多工具链同名文件选择不稳定。
        printf '%s\n' "${preferred_dir}/${file_name}"
        return
    fi

    local matches=()
    # 全局回退排序后只返回首个现有文件。
    mapfile -t matches < <(find "${fallback_dir}" -type f -name "${file_name}" | sort)
    if (( ${#matches[@]} > 0 )); then
        printf '%s\n' "${matches[0]}"
    fi
}

# 只清理网站仓由发布流程生成的受控路径。
clean_website_generated_paths() {
    if [[ -n "$(git -C "${website_dir}" status --porcelain -- release public src/config/app.ts)" ]]; then
        # 同时恢复 tracked 内容并删除 release/public 下未跟踪生成物。
        log "清理上次失败发布遗留的生成物"
        run git -C "${website_dir}" restore --staged --worktree -- release public src/config/app.ts
        run git -C "${website_dir}" clean -fd -- release public
    fi
}

# 同步目标分支，并确保网站仓没有用户或其他任务的未提交改动。
prepare_website_git() {
    log "同步网站仓库"
    if [[ -z "${website_branch}" ]]; then
        # 未显式指定时采用当前 checkout 分支。
        website_branch="$(git -C "${website_dir}" branch --show-current)"
    fi
    [[ -n "${website_branch}" ]] || fail "无法确定网站仓库分支"

    run git -C "${website_dir}" fetch --prune origin
    # checkout 后先移除上次失败发布在受控路径内的残留。
    run git -C "${website_dir}" checkout "${website_branch}"
    clean_website_generated_paths
    run git -C "${website_dir}" pull --ff-only origin "${website_branch}"
    # 拉取后再次清理，覆盖分支切换或钩子留下的生成物。
    clean_website_generated_paths

    if [[ -n "$(git -C "${website_dir}" status --porcelain)" ]]; then
        # 受控路径之外的任何改动都需要人工处理，不能自动丢弃。
        git -C "${website_dir}" status --short
        fail "网站仓库在发布前存在未提交改动"
    fi
}

# 用当前聚合产物原子式重建网站仓的 release 目录内容。
copy_release_to_website() {
    local website_release_dir="$1"
    log "复制 CI 产物到 ${website_release_dir}"
    # 删除范围固定为网站仓内解析后的 release 目录。
    run rm -rf "${website_release_dir}"
    run mkdir -p "${website_release_dir}"
    run cp -a "${source_release_dir}/." "${website_release_dir}/"
    # 网站公开下载不携带 Linux 拆分调试文件。
    run find "${website_release_dir}" -type f -name "*.dbg" -delete
    create_assets_zip "${website_release_dir}/assets" "${website_release_dir}/assets.zip"
}

# 选择公开平台产物，安装锁定依赖并执行网站完整部署命令。
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

    # 首选目录允许 CI 覆盖，回退仅用于兼容聚合布局变化。
    windows_exe="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Next.exe" "${website_release_dir}")"
    windows_updater="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Updater.exe" "${website_release_dir}")"
    windows_pdb="$(find_file_by_name "${website_release_dir}/${preferred_windows_dir}" "MusicMapMaker-Next.pdb" "${website_release_dir}")"
    linux_exe="$(find_file_by_name "${website_release_dir}/${preferred_linux_dir}" "MusicMapMaker-Next" "${website_release_dir}")"
    macos_dmg="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Next.dmg" "${website_release_dir}")"
    macos_app_zip="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Next.app.zip" "${website_release_dir}")"
    macos_updater="$(find_file_by_name "${website_release_dir}/${preferred_macos_dir}" "MusicMapMaker-Updater" "${website_release_dir}")"

    # Windows 和 macOS 公开下载集合是网站发布硬要求。
    [[ -n "${windows_exe}" ]] || fail "找不到 Windows 主程序产物"
    [[ -n "${windows_updater}" ]] || fail "找不到 Windows 更新器产物"
    [[ -n "${windows_pdb}" ]] || fail "找不到 Windows PDB 产物"
    [[ -n "${macos_dmg}" ]] || fail "找不到 macOS DMG 产物"
    [[ -n "${macos_app_zip}" ]] || fail "找不到 macOS App ZIP 产物"
    [[ -n "${macos_updater}" ]] || fail "找不到 macOS 更新器产物"
    [[ -f "${website_release_dir}/assets.zip" ]] || fail "找不到 assets.zip"

    # 使用数组保持路径和 changelog 参数边界。
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
        # Linux 当前为可选下载，仅在发现产物时传给网站构建。
        release_args+=(--linux-exe "${linux_exe}")
    fi

    log "安装网站依赖"
    # npm ci 严格使用网站仓 lockfile，避免依赖版本漂移。
    run npm --prefix "${website_dir}" ci

    log "执行网站完整构建和部署"
    (
        # 子 shell 隔离网站工作目录变化。
        cd "${website_dir}"
        run npm run deploy -- "${release_args[@]}"
    )
}

# 将网站生成路径作为一个版本发布提交推送到目标分支。
commit_and_push_website() {
    local release_version="$1"
    log "提交并推送网站仓库变更"
    run git -C "${website_dir}" add release public src/config/app.ts

    if git -C "${website_dir}" diff --cached --quiet; then
        # 幂等结果无需创建空提交或重复推送。
        log "网站仓库没有需要提交的变更"
        return
    fi

    run git -C "${website_dir}" commit -m "chore(release): 发布 MusicMapMaker-Next ${release_version}"
    # 显式 refspec 把当前 HEAD 推送到解析后的发布分支。
    run git -C "${website_dir}" push origin "HEAD:${website_branch}"
}

# 编排版本范围、changelog、网站同步、部署与推送的完整事务。
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
    # 兼容 artifact 解压后额外包含 release 顶层目录的情况。
    if [[ ! -d "${source_release_dir}/assets" && -d "${source_release_dir}/release/assets" ]]; then
        source_release_dir="$(cd -- "${source_release_dir}/release" && pwd)"
    fi
    website_dir="$(cd -- "${website_dir}" && pwd)"
    # 版本和 Git 范围在修改网站仓之前确定。
    release_version="$(read_release_version)"
    version_update_commit="$(find_version_update_commit "${release_version}")"
    [[ -n "${version_update_commit}" ]] || fail "无法定位上一次版本更新提交"
    commit_range="$(commit_range_for_base "${version_update_commit}")"
    commit_count="$(git -C "${source_root}" rev-list --count "${commit_range}")"
    [[ "${commit_count}" -gt 0 ]] || fail "提交范围为空: ${commit_range}"

    log "发布版本: ${release_version}"
    log "版本更新提交: ${version_update_commit}"
    log "提交范围: ${commit_range} (${commit_count} commits)"

    # 临时目录集中保存可能较大的上下文和模型输出。
    context_dir="$(mktemp -d)"
    context_file="${context_dir}/branch-details.md"
    generated_changelog_file="${context_dir}/changelog.md"
    trap "rm -rf -- '${context_dir}'" EXIT
    # trap 目标来自 mktemp 的已解析路径，不接受用户输入。
    write_branch_context "${context_file}" "${release_version}" "${version_update_commit}" "${commit_range}" "${commit_count}"
    generate_changelog "${context_file}" "${generated_changelog_file}" "${release_version}"

    prepare_website_git

    # 生成目录固定为网站仓约定的 release 路径。
    website_release_dir="${website_dir}/release"
    changelog_file="${website_release_dir}/changelog.md"
    copy_release_to_website "${website_release_dir}"
    # changelog 在产物复制后写回，避免被目录重建覆盖。
    run cp "${generated_changelog_file}" "${changelog_file}"

    build_and_deploy_website "${website_release_dir}" "${release_version}" "${changelog_file}"
    # 只有完整部署成功后才提交网站仓变化。
    commit_and_push_website "${release_version}"
}

# 保留所有命令行参数边界进入主流程。
main "$@"

# 维护约束：发布输入必须来自同一 MusicMapMaker-Next 目标提交的 CI 产物。
# 维护约束：网站仓库与源码仓库保持独立 Git 历史和工作树。
# 维护约束：脚本只能自动清理 release、public 和 src/config/app.ts 生成路径。
# 维护约束：网站仓其他未提交改动必须阻止发布，不能自动恢复。
# 维护约束：分支同步只允许 fast-forward，不生成合并提交。
# 维护约束：fetch、checkout 和 pull 任一步失败都不得继续复制产物。
# 维护约束：发布分支为空时只采用网站仓当前具名分支。
# 维护约束：detached HEAD 下必须由调用方显式提供 website branch。
# 维护约束：网站远端固定使用 origin，变更需同步 CI 权限配置。
# 维护约束：受控生成路径清理前必须使用 Git porcelain 精确检测。
# 维护约束：rm -rf 目标只能是已解析网站仓内的 release 目录。
# 维护约束：聚合目录兼容只允许额外一层名为 release 的包装。
# 维护约束：复制使用 cp -a 保留可执行位、时间和目录结构。
# 维护约束：公开产物复制后统一删除 .dbg 文件以控制下载体积。
# 维护约束：Windows PDB 是网站 Windows 发布集合的必需项。
# 维护约束：Windows 主程序与更新器应优先来自同一工具链目录。
# 维护约束：macOS DMG、App ZIP 与更新器应优先来自同一架构目录。
# 维护约束：Linux 产物缺失时当前网站发布仍可继续。
# 维护约束：全局文件名回退按排序首项选择，聚合布局应避免歧义。
# 维护约束：新增同名平台变体时应优先显式设置首选目录环境变量。
# 维护约束：assets.zip 必须从本次复制后的 assets 目录重新生成。
# 维护约束：assets.zip 中每个文件必须位于 assets 顶层路径之下。
# 维护约束：归档成员排序应保持稳定，便于哈希和差异审查。
# 维护约束：旧 assets.zip 在写入新归档前只删除单一目标文件。
# 维护约束：资源归档不得包含目录之外的符号链接目标内容。
# 维护约束：npm ci 必须在 deploy 前成功完成并遵循 lockfile。
# 维护约束：npm 运行时优先级必须保持 node24 高于 node20。
# 维护约束：PATH 注入只选择确实包含可执行 npm 的 Runner 目录。
# 维护约束：用户 bashrc 加载失败应由严格模式阻止继续发布。
# 维护约束：敏感 API key 只通过环境传给单次 Python 进程。
# 维护约束：命令回显不得打印未作为参数传入的密钥环境变量。
# 维护约束：DEEPSEEK_APIKEY 始终优先于兼容名称 DEEPSEEK_API_KEY。
# 维护约束：模型名称、token 上限和超时可由 CI 环境显式固定。
# 维护约束：temperature 默认保持低值以减少发布文本随机漂移。
# 维护约束：模型只能依据生成的 Git 分支上下文撰写 changelog。
# 维护约束：上下文必须记录 HEAD、版本、基线、范围和提交数量。
# 维护约束：上下文提交列表和详细信息都按提交时间正序生成。
# 维护约束：提交详情必须保留 rename、copy、stat 与 name-status 信息。
# 维护约束：上下文文件不写入源码仓或网站仓的 tracked 路径。
# 维护约束：临时上下文在成功、失败和信号退出时都由 trap 清理。
# 维护约束：trap 删除目标必须始终来自 mktemp -d 返回值。
# 维护约束：显式版本基线必须可解析为源码仓中的 commit。
# 维护约束：自动基线只依据 CMakeLists.txt 的 APPVER 历史推导。
# 维护约束：当前版本比较时只忽略一个开头 v 前缀。
# 维护约束：找不到上一版本更新提交时必须阻止生成 changelog。
# 维护约束：提交范围为空时不得调用模型或修改网站仓。
# 维护约束：根提交范围处理不能构造不存在的父提交 revision。
# 维护约束：模型响应必须以 finish_reason=stop 正常结束。
# 维护约束：响应 content 必须是字符串且满足最低长度要求。
# 维护约束：版本二级标题必须与当前 release_version 完全匹配。
# 维护约束：代码围栏清理只处理响应最外层，不修改正文代码片段。
# 维护约束：缺失一级标题只补充统一的 Changelog 标题。
# 维护约束：DeepSeek HTTP 错误正文只进入 stderr，不写入 changelog。
# 维护约束：异常响应日志必须限制长度，避免 CI 日志失控。
# 维护约束：网络或模型失败后不得回退到陈旧 changelog。
# 维护约束：生成文件统一使用 UTF-8 和末尾单换行。
# 维护约束：发布版本标题日期必须依据上下文生成时间。
# 维护约束：changelog 不应包含 API key、完整提示词或模型推理过程。
# 维护约束：网站 deploy 参数必须使用数组保存路径边界。
# 维护约束：网站构建应同时消费版本号和 assets 版本号。
# 维护约束：changelog 文件必须位于本次重建的 release 目录。
# 维护约束：公开下载路径由网站 deploy 脚本生成，本脚本不手工拼接 URL。
# 维护约束：npm deploy 失败时不得提交或推送网站仓改动。
# 维护约束：Git 暂存范围必须限制为三个受控生成路径。
# 维护约束：无实际暂存差异时不创建空发布提交。
# 维护约束：提交消息必须包含规范 release_version。
# 维护约束：push 使用 HEAD:<branch> 明确目标分支。
# 维护约束：push 失败时保留本地提交供人工检查和重试。
# 维护约束：脚本不强推、不删除远端分支也不改写网站历史。
# 维护约束：网站仓生成提交属于外部副作用，只能在完整流程末尾发生。
# 维护约束：源仓当前工作树内容不由本脚本提交或推送。
# 维护约束：脚本不创建 MusicMapMaker-Next GitHub Release。
# 维护约束：新增平台下载项需同步查找、必需性和 deploy 参数。
# 维护约束：新增网站生成路径需同步清理、暂存和洁净检查范围。
# 维护约束：模型 API 变更时需保持响应完整性和标题验证。
# 维护约束：Node 版本变更时需同步 Runner PATH 候选优先级。
# 维护约束：成功退出表示网站构建部署完成且必要变更已推送。
# 维护约束：发布后验证网站可访问性属于调用工作流的后续步骤。
# 维护约束：默认目录仅适用于当前自托管 Runner，参数覆盖优先。
# 维护约束：网站仓路径解析后必须仍包含有效 .git 目录。
# 维护约束：源码发布目录和网站 release 目录不得指向同一位置。
# 维护约束：模型生成发生在网站仓同步之前，不依赖网站当前内容。
# 维护约束：网站同步发生在产物复制之前，避免 pull 覆盖新文件。
# 维护约束：产物复制发生在 changelog 写入之前，避免重建目录误删文本。
# 维护约束：网站部署发生在 Git add 之前，只有成功输出进入提交。
# 维护约束：Git commit 发生在 push 之前，失败时不得继续远端写入。
# 维护约束：默认 DeepSeek 模型变更需先验证 API 参数兼容性。
# 维护约束：响应篇幅限制应与 API max_tokens 和完整性阈值协调。
# 维护约束：发布脚本日志应足以复核版本范围与目标分支。
# 维护约束：调用方应在发布后独立验证下载链接和校验信息。
