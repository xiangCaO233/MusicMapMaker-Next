# debsrv Android ARM64 CI

现有 `Self Hosted CI Runner` 的人工任务新增 `android-build`，仅在 Debian Runner 上生成 ARM64 发布型 APK。普通 `ci-build` 也调用相同 Android 工作流，并把同源 APK 聚合进正式候选；旧版 hotfix 继续原平台集合。构建成功不自动公开发布。

## 固定环境与目录

宿主 `xiang@192.168.31.12` 的 `github-runner-mmm` 容器已经挂载 `/home/xiang/cross`，不需要另开模拟器或修改容器挂载。Android 状态全部集中于 `/home/xiang/cross/android`：

| 路径 | 用途 |
| --- | --- |
| `sdk` | Android SDK 36.1、Build Tools 36.1.0、NDK 30.0.16248370、命令行工具 22.0 |
| `jdk-21` | Eclipse Temurin 21.0.13+9 完整 JDK，仅本任务使用 |
| `checkout` | 独立源码检出，不争用桌面 CI 工作树 |
| `work/build/arm64-v8a` | CMake、原生依赖和未剥离的调试符号 |
| `work/cache` | Gradle、下载与临时文件 |
| `work/apk/ci` | 本轮发布型 APK、SHA256、build-info.json |
| `secrets/qa` | 仅 QA 测试签名，目录 700、文件 600，不上传 |

SDK/NDK 来自开发机已安装的官方 Linux x86_64 SDK 软件包，按固定版本复制，不复制 Gentoo 编译的库。JDK 来自 Adoptium 官方归档 `OpenJDK21U-jdk_x64_linux_hotspot_21.0.13_9.tar.gz`，SHA256 为 `d7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c`。

重建 SDK 环境时用固定版本的 sdkmanager 安装 `platforms;android-36.1`、`build-tools;36.1.0`、`ndk;30.0.16248370` 并确认许可证；无需下载 emulator 或 system-images。宿主还需现有 CMake 3.31+、Ninja、make、Meson、Python3、Git LFS、curl 和 pkg-config。安装 SDK/JDK 不改变容器或宿主默认 Java。

## 构建与交付

人工执行：

```sh
gh workflow run ci.yaml --ref feat/android-sdl3 -f job=android-build -f pgo_instrument=false
```

入口将事件精确 SHA 传给可复用工作流，整段源码同步与构建持有 `build.lock`。子模块按 gitlink 检出，只显式拉取 `assets/**` 的 NAS LFS 内容。原生库使用 NDK 官方 CMake toolchain，目标 `arm64-v8a`、最低 API 28、`c++_shared`、`RelWithDebInfo`。业务默认启用 PGO 插桩，依赖继续禁止插桩。默认并发 6，避免与服务器其它任务争用内存。

Gradle Wrapper 9.3.0 与 AGP 9.0.1 仅打包 Java 外壳、图标和匹配的 JNI 库。发布型包显式关闭 Java/native 调试开关，原生符号留在集中构建树。默认本机构建仍为 debug、可选双 ABI；CI 显式选择 release 和单 ARM64，不修改开发者的默认行为。

安装身份沿用既有包名 `top.xiang233.musicmapmaker.next.qa` 和 `mmm-qa` 测试证书。私钥与密码文件由自托管宿主提供；缺失时失败，不退回 debug 或 unsigned 包。版本名与桌面一样使用 APPVER，精确源码 SHA 保存在 build-info.json 中；版本码使用 `2026111404 + GITHUB_RUN_NUMBER`，高于此前 QA 包，超过 Android 上限时明确拒绝。

上传前从实际 APK 检查包名、版本码、非 debuggable、唯一 ARM64 目录、ELF AArch64、ZIP CRC 和签名。GitHub Artifact 仅保留 3 天，APK 不再二次压缩；不上传依赖缓存、私钥或原生符号，也不会创建 Release 或更新网站。运行成功仍不代替手机运行验收。

## 交付名称

