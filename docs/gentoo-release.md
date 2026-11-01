# Gentoo 稳定源码包发布同步

普通版本的 `app-editors/musicmapmaker-next` 从 `refs/tags/v${PV}` 获取源码；`9999` 保留为跟随 `main` 的开发包。稳定配方继承 Clang、PGO 和关闭编译耗时诊断的构建选项。每次发布新增对应版本文件，保留历史版本，不把一个旧 ebuild 改成指向另一个 tag。

`mmm-github-release` 在公开正式 Release 成功后调用 `scripts/ci/update-gentoo-overlay.sh`。同步前检查 tag 是最新公开正式版本，并解引用 tag 核对发布源码 SHA；草稿与预发布不更新稳定配方。随后克隆 overlay 的最新 `main`，调用该仓 `scripts/update-source-release.py`，检查 ebuild 语法，提交并普通推送。发生并发冲突时失败，不强推。

跨仓克隆和推送统一使用 HTTPS，沿用自托管 CI Docker 环境已经配置的 Git 凭据，不再需要 `GENTOO_OVERLAY_SSH_KEY`。脚本不覆盖 credential helper，不把凭据写入远程 URL、文件或日志；主仓 `GITHUB_TOKEN` 仍仅用于主仓 API。正式发布、独立 overlay 恢复和热修复包恢复均在同一自托管 Runner 环境执行。

如果 Release 已成功但 overlay 同步失败，运行 `mmm-gentoo-overlay-sync`，传入该正式 tag。这个入口不会重新创建 Release、改写 tag 或部署网站；已存在的同版本 ebuild 不产生重复提交。过渡期可从包含本工作流的 `develop` ref 运行恢复入口，0.6.3 之后遵循主仓新工作流。

源码包仍需使用系统工具链进行编译，因此只保证源码 tag 对应发布版本，不保证与网站预编译二进制逐字节相同。PGO 数据在每次构建时实时下载，也会影响最终二进制。完整 Gentoo 构建验收应由目标系统执行。
