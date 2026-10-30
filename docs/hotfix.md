# 热修复与资源热更新

## v0.6.4-hotfix.2 候选（尚未发布）

- 基线沿用 `v0.6.4`，在 `hotfix.1` 维护代码上修复，程序修订为 **2**，无需资源更新。
- 修复含函数或预设插值 timing 的 MMM 导出 MC/MCZ 时，折线子长条与滑键被重复写为独立物件的问题。秒域和拍域均重建副本内部引用，保留原插值精度。
- 打包候选跳过项目内部 `.mmm` 目录，实际打包命令也拒绝夹带自动备份，普通 `chart.mmm` 仍可正常转换。
- 本机验证构建使用 `MMM_HOTFIX_REVISION=2`。后续人工发布选择 `hotfix_scope=program`、`hotfix_revision=2`，`asset_paths` 留空；此候选记录不创建发布 tag，也不代表已经上线。
- 验证：修复前程序化 MMM 的 4 个顶层物件导出后变为 10 个；修复后秒域/拍域、三种曲线位置、预设/自定义函数共 12 组场景均保持原始 `note[]`。真实 MCZ 打包解包检查连接及内部备份拒绝，完整构建与 CTest **175/175** 通过；本批代码逐文件注释率均不低于 30%。未执行外部 MA 客户端运行测试。

这套逻辑随 0.6.3 正式版本发布。0.6.2 到 0.6.3 仍使用原有三段版本更新；0.6.3 起程序支持同版本内递增的 `hotfix_revision`。程序更新需要重启，资产在启动同步时按 SHA256 下载差异文件。

## 包管理器热修复同步

`program` / `both` 模式在网页程序发布成功后同步签名 APT 源和 Gentoo overlay；`assets` 模式沿用应用的资源更新，不重建程序包。软件源同步失败会使整个发布任务失败，不能只因网页已切换就宣称完整发布成功。

DEB 包保持应用版本不变，以 `0.6.3-2+hotfix.1`、`0.6.3-2+hotfix.2` 递增。包内主 ELF 必须与网页发布的 Linux 程序摘要一致，源码提交、包内记录、控制字段和文件名也必须匹配本次修订。包从原 CI run 的不可变聚合快照取出并随 hotfix Artifact 保存，不从共享 release 缓存猜测。

Gentoo 源码新增 `musicmapmaker-next-0.6.3-r1.ebuild`，固定到不可变的 `v0.6.3-hotfix.1` tag，并在配置时写入相同应用修订。Gentoo 二进制包新增 `musicmapmaker-next-bin-0.6.3-r2.ebuild`（原正式二进制为 r1），绑定同一热修复 DEB 的固定 CDN 地址和 Manifest 摘要。标签只允许新建或复用同 SHA，不移动正式 tag，也不另建正式 Release。

如果网页程序已公开，但 APT 或 Gentoo 同步失败，运行 `mmm-hotfix-package-sync`，输入原 `ci.yaml` hotfix 发布 run ID。恢复读取原 Artifact，检查本仓 hotfix 人工运行身份和精确 SHA，再对照当前公开清单与 DEB 载荷，只重试两个软件源；纯资产、未公开候选和已过期修订不能通过恢复。Artifact 过期或丢失时不得用重新编译的包冒充原发布对象。

## 分支与基线

程序热修复从当前已发布稳定版本的不可变 tag 建立持久 `hotfix` 分支，修复及测试先在维护分支完成，再按 `AGENTS.md` §7.4 及时前向移植到主线 PR。维护分支不从任意开发分支 HEAD 初始化。

tag 后首个维护提交必须更新 `hotfix-base.json`，将 `tag` 设置为该正式版本 tag，`commit` 设置为 `git rev-parse <tag>^{commit}` 的精确结果。迁移到新正式版本时重新从新 tag 建立分支并在第一个维护提交声明基线；程序发布入口拒绝不支持同版本修订的 v0.6.2。

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

## 传输与断连恢复

仅更新程序时选择 `hotfix_scope=program`，`asset_paths` 留空；`both` 和 `assets` 都要求显式选择资源。没有发布成功的修订可以用原构建产物恢复，不能通过更改摘要或覆盖同修订对象绕过检查。

候选先复制到网站服务器并完整校验。服务器只给尚不存在的对象签发三十分钟有效的单对象 PUT 地址，绑定长度及 SHA256 元数据；Runner 从同一次构建的候选直接上传 R2，避开网站服务器的慢速上传出口。长期 R2 密钥不离开网站服务器，限时许可只写 Runner 私有临时文件，不进入日志或 Artifact，并在该步骤退出时删除。

SSH/SCP 设置连接保活及超时，远端发布进程最多运行 45 分钟。日志区分上传、复用、R2 正文回读、公开 CDN 校验与清单切换；已有同摘要对象恢复时不重传，但仍须完整验收。上传不等于发布完成，固定清单必须最后切换。

SSH 断连后先核对服务器上原进程与线上清单，处理原进程后再恢复同一运行的候选，避免两个发布进程并行。程序已经公开但软件源未完成时，使用前述 `mmm-hotfix-package-sync` 消费原 run 的不可变 Artifact；无需重新构建另一份程序。

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
| `.github/workflows/hotfix-packages.yaml` | 已公开程序的 APT/Gentoo 同步恢复 |

完整构建使用共享并发组保护固定工作树；普通网站发布和热修复使用另一共享并发组保护清单切换。普通 `ci-build`、ci push 和 `website-release` 的步骤与产物路径保留。首次启用前应将这批工作流随正式版本合入默认分支，使 GitHub 页面能够发现人工入口。

## 验证入口

```text
python -m unittest discover -s scripts/hotfix -p "test_*.py" -v
cmake --build build
ctest --test-dir build --output-on-failure -R "^(UpdateCheckerTest|AssetSyncServiceTest)$"
```

离线测试覆盖资源部分覆盖与完整包、制品篡改、基线变化、程序聚合身份、0.6.2 禁止程序热修复，以及正常合并和错误分支来源。GitHub 和服务器的实际公开发布留待 v0.6.3 建立 tag 后从维护分支执行。

## 最近修改文件的注释率检查

2026-10-13，仅统计本批修改的自维护代码，按 cloc 逐文件计算；文档不参与。热修复 unittest 24 项通过，本机构建通过；Linux 隔离小程序完成正式版、hotfix 1、hotfix 2 的真实 CPack DEB 打包、载荷校验和 dpkg 升级顺序检查。原 Debian 维护测试 13 项和 APT 发布测试 7 项通过；APT 索引集成测试因隔离容器缺少 apt-ftparchive 跳过。Gentoo 配方与 Manifest 的生成、幂等恢复和回退拒绝检查通过；完整 Gentoo 安装未执行。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `.github/workflows/hotfix-release.yaml` | 74 | 171 | 30.20% |
| `.github/workflows/hotfix-packages.yaml` | 27 | 63 | 30.00% |
| `.github/workflows/ci.yaml` | 24 | 56 | 30.00% |
| `cmake/linux-package.cmake` | 69 | 160 | 30.13% |
| `scripts/ci/update-gentoo-overlay.sh` | 37 | 85 | 30.33% |
| `scripts/hotfix/validate_deb.py` | 31 | 71 | 30.39% |
| `scripts/hotfix/sync_packages.py` | 13 | 29 | 30.95% |
| `scripts/hotfix/test_packages.py` | 32 | 73 | 30.48% |
| `C:/coding/mmm-gentoo-overlay/scripts/update-source-release.py` | 40 | 86 | 31.75% |
