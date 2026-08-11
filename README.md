<div align="center">
  <img src="Modules/Main/src/logo.svg" width="160" alt="MusicMapMaker-Next 图标 / MusicMapMaker-Next icon">
  <h1>MusicMapMaker-Next</h1>
  <p><strong>面向音乐游戏的现代跨平台谱面编辑器</strong><br><strong>A modern cross-platform chart editor for rhythm games</strong></p>
  <p>
    <a href="https://mmm.xiang233.top">官方网站 / Website</a> ·
    <a href="https://mmm.xiang233.top/download">官方下载 / Download</a> ·
    <a href="https://feedback.xiang233.top">反馈主页 / Feedback</a> ·
    <a href="https://github.com/xiangCaO233/MusicMapMaker-Next">源代码 / Source</a>
  </p>
  <p>
    <a href="#简体中文">简体中文</a> ·
    <a href="#english">English</a>
  </p>
</div>

## 预览 / Preview

| 默认暗色主题 / Default dark theme | 默认亮色主题 / Default light theme |
| --- | --- |
| ![MusicMapMaker-Next 默认暗色主题](docs/images/preview-dark.png) | ![MusicMapMaker-Next 默认亮色主题](docs/images/preview-light.png) |

---

## 简体中文

### 下载与安装

Windows 与 Linux 正式产物是单可执行文件：下载后可放在任意目录直接运行，无需安装程序。首次启动会在用户目录下的 `.config/mmm` 创建配置、日志和自动同步的运行资源，因此移动主程序不会丢失用户配置。macOS 遵循平台分发规则，提供 DMG 和 `.app` ZIP，而不是裸 Mach-O 文件。

