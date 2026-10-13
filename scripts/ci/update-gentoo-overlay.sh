#!/usr/bin/env bash
# 正式 Release 已发布后同步固定 tag 的 Gentoo 源码配方，不执行应用发布。
# 独立入口允许仅重试 overlay 同步，避免覆盖或重建已存在的 Release。
# 普通 QA 构建不得调用此入口，否则开发提交会被误标为稳定来源。
# 源码包版本来自正式 tag，不复用应用二进制或 CI 分支的可变 HEAD。
set -euo pipefail

# 调用方必须显式传入本次发布标签，不能从分支 HEAD 猜测稳定版本。
release_tag="${1:?Usage: update-gentoo-overlay.sh vMAJOR.MINOR.PATCH [target-sha]}"
target_sha="${2:-}"
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
if [[ -n "${target_sha}" && "${target_sha}" != "${published_sha}" ]]; then
    printf 'error: released tag does not match the build source SHA\n' >&2
    exit 1
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
python3 "${overlay_root}/scripts/update-source-release.py" --tag "${release_tag}"
# 配方生成器保留历史版本，新包名称包含版本号，Gentoo 更新会选择较新版本。
ebuild_path="app-editors/musicmapmaker-next/musicmapmaker-next-${release_tag#v}.ebuild"
# 新配方至少通过 Bash 语法检查；构建选项由已验证的开发版模板继承。
bash -n "${overlay_root}/${ebuild_path}"
git -C "${overlay_root}" add -- "${ebuild_path}"
if git -C "${overlay_root}" diff --cached --quiet; then
    # 同一 tag 的恢复操作不生成重复提交，也不改旧配方。
    printf 'Gentoo source ebuild already synchronized: %s\n' "${release_tag}"
    exit 0
fi
git -C "${overlay_root}" diff --cached --check
# 提交仅包含本次新版本配方；身份固定为 Actions bot，遵循中文提交规范。
git -C "${overlay_root}" -c user.name='github-actions[bot]' \
    -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
    commit -m "ci(gentoo): 更新源码包至 ${release_tag}"
git -C "${overlay_root}" push origin HEAD:main
# 只有远端 push 成功才报告完成，失败时现有 Release 和 tag 均保持原状。
printf 'Gentoo source ebuild synchronized: %s (%s)\n' "${release_tag}" "${published_sha}"
