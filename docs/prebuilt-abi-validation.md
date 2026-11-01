# 预编译库配套检查（2026-11-01）

## 当前音频引擎包

- 源码为 `IonCachyEngine` 的 `8c15136af7eda2a7b1374371bf6b19d6e72fcb6a`，包含 SDL 按需回调供数；主仓基线为 `78cbd10d`。
- Linux 与 Windows 均在 debsrv 的既有 `github-runner-mmm:trixie` 容器中构建独立源码快照，不改 Runner 原工作区分支。
- Linux GCC 14、Clang 19；Windows MSVC ABI/LLVM 22、MinGW clang64、MinGW ucrt64，全部包含 Debug 与 RelWithDebInfo；同时更新原有 MSVC 动态库、导入库和 PDB。
- macOS ARM64 在原生 Mac Runner 使用 AppleClang 17、Xcode 26.1 SDK、macOS 11.0 部署下限构建 Debug 与 RelWithDebInfo。
- 共完成 14 组构建，带回 18 个二进制文件；公开头与同一源码快照逐字节一致，修正 `AudioBuffer.hpp` 和 `SDLPlayer.hpp` 的旧副本。
- 引擎本体从源码编译，第三方依赖显式使用 `SOURCES_BUILD=OFF` 的既有对应平台包；未进行 PGO 插桩或 strip。原生静态库调试段保留，两个 Windows DLL 与对应 PDB 的 GUID 匹配。
- 四组 Linux、两组 macOS 引擎实时回归测试通过；Windows 示例与测试程序完成交叉链接，没有宣称 Windows 原生运行验证。
- macOS 独立示例消费方显式链接 IOKit、CoreServices、Foundation；这只补齐独立验证程序的系统框架闭包，不修改引擎或第三方源码。
- 本机主项目预编译模式完整构建成功，最终 CTest 为 179/179；音频集成用例首轮出现一次共享拉伸器/节拍路由时序失败，随后单独连续三次与最终全量均通过，没有调整断言。
- 校验了带回产物的 SHA256、归档结构、静态库调试段、公开头一致性及 Windows DLL/PDB 配套。详细本机证据集中于 `build/ice-prebuilt-update-20261101/`。
- 同步公开头文件不保留旧修改时间，消费方 ABI 指纹改变后已触发相关翻译单元重编。

## 历史兼容性记录（2026-09-14）

## 已修复

- Linux ICE 的 GCC 14、Clang 19，及 Windows MSVC 2026 的静态、动态库缺少当前公开头文件要求的 `ThreadPool(int)` 符号。
- 在 debsrv 既有 `github-runner-mmm` 容器、既有仓库当前分支中，从 ICE 提交 `3fe3b1056bf79f8a3fff9f84ddd477d44e5c3889` 重编；没有新建仓库或工作树。
- 每组均生成 Debug、RelWithDebInfo。Linux 使用 GCC 14.2 / Clang 19.1.7；MSVC ABI 使用 clang-cl 22.1.8。
- 引擎独立构建使用 `SOURCES_BUILD=OFF`，仅重编自维护 ICE；第三方依赖仍使用对应预编译包，未开启 PGO。
- 静态库保留嵌入调试信息；Windows 动态库同时更新 DLL、导入库和 PDB。
- 本机仍选择 `PROJECT_PREBUILT_COMPILER_TAG=clang19`，不提交临时 clang23 产物。
- debsrv 的旧公共 ICE 头文件也已同步；源码头文件与本机公共头文件一致，无须重复修改本机头文件。

## 验证范围与边界

- 主仓及 ICE 内部预编译目录的 648 个 `.a` / `.lib` 均通过归档目录读取检查。这只能排除损坏或未拉取的 LFS 指针，不能证明所有 ABI 一致。
- Linux、MinGW、MSVC、macOS ARM 的 ImGui 静态库内嵌版本均为 `1.92.9`，与公共头文件一致；主程序已有 `IMGUI_CHECKVERSION()` 布局检查。
- MinGW 和 macOS ARM 的 ICE 静态库含所需线程池构造符号，本次未改动；未进行这些平台的原生运行验证。
- Linux 四组修复库均通过 ICE 自带实时测试，以及使用公共头文件单独编译的 `AudioEngineAbiTest` 消费方检查。
- MSVC 两组静态库完成示例和测试程序链接；动态库检查了线程池构造导出。未在 Windows 原生系统运行。
- 本机完整构建成功，链接命令确认选择 clang19 ICE；CTest 为 129/131 通过。原有失败为 `NoteOverlapMaskRenderTest`、`MarkdownRendererTest`，未在本次扩大修复范围。
- 新增 `AudioEngineAbiTest` 不打开声卡，覆盖线程池构造、模板入队、混音总线构造、缓冲区布局和静音处理。它是启动契约回归测试，不是所有依赖的完整 ABI 证明。

## 后续更新约束

同步头文件时不保留旧修改时间，确保消费方增量构建重新编译；不可只替换二进制后沿用旧对象文件。发布前同时检查当前公共头文件、最终链接所选库和实际消费方测试。

## 2026-09-25 Windows 音轨布局回归

- 崩溃 dump 与 `build_cross_msvc/bin/MusicMapMaker-Next.pdb` 的 GUID 完全匹配；启动预加载皮肤音效时，旧 `AudioTimelineMixerNode.cpp.obj` 把 `AudioTrack` 的 `+0xb0` 当成解码器指针，读到了字符串容量 `0x5f`，触发访问冲突。当前 ICE 库实际把解码器放在 `+0xb8`。
- 出问题的消费方对象生成于 9 月 11 日，ICE 库于 9 月 14 日更新；公共头文件保留了更早的修改时间，增量构建只重新链接，未重新编译该对象。
- `FindIonCachyEngine.cmake` 现在将公开头文件及所选库的内容指纹放入消费方编译定义。重新配置后，输入内容变化会改变编译命令并清除旧布局对象；首次启用也会重编旧构建树。
- `AudioEngineAbiTest` 增加内存音轨的构造与消费方读取，覆盖 `MediaInfo`、`AlbumArt` 和 `AudioTrack` 的布局边界。预编译包同步仍不得保留旧修改时间；外部工具若保留时间戳，应先显式重新配置。