安装包统一为 `MusicMapMaker-Next-<APPVER>-android-arm64.apk`，例如 `MusicMapMaker-Next-0.6.4-android-arm64.apk`；GitHub Artifact 使用相同名称去掉 `.apk`。名称顺序与其他平台的产品名、版本、平台、架构保持一致，不再添加 `qa` 或源码短 SHA。SHA256 文件引用实际 APK 文件名，源码完整 SHA 与运行链接保存在 `build-info.json`。

历史包名与签名接口中的 `qa` 保留用于兼容已有安装；这不再影响交付名称。此次不更换证书或 Android 应用身份，以便已有安装继续覆盖升级。

## 正式发布链路

完整 CI 通过 `stage_release=true` 将已验证交付放到主仓 `.git/mmm-ci-transfer/<run_id>/android-arm64`，聚合 job 等待桌面、macOS 与 Android 均成功，再原子替换 `release/android-arm64`。旧版 hotfix 显式跳过 Android 依赖；macOS 聚合不能提前删除共用传输根。人工单独 Android 构建只上传 Artifact，不留下未消费的跨 job staging。

`scripts/ci/android-release.py` 是聚合与发布共用入口，验证完整来源 SHA、版本、发布变体、大小、SHA256、ZIP CRC 与实际 AArch64 JNI。签名仍在 Android 构建入口验证，摘要将后续交付绑定到同一份已签名文件。GitHub 正式发布将原 APK 直接作为附件，并写入 `SHA256SUMS`；网站发布向 `--android-apk` 传入同一候选，不重新构建或重签。

网站只根据正式版 `platforms.android` 启用下载。当前 v0.6.4 hotfix.3 不加入新分支的 APK；下一次正式发布时 tag、源码、桌面与 Android 候选必须同源。网站稳定路径、版本化保存名、R2 MIME、摘要缓存键和公开 APK 完整回读由网站发布脚本维护，未提供 APK 的旧版本不生成下载链接。

可在集中输出目录执行 `python3 scripts/ci/test_android_release.py`，测试包括实际 `publish-github-release.sh --package-only` 打包流程，不创建 tag 或公开 Release。

## 启动下载资源

APK 不再内置 `mmm-assets.zip` 或随包摘要，Gradle 显式不引用旧的资源打包目录。首次启动复用桌面的 HTTPS 清单、完整包下载、SHA256 校验及进度／失败重试界面，资源写入当前应用私有配置目录；系统字体负责尚未下载字体时的中文提示。

已有完整资源时，断网仍可按桌面服务的回退策略进入编辑器。旧版 APK 解压的资源、配置和工程保持原路径，首次升级可离线复用；联网后按正式资源清单检查更新，不从旧随包标记推断新版本。清空应用数据或卸载后需要重新下载资源；APK 自身升级不清除这些文件。

构建交付校验会拒绝含旧完整 ZIP 或旧随包摘要的 APK，防止复用构建缓存时再次变成大包。`package_assets.py` 保留作旧包复现工具，常规 Linux／Windows 构建入口均不再调用。

开发分支首次下载仍读取公开的正式资源清单，可能暂时缺少尚未发布的移动端译文。正式发布时必须继续让同源 APK 与资源包一起进入发布候选并同步到 R2，不能只替换 APK 而沿用旧资源清单。

## PGO 采集

默认使用 `RelWithDebInfo` 采集业务模块计数器；Linux 本地可用 `MMM_ANDROID_PGO_INSTRUMENT=OFF` 关闭，Windows 使用 `-PgoInstrument OFF`。单独 Android 与完整 CI 都继承主入口的 `pgo_instrument` 布尔选择，交付元数据记录实际原生配置。第三方依赖和业务目标中编译的上游 FreeType loader 均不插桩。

采集包沿用应用内上传授权，正常退出时写出私有缓存中的 `.profraw`；用户同意且实际运行达到 600 秒才上传到既有服务。系统强杀或划掉进程不保证执行正常退出上传。`MMM_PGO_USE` 与插桩互斥，采集包保持其为 OFF；使用已采集样本优化应另建构建树，并使用匹配 Android ABI 与 NDK LLVM 的数据。

本地原始采样总量超过 100 MiB 时按修改时间淘汰最旧文件，上传中的当前采样暂时保护，结束后继续清理。规则覆盖旧版本遗留数据，详见 [本地 PGO 缓存](pgo-cache.md)。
