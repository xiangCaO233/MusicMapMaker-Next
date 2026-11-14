# debsrv Android ARM64 CI

现有 `Self Hosted CI Runner` 的人工任务新增 `android-build`，仅在 Debian Runner 上生成 ARM64 QA APK。普通 `ci-build` 也调用相同 Android 工作流；hotfix、网站清单与正式 Release 的发布范围保持原样。

## 固定环境与目录

宿主 `xiang@192.168.31.12` 的 `github-runner-mmm` 容器已经挂载 `/home/xiang/cross`，不需要另开模拟器或修改容器挂载。Android 状态全部集中于 `/home/xiang/cross/android`：

| 路径 | 用途 |
| --- | --- |
| `sdk` | Android SDK 36.1、Build Tools 36.1.0、NDK 30.0.16248370、命令行工具 22.0 |
| `jdk-21` | Eclipse Temurin 21.0.13+9 完整 JDK，仅本任务使用 |
| `checkout` | 独立源码检出，不争用桌面 CI 工作树 |
| `work/build/arm64-v8a` | CMake、原生依赖和未剥离的调试符号 |
| `work/cache` | Gradle、下载与临时文件 |
| `work/apk/ci` | 本轮 QA APK、SHA256、build-info.json |
| `secrets/qa` | 仅 QA 测试签名，目录 700、文件 600，不上传 |

SDK/NDK 来自开发机已安装的官方 Linux x86_64 SDK 软件包，按固定版本复制，不复制 Gentoo 编译的库。JDK 来自 Adoptium 官方归档 `OpenJDK21U-jdk_x64_linux_hotspot_21.0.13_9.tar.gz`，SHA256 为 `d7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c`。

重建 SDK 环境时用固定版本的 sdkmanager 安装 `platforms;android-36.1`、`build-tools;36.1.0`、`ndk;30.0.16248370` 并确认许可证；无需下载 emulator 或 system-images。宿主还需现有 CMake 3.31+、Ninja、make、Meson、Python3、Git LFS、curl 和 pkg-config。安装 SDK/JDK 不改变容器或宿主默认 Java。

## 构建与交付

人工执行：

```sh
gh workflow run ci.yaml --ref feat/android-sdl3 -f job=android-build -f pgo_instrument=false
```

入口将事件精确 SHA 传给可复用工作流，整段源码同步与构建持有 `build.lock`。子模块按 gitlink 检出，只显式拉取 `assets/**` 的 NAS LFS 内容。原生库使用 NDK 官方 CMake toolchain，目标 `arm64-v8a`、最低 API 28、`c++_shared`、`RelWithDebInfo`，业务和依赖均不启用 PGO。默认并发 6，避免与服务器其它任务争用内存。

Gradle Wrapper 9.3.0 与 AGP 9.0.1 仅打包 Java 外壳、默认资源和匹配的 JNI 库。发布型 QA 包显式关闭 Java/native 调试开关，原生符号留在集中构建树。默认本机构建仍为 debug、可选双 ABI；CI 显式选择 release 和单 ARM64，不修改开发者的默认行为。

QA 使用既有包名 `top.xiang233.musicmapmaker.next.qa` 和 `mmm-qa` 测试证书。私钥与密码文件由自托管宿主提供；缺失时失败，不退回 debug 或 unsigned 包。版本名包含 APPVER 和源码短 SHA，版本码使用 `2026111404 + GITHUB_RUN_NUMBER`，高于此前 QA 包，超过 Android 上限时明确拒绝。

上传前从实际 APK 检查包名、版本码、非 debuggable、唯一 ARM64 目录、ELF AArch64、ZIP CRC 和签名。GitHub Artifact 仅保留 3 天，APK 不再二次压缩；不上传依赖缓存、私钥或原生符号，也不会创建 Release 或更新网站。运行成功仍不代替手机运行验收。