- [官方网站](https://mmm.xiang233.top)：项目主页与最新版本信息。
- [官方下载页](https://mmm.xiang233.top/download)：Windows、Linux 和 macOS 正式产物直达下载。
- [反馈主页](https://feedback.xiang233.top)：问题反馈与建议入口。
- [GitHub 仓库](https://github.com/xiangCaO233/MusicMapMaker-Next)：源代码、提交记录与开发进度。

#### 我该下载哪个版本？

各版本功能相同，区别主要在目标平台、ABI、C/C++ 运行时和编译器优化器。发布构建统一使用 `RelWithDebInfo`：开启发布级优化并保留调试符号；它不是 `Debug` 构建。编译器之间不存在对所有谱面和硬件都成立的固定性能排名。

| 产物目录 | 编译器与 ABI | 优化程度与用途 | 建议 |
| --- | --- | --- | --- |
| `windows-msvc-clang` | LLVM clang-cl 22，MSVC 14.51 / Windows SDK 10.0.26100 ABI | `RelWithDebInfo`，PGO 关闭；兼容 MSVC 生态，官网 Windows 默认产物 | 大多数 Windows 用户首选 |
| `windows-mingw-gcc` | MinGW-w64 UCRT，GCC 14 系列 | `RelWithDebInfo`，PGO 关闭；GNU 工具链替代版 | 需要 GCC/UCRT 兼容性时选择 |
| `windows-mingw-clang` | MinGW-w64 UCRT，LLVM/Clang 22，`clang64` 布局 | `RelWithDebInfo`，PGO 关闭；LLVM MinGW 替代版 | 需要 LLVM MinGW ABI 时选择 |
| `linux-gcc14` | GCC 14，GNU/Linux x86_64 | `RelWithDebInfo`，PGO 关闭；官网 Linux 默认产物 | 大多数 Linux 用户首选 |
| `linux-clang19` | Clang 19，GNU/Linux x86_64 | `RelWithDebInfo`，当前开启 PGO **插桩采集**，并未使用 profile 做最终优化，运行会有采集开销 | 用于 LLVM 兼容性测试和 PGO 数据采集 |
| `macos-arm64` | AppleClang，ARM64；当前预编译依赖标签为 `clang17` | `RelWithDebInfo`，PGO 关闭；DMG 与 `.app` ZIP | Apple Silicon Mac 用户 |

本地 `scripts/ci/msvc-build.ps1` 使用当前 Visual Studio 环境中的原生 `cl.exe`，版本由本机决定；它消费 `msvc/2026` 预编译依赖、使用 `RelWithDebInfo` 并关闭 PGO，不等同于 CI 中在 Linux 上交叉编译的 `windows-msvc-clang` 产物。

编译器与构建工具官方下载入口：

- [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
- [LLVM / Clang Releases](https://github.com/llvm/llvm-project/releases)
- [GCC](https://gcc.gnu.org/)
- [MSYS2](https://www.msys2.org/)（MinGW-w64 UCRT/CLANG64 环境）
- [Xcode](https://developer.apple.com/xcode/)（AppleClang）
- [CMake 下载](https://cmake.org/download/)、[Ninja](https://ninja-build.org/)、[Git LFS](https://git-lfs.com/)
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)

### 分支与产物

构建脚本始终构建当前检出的提交。分支决定代码内容和 CI 触发方式，不会暗中替换编译器。

| 分支 | 用途 | 产物说明 |
| --- | --- | --- |
| `main` | 默认公开分支，保存稳定基线 | 从该分支手动运行脚本时，产物严格对应当前 `main` 提交；普通 push 不触发仓库当前的完整发布矩阵 |
| `develop` | 日常开发与集成分支 | 包含即将合入稳定线的变化；适合本地验证，稳定性可能低于 `main` |
| `ci` | 连续构建与发布编排分支 | push 会触发 Windows 三套、Linux 两套和 macOS ARM64 构建，并暂存到 `release/<产物目录>`；网站发布仍由 `workflow_dispatch` 的 `website-release` 任务显式执行 |

原生 MSVC 脚本已不再 `fetch`、`checkout ci` 或 `reset --hard`；无论当前位于 `main`、`develop`、功能分支还是某个 detached commit，都会原样构建当前工作树。

### 按需构建

仓库中的预编译依赖由 Git LFS 管理。为了避免克隆时下载所有平台、所有编译器的二进制，先使用 `GIT_LFS_SKIP_SMUDGE=1` 克隆，再只拉取目标工具链对应的对象。`SOURCES_BUILD` 默认且必须保持为 `OFF`；只有明确希望从第三方源码重建依赖时才使用 `--sources-build`。

#### Windows 原生 MSVC

先安装 Microsoft C++ Build Tools（勾选“使用 C++ 的桌面开发”）、CMake 3.31+、Ninja、Git LFS 和 Vulkan SDK，并设置 `VULKAN_SDK`。然后在 “Developer PowerShell for VS” 中执行：

```powershell
git lfs install
$env:GIT_LFS_SKIP_SMUDGE = '1'
git clone https://github.com/xiangCaO233/MusicMapMaker-Next.git
Set-Location MusicMapMaker-Next
Remove-Item Env:GIT_LFS_SKIP_SMUDGE

pwsh -File .\scripts\ci\msvc-build.ps1
.\build_msvc\bin\MusicMapMaker-Next.exe
```

脚本会在当前分支按需拉取公共头文件、运行资源、测试数据和 `windows/*/libs/x86_64/msvc/2026/RelWithDebInfo` 静态库，然后重新配置、编译并运行 CTest。预编译模式不检出第三方源码子模块，也不会拉取 Linux、macOS、MinGW、Debug、shared 或其他 MSVC 版本的预编译二进制。

#### Linux GCC 14 / Clang 19

下面以 GCC 14 为例。Clang 19 只需把路径中的 `gcc/gcc14` 改为 `clang/clang19`，并把脚本参数改为 `--compiler clang19`。

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next

git lfs pull \
  --include="3rdpty/prebuilts/headers/**,assets/**,tests/data/**,Modules/Main/src/logo.svg" \
  --exclude=""
git lfs pull \
  --include="3rdpty/prebuilts/binaries/linux/*/libs/x86_64/gcc/gcc14/RelWithDebInfo/**" \
  --exclude=""
bash scripts/ci/linux-build.sh --compiler gcc14 --fresh
./build_linux_gcc14/bin/MusicMapMaker-Next
```

Linux 还需要所选编译器、CMake 3.31+、Ninja、Vulkan 开发包、PkgConfig、Fontconfig 和桌面系统开发包。发行版包名不同，请使用对应发行版的软件仓库。

#### macOS Apple Silicon

安装 Xcode Command Line Tools、CMake 3.31+、Ninja、Git LFS 和 Vulkan SDK 后执行：

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next

git lfs pull \
  --include="3rdpty/prebuilts/headers/**,assets/**,tests/data/**,Modules/Main/src/logo.svg" \
  --exclude=""
git lfs pull \
  --include="3rdpty/prebuilts/binaries/macos/*/libs/arm64/clang/clang17/RelWithDebInfo/**" \
  --exclude=""
bash scripts/ci/macos-build.sh --arch arm64 --fresh
./build_macos_arm64/bin/MusicMapMaker-Next
```

`macos-build.sh` 会从本机 AppleClang 主版本自动推导预编译标签；如果本机不是 AppleClang 17，请使用仓库实际存在的标签或传入 `--compiler-tag`。

#### Linux 主机上的 Windows 交叉构建

这些脚本面向已经准备好 `/mnt/cross/windows`、Vulkan SDK 与对应 sysroot 的 CI/开发主机。仍需按上面的方式把 LFS 二进制路径替换为下表中的目标路径后再运行脚本。

| 目标 | 预编译路径（位于每个 package 的 `libs` 下） | 构建命令 | 输出目录 |
| --- | --- | --- | --- |
| clang-cl / MSVC ABI | `windows/*/libs/x86_64/msvc/2026/RelWithDebInfo/**` | `bash scripts/ci/cross/msvc-clang-build.sh --fresh` | `build_cross_msvc/bin` |
| MinGW GCC UCRT64 | `windows/*/libs/x86_64/mingw/ucrt64/RelWithDebInfo/**` | `bash scripts/ci/cross/mingw-gcc-build.sh --fresh` | `build_cross_mingw_gcc/bin` |
| MinGW Clang CLANG64 | `windows/*/libs/x86_64/mingw/clang64/RelWithDebInfo/**` | `bash scripts/ci/cross/mingw-clang-build.sh --fresh` | `build_cross_mingw_clang/bin` |

静态链接是所有示例的默认值。若使用 `--linkage shared`，还必须拉取对应工具链下的 `shared/RelWithDebInfo` 库、`bin` 运行时和符号文件；不要混用 static 与 shared 布局。

#### 从第三方源码构建依赖

源码构建不需要拉取任何 `3rdpty/prebuilts/binaries`，但会明显增加构建时间。例如：

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone --recurse-submodules \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next
git lfs pull --include="assets/**,tests/data/**,Modules/Main/src/logo.svg" --exclude=""
bash scripts/ci/linux-build.sh --compiler gcc14 --sources-build --fresh
```

脚本输出目录：原生 MSVC 为 `build_msvc/bin`，Linux 为 `build_linux_<compiler>/bin`，macOS 为 `build_macos_<arch>/bin`；CI 汇总后的可发布目录为 `release/windows-*`、`release/linux-*` 与 `release/macos-arm64`。

### 第三方源码来源

以下列表对应仓库直接维护的源码依赖、IonCachyEngine 内部依赖以及已固定的递归子模块。各项目版权和许可证仍归各自上游所有。

| 主项目依赖 | 上游源码 |
| --- | --- |
| IonCachyEngine | [xiangCaO233/IonCachyEngine](https://github.com/xiangCaO233/IonCachyEngine) |
| Dear ImGui | [ocornut/imgui](https://github.com/ocornut/imgui) |
| GLFW | [glfw/glfw](https://github.com/glfw/glfw) |
| sol2 | [ThePhD/sol2](https://github.com/ThePhD/sol2) |
| LuaJIT | [LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT) |
| FreeType | [freetype/freetype](https://github.com/freetype/freetype) |
| GLM | [icaven/glm](https://github.com/icaven/glm) |
| Clay | [nicbarker/clay](https://github.com/nicbarker/clay) |
| JSON for Modern C++ | [nlohmann/json](https://github.com/nlohmann/json) |
| stb | [nothings/stb](https://github.com/nothings/stb) |
| ImPlot | [epezent/implot](https://github.com/epezent/implot) |
| LunaSVG / PlutoVG | [sammycage/lunasvg](https://github.com/sammycage/lunasvg) / [sammycage/plutovg](https://github.com/sammycage/plutovg) |
| EnTT | [skypjack/entt](https://github.com/skypjack/entt) |
| moodycamel::ConcurrentQueue | [cameron314/concurrentqueue](https://github.com/cameron314/concurrentqueue) |
| nativefiledialog-extended | [btzy/nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) |
| ImGuiFileDialog | [aiekick/ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) |
| curl | [curl/curl](https://github.com/curl/curl) |
| miniz | [richgel999/miniz](https://github.com/richgel999/miniz) |
| Mbed TLS | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) |
| libdatachannel | [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel) |

| IonCachyEngine 音频依赖 | 上游源码 |
| --- | --- |
| FFmpeg | [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg) |
| Rubber Band | [breakfastquay/rubberband](https://github.com/breakfastquay/rubberband) |
| spdlog | [gabime/spdlog](https://github.com/gabime/spdlog) |
| fmt | [fmtlib/fmt](https://github.com/fmtlib/fmt) |
| OpenAL Soft | [kcat/openal-soft](https://github.com/kcat/openal-soft) |
| SDL | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) |
| zlib | [madler/zlib](https://github.com/madler/zlib) |
| LAME | [lameproject/lame](https://github.com/lameproject/lame) |
| FFTW | [FFTW/fftw3](https://github.com/FFTW/fftw3) |
| libsamplerate | [libsndfile/libsamplerate](https://github.com/libsndfile/libsamplerate) |

| 固定的递归/传递源码 | 上游源码 |
| --- | --- |
| libjuice | [paullouisageneau/libjuice](https://github.com/paullouisageneau/libjuice) |
| usrsctp | [paullouisageneau/usrsctp](https://github.com/paullouisageneau/usrsctp) |
| libsrtp | [cisco/libsrtp](https://github.com/cisco/libsrtp) |
| plog | [SergiusTheBest/plog](https://github.com/SergiusTheBest/plog) |
| dlg | [nyorain/dlg](https://github.com/nyorain/dlg) |
| Mbed TLS framework | [Mbed-TLS/mbedtls-framework](https://github.com/Mbed-TLS/mbedtls-framework) |
| Wayland Protocols | [freedesktop/wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) |

### 开源许可证

MusicMapMaker-Next 以 [GNU General Public License v3.0](LICENSE) 开源；也可阅读 [GNU 官方 GPL-3.0 文本](https://www.gnu.org/licenses/gpl-3.0.html)。第三方组件适用各自许可证，分发或再利用时请同时遵守对应上游条款。

---

## English

### Download and installation

Official Windows and Linux builds are single executable files. Download the file, place it anywhere, and run it without an installer. On first launch, the application creates configuration, logs, and synchronized runtime assets under `.config/mmm` in the user directory, so moving the executable does not discard user settings. macOS follows platform distribution conventions and ships a DMG plus a `.app` ZIP instead of a bare Mach-O executable.

- [Official website](https://mmm.xiang233.top): project home and latest-version information.
- [Official download page](https://mmm.xiang233.top/download): direct Windows, Linux, and macOS downloads.
- [Feedback home](https://feedback.xiang233.top): issue reports and suggestions.
- [GitHub repository](https://github.com/xiangCaO233/MusicMapMaker-Next): source, history, and development progress.

#### Which build should I download?

All builds provide the same application features. Their main differences are the target platform, ABI, C/C++ runtime, and compiler optimizer. Published builds use `RelWithDebInfo`: release-level optimization with debug information retained. They are not `Debug` builds. No compiler has a fixed performance advantage across every chart and hardware configuration.

| Artifact directory | Compiler and ABI | Optimization and purpose | Recommendation |
| --- | --- | --- | --- |
| `windows-msvc-clang` | LLVM clang-cl 22 with the MSVC 14.51 / Windows SDK 10.0.26100 ABI | `RelWithDebInfo`, PGO off; MSVC ecosystem compatibility and the website's default Windows build | First choice for most Windows users |
| `windows-mingw-gcc` | MinGW-w64 UCRT, GCC 14 series | `RelWithDebInfo`, PGO off; GNU toolchain alternative | Choose when GCC/UCRT compatibility is required |
| `windows-mingw-clang` | MinGW-w64 UCRT, LLVM/Clang 22, `clang64` layout | `RelWithDebInfo`, PGO off; LLVM MinGW alternative | Choose when the LLVM MinGW ABI is required |
| `linux-gcc14` | GCC 14, GNU/Linux x86_64 | `RelWithDebInfo`, PGO off; the website's default Linux build | First choice for most Linux users |
| `linux-clang19` | Clang 19, GNU/Linux x86_64 | `RelWithDebInfo` with PGO **instrumentation for collection** currently enabled; it does not consume a profile for final optimization and therefore has collection overhead | LLVM compatibility testing and PGO data collection |
| `macos-arm64` | AppleClang, ARM64; current prebuilt dependency tag `clang17` | `RelWithDebInfo`, PGO off; DMG and `.app` ZIP | Apple Silicon Macs |

The local `scripts/ci/msvc-build.ps1` script uses the native `cl.exe` from the active Visual Studio environment, so its compiler version is determined by the machine. It consumes the `msvc/2026` prebuilts, uses `RelWithDebInfo`, and disables PGO. It is different from the `windows-msvc-clang` CI artifact cross-compiled with clang-cl on Linux.

Official compiler and build-tool downloads:

- [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
- [LLVM / Clang Releases](https://github.com/llvm/llvm-project/releases)
- [GCC](https://gcc.gnu.org/)
- [MSYS2](https://www.msys2.org/) for MinGW-w64 UCRT/CLANG64 environments
- [Xcode](https://developer.apple.com/xcode/) for AppleClang
- [CMake download](https://cmake.org/download/), [Ninja](https://ninja-build.org/), and [Git LFS](https://git-lfs.com/)
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)

### Branches and artifacts

Build scripts always build the currently checked-out commit. A branch determines source content and CI triggers; it does not silently select a compiler.

| Branch | Purpose | Artifact behavior |
| --- | --- | --- |
| `main` | Default public branch and stable baseline | A manually invoked script produces binaries from the exact current `main` commit; a normal push does not trigger the repository's current full release matrix |
| `develop` | Day-to-day development and integration | Contains changes intended for the stable line; appropriate for local validation, but potentially less stable than `main` |
| `ci` | Continuous build and release orchestration | A push triggers three Windows builds, two Linux builds, and macOS ARM64, staging them under `release/<artifact-directory>`; website publication remains an explicit `workflow_dispatch` `website-release` job |

The native MSVC script no longer performs `fetch`, `checkout ci`, or `reset --hard`. Whether the checkout is on `main`, `develop`, a feature branch, or a detached commit, it builds that exact working tree.

### On-demand builds

Prebuilt dependencies are managed by Git LFS. Clone with `GIT_LFS_SKIP_SMUDGE=1` to avoid downloading binaries for every platform and compiler, then pull objects for only the selected toolchain. `SOURCES_BUILD` defaults to and must remain `OFF`; pass `--sources-build` only when intentionally rebuilding third-party dependencies from source.

#### Windows native MSVC

Install Microsoft C++ Build Tools with “Desktop development with C++”, CMake 3.31+, Ninja, Git LFS, and the Vulkan SDK. Set `VULKAN_SDK`, then run in “Developer PowerShell for VS”:

```powershell
git lfs install
$env:GIT_LFS_SKIP_SMUDGE = '1'
git clone https://github.com/xiangCaO233/MusicMapMaker-Next.git
Set-Location MusicMapMaker-Next
Remove-Item Env:GIT_LFS_SKIP_SMUDGE

pwsh -File .\scripts\ci\msvc-build.ps1
.\build_msvc\bin\MusicMapMaker-Next.exe
```

On the current branch, the script selectively pulls shared headers, runtime assets, test data, and the `windows/*/libs/x86_64/msvc/2026/RelWithDebInfo` static libraries. It then performs a fresh configure, build, and CTest run. Prebuilt mode does not check out third-party source submodules or download Linux, macOS, MinGW, Debug, shared, or other MSVC prebuilt binaries.

#### Linux GCC 14 / Clang 19

This example selects GCC 14. For Clang 19, replace `gcc/gcc14` with `clang/clang19` in the binary LFS path and pass `--compiler clang19`.

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next

git lfs pull \
  --include="3rdpty/prebuilts/headers/**,assets/**,tests/data/**,Modules/Main/src/logo.svg" \
  --exclude=""
git lfs pull \
  --include="3rdpty/prebuilts/binaries/linux/*/libs/x86_64/gcc/gcc14/RelWithDebInfo/**" \
  --exclude=""
bash scripts/ci/linux-build.sh --compiler gcc14 --fresh
./build_linux_gcc14/bin/MusicMapMaker-Next
```

Linux also requires the selected compiler, CMake 3.31+, Ninja, Vulkan development files, PkgConfig, Fontconfig, and desktop-system development packages. Package names vary by distribution.

#### macOS Apple Silicon

Install Xcode Command Line Tools, CMake 3.31+, Ninja, Git LFS, and the Vulkan SDK:

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next

git lfs pull \
  --include="3rdpty/prebuilts/headers/**,assets/**,tests/data/**,Modules/Main/src/logo.svg" \
  --exclude=""
git lfs pull \
  --include="3rdpty/prebuilts/binaries/macos/*/libs/arm64/clang/clang17/RelWithDebInfo/**" \
  --exclude=""
bash scripts/ci/macos-build.sh --arch arm64 --fresh
./build_macos_arm64/bin/MusicMapMaker-Next
```

`macos-build.sh` derives the prebuilt tag from the local AppleClang major version. If the machine does not use AppleClang 17, select a tag that actually exists in the repository or pass `--compiler-tag`.

#### Windows cross-builds on a Linux host

These scripts target a prepared CI/development host with `/mnt/cross/windows`, a Vulkan SDK, and the appropriate sysroot. Replace the LFS binary path in the preceding examples with the target path below before invoking the script.

| Target | Prebuilt path below each package's `libs` directory | Build command | Output directory |
| --- | --- | --- | --- |
| clang-cl / MSVC ABI | `windows/*/libs/x86_64/msvc/2026/RelWithDebInfo/**` | `bash scripts/ci/cross/msvc-clang-build.sh --fresh` | `build_cross_msvc/bin` |
| MinGW GCC UCRT64 | `windows/*/libs/x86_64/mingw/ucrt64/RelWithDebInfo/**` | `bash scripts/ci/cross/mingw-gcc-build.sh --fresh` | `build_cross_mingw_gcc/bin` |
| MinGW Clang CLANG64 | `windows/*/libs/x86_64/mingw/clang64/RelWithDebInfo/**` | `bash scripts/ci/cross/mingw-clang-build.sh --fresh` | `build_cross_mingw_clang/bin` |

Static linkage is the default for every example. With `--linkage shared`, also pull the matching `shared/RelWithDebInfo` libraries, `bin` runtime files, and symbols. Do not mix static and shared layouts.

#### Build third-party dependencies from source

A source build does not need any `3rdpty/prebuilts/binaries`, but takes substantially longer. For example:

```bash
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone --recurse-submodules \
  https://github.com/xiangCaO233/MusicMapMaker-Next.git
cd MusicMapMaker-Next
git lfs pull --include="assets/**,tests/data/**,Modules/Main/src/logo.svg" --exclude=""
bash scripts/ci/linux-build.sh --compiler gcc14 --sources-build --fresh
```

Script output directories are `build_msvc/bin` for native MSVC, `build_linux_<compiler>/bin` for Linux, and `build_macos_<arch>/bin` for macOS. Aggregated CI releases are staged under `release/windows-*`, `release/linux-*`, and `release/macos-arm64`.

### Third-party source origins

The following lists cover the source dependencies directly maintained by this repository, IonCachyEngine's internal dependencies, and pinned recursive submodules. Copyright and licenses remain with their respective upstream projects.

| Main-project dependency | Upstream source |
| --- | --- |
| IonCachyEngine | [xiangCaO233/IonCachyEngine](https://github.com/xiangCaO233/IonCachyEngine) |
| Dear ImGui | [ocornut/imgui](https://github.com/ocornut/imgui) |
| GLFW | [glfw/glfw](https://github.com/glfw/glfw) |
| sol2 | [ThePhD/sol2](https://github.com/ThePhD/sol2) |
| LuaJIT | [LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT) |
| FreeType | [freetype/freetype](https://github.com/freetype/freetype) |
| GLM | [icaven/glm](https://github.com/icaven/glm) |
| Clay | [nicbarker/clay](https://github.com/nicbarker/clay) |
| JSON for Modern C++ | [nlohmann/json](https://github.com/nlohmann/json) |
| stb | [nothings/stb](https://github.com/nothings/stb) |
| ImPlot | [epezent/implot](https://github.com/epezent/implot) |
| LunaSVG / PlutoVG | [sammycage/lunasvg](https://github.com/sammycage/lunasvg) / [sammycage/plutovg](https://github.com/sammycage/plutovg) |
| EnTT | [skypjack/entt](https://github.com/skypjack/entt) |
| moodycamel::ConcurrentQueue | [cameron314/concurrentqueue](https://github.com/cameron314/concurrentqueue) |
| nativefiledialog-extended | [btzy/nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) |
| ImGuiFileDialog | [aiekick/ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) |
| curl | [curl/curl](https://github.com/curl/curl) |
| miniz | [richgel999/miniz](https://github.com/richgel999/miniz) |
| Mbed TLS | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) |
| libdatachannel | [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel) |

| IonCachyEngine audio dependency | Upstream source |
| --- | --- |
| FFmpeg | [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg) |
| Rubber Band | [breakfastquay/rubberband](https://github.com/breakfastquay/rubberband) |
| spdlog | [gabime/spdlog](https://github.com/gabime/spdlog) |
| fmt | [fmtlib/fmt](https://github.com/fmtlib/fmt) |
| OpenAL Soft | [kcat/openal-soft](https://github.com/kcat/openal-soft) |
| SDL | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) |
| zlib | [madler/zlib](https://github.com/madler/zlib) |
| LAME | [lameproject/lame](https://github.com/lameproject/lame) |
| FFTW | [FFTW/fftw3](https://github.com/FFTW/fftw3) |
| libsamplerate | [libsndfile/libsamplerate](https://github.com/libsndfile/libsamplerate) |

| Pinned recursive/transitive source | Upstream source |
| --- | --- |
| libjuice | [paullouisageneau/libjuice](https://github.com/paullouisageneau/libjuice) |
| usrsctp | [paullouisageneau/usrsctp](https://github.com/paullouisageneau/usrsctp) |
| libsrtp | [cisco/libsrtp](https://github.com/cisco/libsrtp) |
| plog | [SergiusTheBest/plog](https://github.com/SergiusTheBest/plog) |
| dlg | [nyorain/dlg](https://github.com/nyorain/dlg) |
| Mbed TLS framework | [Mbed-TLS/mbedtls-framework](https://github.com/Mbed-TLS/mbedtls-framework) |
| Wayland Protocols | [freedesktop/wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) |

### Open-source license

MusicMapMaker-Next is licensed under the [GNU General Public License v3.0](LICENSE). The [official GNU GPL-3.0 text](https://www.gnu.org/licenses/gpl-3.0.html) is also available online. Third-party components remain under their own licenses; redistribution and reuse must comply with the corresponding upstream terms.
