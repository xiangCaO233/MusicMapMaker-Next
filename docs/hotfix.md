# 热修复与资源热更新

这套逻辑随 0.6.3 正式版本发布。0.6.2 到 0.6.3 仍使用原有三段版本更新；0.6.3 起程序支持同版本内递增的 `hotfix_revision`。程序更新需要重启，资产在启动同步时按 SHA256 下载差异文件。

## 分支与基线

实现提交到 `develop`，本批不推送维护分支或改动线上发布。0.6.3 发布并建立 `v0.6.3` tag 后，从该 tag 建立持久 `hotfix` 分支。

首个维护提交必须更新 `hotfix-base.json`，将 `tag` 设置为 `v0.6.3`，`commit` 设置为 `git rev-parse v0.6.3^{commit}` 的结果。当前记录 v0.6.2 仅是启动前的占位基线，程序发布入口会拒绝它。以后迁移到新正式版本时同样重新从新 tag 建立分支并在第一个维护提交更新基线。

CI 检查固定 tag 对象、源码 APPVER、第一父链及首个维护提交的声明；更高正式 tag 成为祖先时拒绝执行。人工发布还检查 GitHub 上存在已发布的稳定 Release。普通功能合并可以继续使用，不要求线性历史。Git 不保存分支创建事件，因此这是一份可验证的初始化契约，不能证明历史上某个分支名称何时创建。

## GitHub 人工入口

打开 **Self Hosted CI Runner → Run workflow**：

- 分支选择 `hotfix`。
- `job` 选择 `hotfix`。
- `hotfix_scope` 选择 `program`、`assets` 或 `both`。
- `hotfix_revision` 填严格递增的正整数，最大 2147483647。
- 更新资产时，在 `asset_paths` 每行填写一个相对 `assets` 的文件或目录；单独填写 `*` 表示全部。未选资产保持当前线上内容。

每次发布都从当前线上清单建立候选。程序和资产分别检查修订是否递增；组合发布的数值必须同时高于二者。没有内容变化的资产选择会被拒绝，不产生空修订。第一版不处理资源删除。

仅程序或组合发布会复用原跨平台构建、CTest 和打包。聚合后按运行 ID 保存不可变快照，发布器核对提交及编译修订，不能消费旧 `release` 缓存。仅资产发布不需要编译。

## 发布与客户端

CI 在 debsrv 准备候选，通过 SSH 将文件送到 podman 网站服务器。R2 凭据始终保留在网站服务器。对象存储在 `download/hotfix/program/<version>/r<N>/` 或 `download/hotfix/assets/<version>/r<N>/`，同名不同内容不得覆盖。

所有候选先验证，再上传并核对 R2 正文和公开 CDN 正文，最后切换现有 `download/check/check.json` 与 `download/assets/manifest.json`。仅选择的清单会改变；组合发布先切资源清单，再切程序清单。两个远端 JSON 不能跨 CDN 原子切换，因此资产必须兼容同一正式版本的旧修订；不兼容的程序和资源变更应使用正式版本发布。

程序清单保持 `version`，增加 `hotfix_revision`、`hotfix_base_tag`、`hotfix_commit`。客户端先比较正式三段版本，再比较同基线修订号；旧响应按零修订处理。正式版本升级后，上一正式版本遗留的扩展字段不再参与比较。更新弹窗显示 `v0.6.3-hotfix.1`，沿用现有下载和重启安装入口。

资源仍使用 schema 1 的完整清单，只替换选定路径。候选同时重建完整包，使首次安装与逐文件更新获得一致内容；未选择资源继续使用原清单条目。资源版本使用 `<base>-assets.<revision>`，不会被客户端的相同版本快速路径跳过。

切换前检查原清单摘要，发现并发发布则要求重新准备。原清单保留摘要命名的 `.backup` 副本；公开 CDN 已成功切换后本地入口失败的情况需人工恢复一致性。此流程不承诺新程序启动后的自动回滚，安装仍沿用现有独立更新器行为。

## CI 文件职责

| 文件 | 职责 |
| --- | --- |
| `.github/workflows/ci.yaml` | 保留原任务菜单，增加 hotfix 选项 |
| `.github/workflows/build.yaml` | 原跨平台构建、测试、打包、聚合 |
| `.github/workflows/website-release.yaml` | 原完整网站与 APT 发布 |
| `.github/workflows/hotfix-release.yaml` | 基线检查、范围构建、准备及发布 |
| `.github/workflows/hotfix.yaml` | hotfix 分支 push 的离线验证 |

完整构建使用共享并发组保护固定工作树；普通网站发布和热修复使用另一共享并发组保护清单切换。普通 `ci-build`、ci push 和 `website-release` 的步骤与产物路径保留。首次启用前应将这批工作流随正式版本合入默认分支，使 GitHub 页面能够发现人工入口。

## 验证入口

```text
python -m unittest discover -s scripts/hotfix -p "test_*.py" -v
cmake --build build
ctest --test-dir build --output-on-failure -R "^(UpdateCheckerTest|AssetSyncServiceTest)$"
```

离线测试覆盖资源部分覆盖与完整包、制品篡改、基线变化、程序聚合身份、0.6.2 禁止程序热修复，以及正常合并和错误分支来源。GitHub 和服务器的实际公开发布留待 v0.6.3 建立 tag 后从维护分支执行。

## 本批注释率检查

2026-10-12，按 cloc 逐文件计算；纯数据基线 JSON 和文档不参与。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Network/src/UpdateChecker.cpp` | 368 | 857 | 30.04% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/help/CheckUpdateAction.cpp` | 212 | 493 | 30.07% |
| `Modules/Network/tests/UpdateCheckerTest.cpp` | 152 | 351 | 30.22% |
| `.github/workflows/build.yaml` | 100 | 233 | 30.03% |
| `cmake/project-configure.cmake` | 83 | 188 | 30.63% |
| `scripts/hotfix/publish_assets.py` | 58 | 135 | 30.05% |
| `.github/workflows/hotfix-release.yaml` | 57 | 133 | 30.00% |
| `scripts/hotfix/prepare_assets.py` | 57 | 131 | 30.32% |
| `scripts/hotfix/prepare_program.py` | 39 | 81 | 32.50% |
| `Modules/Network/include/network/UpdateChecker.h` | 44 | 75 | 36.97% |
| `scripts/hotfix/test_assets.py` | 30 | 68 | 30.61% |
| `scripts/hotfix/validate_base.py` | 28 | 65 | 30.11% |
| `scripts/hotfix/test_program.py` | 25 | 56 | 30.86% |
| `scripts/hotfix/test_base.py` | 24 | 54 | 30.77% |
| `.github/workflows/ci.yaml` | 23 | 52 | 30.67% |
| `.github/workflows/website-release.yaml` | 24 | 49 | 32.88% |
| `.github/workflows/hotfix.yaml` | 9 | 19 | 32.14% |
| `.github/actionlint.yaml` | 3 | 4 | 42.86% |
| `cmake/mmmversion.h.in`（按 C++ 模板统计） | 16 | 10 | 61.54% |
