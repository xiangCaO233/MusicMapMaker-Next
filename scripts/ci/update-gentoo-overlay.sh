#!/usr/bin/env bash
# 正式 Release 已发布后同步固定 tag 的 Gentoo 源码配方，不执行应用发布。
# 独立入口允许仅重试 overlay 同步，避免覆盖或重建已存在的 Release。
# 普通 QA 构建不得调用此入口，否则开发提交会被误标为稳定来源。
# 源码包版本来自正式 tag，不复用应用二进制或 CI 分支的可变 HEAD。
set -euo pipefail

# 调用方必须显式传入本次发布标签，不能从分支 HEAD 猜测稳定版本。
release_tag="${1:?Usage: update-gentoo-overlay.sh vMAJOR.MINOR.PATCH [target-sha] [hotfix-revision DEB]}"
target_sha="${2:-}"
# 修订零为普通正式发布；正修订必须携带已经通过载荷验证的 DEB。
hotfix_revision="${3:-0}"
hotfix_deb="${4:-}"
# 数值限制与网页 hotfix 契约相同，避免包修订使用另一个解释范围。
[[ "${hotfix_revision}" =~ ^(0|[1-9][0-9]{0,9})$ ]] &&
    (( hotfix_revision <= 2147483647 )) || exit 1
repository="xiangCaO233/MusicMapMaker-Next"
overlay_repository="xiangCaO233/mmm-gentoo-overlay"
# 正式版本入口不接受预发布后缀，也不让标签参与路径或命令注入。
[[ "${release_tag}" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || {
    printf 'error: unsupported stable tag\n' >&2
    exit 1
}

# 使用主仓短期 token 读取公开发布状态；草稿和预发布不能进入稳定配方。
# latest 校验阻止重试较旧发布时把 overlay 的最新版本回退。
published_tag="$(gh api "repos/${repository}/releases/tags/${release_tag}" --jq 'select(.draft == false and .prerelease == false) | .tag_name')"
# 两次查询都必须成功，网络错误不能被误判为尚未同步的首次发布。
latest_tag="$(gh api "repos/${repository}/releases/latest" --jq .tag_name)"
[[ "${published_tag}" == "${release_tag}" && "${latest_tag}" == "${release_tag}" ]] || {
    printf 'error: tag is not the latest published stable Release\n' >&2
    exit 1
}
# 首次发布调用可核对精确源码 SHA；独立重试仍必须验证远端 tag 已存在。
published_sha="$(gh api "repos/${repository}/commits/${release_tag}" --jq .sha)"
# commits API 会解引用 annotated tag，避免只比较 tag 对象本身的 SHA。
[[ "${published_sha}" =~ ^[0-9a-f]{40}$ ]] || exit 1
if [[ "${hotfix_revision}" == 0 && -n "${target_sha}" && "${target_sha}" != "${published_sha}" ]]; then
    printf 'error: released tag does not match the build source SHA\n' >&2
    exit 1
fi
if [[ "${hotfix_revision}" != 0 ]]; then
    # 热修复源码标签仅用于固定对应维护提交，不另建一个正式 Release。
    # 独立恢复也要核对线上修订，禁止把任意维护分支 HEAD 变成包更新。
    [[ "${target_sha}" =~ ^[0-9a-f]{40}$ && -f "${hotfix_deb}" ]] || exit 1
    python3 - "${release_tag}" "${hotfix_revision}" "${target_sha}" <<'PY'
import json, sys
from urllib.request import urlopen
with urlopen('https://mmm.xiang233.top/download/check/check.json', timeout=30) as response:
    published = json.load(response)
# 此入口只能镜像已成功公开的程序身份，不能提前覆盖 Gentoo 稳定版本。
if (published.get('version') != sys.argv[1] or published.get('hotfix_base_tag') != sys.argv[1]
        or published.get('hotfix_revision') != int(sys.argv[2])
        or published.get('hotfix_commit') != sys.argv[3]):
    raise ValueError('hotfix is not the currently published program')
PY
    hotfix_tag="${release_tag}-hotfix.${hotfix_revision}"
    # matching-refs 是前缀查询，后续 jq 必须按完整 ref 过滤才能避免 r1/r10 混淆。
    # 同一标签可幂等复用，但任何不同 SHA 都拒绝；永不移动已有 tag。
    existing_sha="$(gh api "repos/${repository}/git/matching-refs/tags/${hotfix_tag}" \
        --jq ".[] | select(.ref == \"refs/tags/${hotfix_tag}\") | .object.sha")"
    if [[ -n "${existing_sha}" ]]; then
        # 不接受移动过的标签，恢复操作只能重用同一不可变提交。
        [[ "${existing_sha}" == "${target_sha}" ]] || exit 1
    else
        # 只创建新的轻量 tag，不修改正式 Release 或生成另一批二进制。
        gh api --method POST "repos/${repository}/git/refs" \
            -f "ref=refs/tags/${hotfix_tag}" -f "sha=${target_sha}" >/dev/null
    fi
fi

# 私钥仅可写 overlay 仓；空 secret 明确失败，不借用 Runner 的个人凭据。
: "${MMM_GENTOO_OVERLAY_SSH_KEY:?Missing GENTOO_OVERLAY_SSH_KEY secret}"
# 密钥、主机公钥及 checkout 均放在本次私有临时目录，不污染共享 Runner。
temporary_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/mmm-overlay-sync.XXXXXX")"
trap 'rm -rf -- "${temporary_root}"' EXIT
umask 077
printf '%s\n' "${MMM_GENTOO_OVERLAY_SSH_KEY}" > "${temporary_root}/key"
# 主机公钥通过 GitHub HTTPS API 获取，SSH 必须严格验证，禁止关闭校验。
gh api meta --jq '.ssh_keys[] | "github.com " + .' > "${temporary_root}/known_hosts"
# 非空检查防止 API 异常产生空信任文件，且不允许自动接受未知主机密钥。
[[ -s "${temporary_root}/known_hosts" ]] || exit 1
# shell 转义只构造 SSH argv，不将标签、提交消息或私钥拼进命令字符串。
printf -v GIT_SSH_COMMAND 'ssh -i %q -o IdentitiesOnly=yes -o UserKnownHostsFile=%q -o StrictHostKeyChecking=yes' \
    "${temporary_root}/key" "${temporary_root}/known_hosts"
export GIT_SSH_COMMAND

# 只获取当前 main；push 不强制覆盖，并发写入时失败后可以安全重试。
git clone --depth 1 --branch main "git@github.com:${overlay_repository}.git" "${temporary_root}/overlay"
overlay_root="${temporary_root}/overlay"
generator_args=(--tag "${release_tag}")
# 同一生成器覆盖普通正式版本与热修复，保留既有普通发布行为。
if [[ "${hotfix_revision}" != 0 ]]; then
    generator_args+=(--hotfix-revision "${hotfix_revision}" --deb "${hotfix_deb}")
fi
python3 "${overlay_root}/scripts/update-source-release.py" "${generator_args[@]}"
# 配方生成器保留历史版本，新包名称包含版本号，Gentoo 更新会选择较新版本。
suffix=""
# 包名里的 rN 属于 Gentoo 版本排序，不能写进 PV 的上游三段版本。
if [[ "${hotfix_revision}" != 0 ]]; then suffix="-r${hotfix_revision}"; fi
ebuild_path="app-editors/musicmapmaker-next/musicmapmaker-next-${release_tag#v}${suffix}.ebuild"
# 新配方至少通过 Bash 语法检查；构建选项由已验证的开发版模板继承。
bash -n "${overlay_root}/${ebuild_path}"
git -C "${overlay_root}" add -- "${ebuild_path}"
if [[ "${hotfix_revision}" != 0 ]]; then
    # Manifest 使用原 DEB 字节生成，不能下载另一个工具链的包来凑更新。
    # 二进制源比正式 r1 高一个修订；所有文件和 Manifest 在同一提交中公布。
    binary_path="app-editors/musicmapmaker-next-bin/musicmapmaker-next-bin-${release_tag#v}-r$((hotfix_revision + 1)).ebuild"
    bash -n "${overlay_root}/${binary_path}"
    git -C "${overlay_root}" add -- "${binary_path}" app-editors/musicmapmaker-next-bin/Manifest
fi
if git -C "${overlay_root}" diff --cached --quiet; then
    # 同一 tag 的恢复操作不生成重复提交，也不改旧配方。
    printf 'Gentoo source ebuild already synchronized: %s\n' "${release_tag}"
    exit 0
fi
git -C "${overlay_root}" diff --cached --check
# 源码配方、二进制配方和摘要一起提交，客户端不会看到缺少 Manifest 的中间状态。
# 提交仅包含本次新版本配方；身份固定为 Actions bot，遵循中文提交规范。
git -C "${overlay_root}" -c user.name='github-actions[bot]' \
    -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
    commit -m "ci(gentoo): 更新软件包至 ${release_tag}${suffix}"
git -C "${overlay_root}" push origin HEAD:main
# 跨仓 push 冲突不覆盖别人提交；使用保留的包单独恢复本步骤。
# 只有远端 push 成功才报告完成，失败时现有 Release 和 tag 均保持原状。
printf 'Gentoo source ebuild synchronized: %s (%s)\n' "${release_tag}" "${published_sha}"
