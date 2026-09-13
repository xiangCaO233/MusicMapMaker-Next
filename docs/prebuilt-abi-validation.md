# 预编译库配套检查（2026-09-14）

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
