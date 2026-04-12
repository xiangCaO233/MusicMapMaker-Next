# CLAUDE.md

本文件为 Claude Code（claude.ai/code）在此仓库中工作时提供指导。

## 构建与运行

- 首次构建前先初始化子模块：`git submodule update --init --recursive`
- 依赖通常从源码构建（默认 `SOURCES_BUILD=ON`），因此 `3rdpty/sources` 下的子模块属于标准构建流程的一部分。
- 配置本地构建：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`
- 构建全部内容：`cmake --build build`
- 仅构建应用目标：`cmake --build build --target MusicMapMaker-Next`
- 清理构建目录：`cmake --build build --target clean`
- 在仓库根目录运行应用：`./build/bin/MusicMapMaker-Next`
- 格式化改动过的 C++ 文件：`clang-format -i /absolute/path/to/file.cpp`

### Windows / 交叉编译说明

- 针对 MSYS2 环境提供了 MinGW 引导脚本：
  - `bash scripts/mingw64-env-bootstrap.sh`
  - `bash scripts/mingw64-clang-env-bootstrap.sh`
- 在 Linux 上交叉编译 Windows 版本时使用：`cmake -S . -B build-cross -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/cross-msvc.cmake`
- `cmake/toolchain/cross-msvc.cmake` 和顶层 `CMakeLists.txt` 中包含 MSVC、Windows SDK 以及 `VCPKG_ROOT` 的硬编码本地路径。如果在其他机器上配置失败，请优先检查这些路径。

### 测试 / 代码风格检查

- 仓库级别没有自动化测试套件，也没有统一的单项测试命令。不要依赖 `ctest` 或 `make test`。
- CMake 中没有专门的 lint 目标。代码格式化需手动通过 `clang-format` 完成。
- 修改后的常规验证方式是：成功构建，然后手动运行应用。

## 架构概览

### 启动流程

- `Modules/Main/src/main.cpp` 会从当前工作目录开始向上查找，直到找到 `assets/` 目录。
- 启动时随后会加载：
  - 通过 `Config::AppConfig` 加载 `user_config.json`
  - 通过 `Config::SkinManager` 加载当前激活的皮肤（目前为 `assets/skins/mmm-nightly/skin.lua`）
- 配置和皮肤加载完成后，程序会创建 `Graphic::NativeWindow`，检查 `Graphic::VKContext`，并进入 `MMM::GameLoop`。

### 模块边界

- `Modules/Main`：仅包含可执行程序入口。
- `Modules/Game`：编辑器/运行时外壳。大部分功能开发都发生在这里。
  - `Graphic`：Vulkan + GLFW 后端（`VKContext`、`VKRenderer`、离屏渲染器、纹理/图集管理）
  - `UI`：ImGui/Clay 视图和 `UIManager`
  - `Canvas`：三个编辑器视口（`Basic2DCanvas`、`PreviewCanvas`、`TimelineCanvas`）
  - `Logic`：线程化编辑器状态、ECS、渲染快照生成、播放/编辑系统
  - `Common`：共享命令/类型，例如 `LogicCommand`
- `Modules/MMM`：谱面、音符、时间点和项目序列化的核心领域模型。
- `Modules/Config`：持久化编辑器设置，以及基于 Lua 的皮肤、翻译、布局、着色器和资源加载。
- `Modules/Event`：单例的强类型事件总线，用于解耦 UI、Canvas、逻辑和音频。
- `Modules/Audio`：封装 `IonCachyEngine` 的 `AudioManager` 门面。
- `Modules/Log`：项目日志宏和日志初始化。

### 运行时数据流

- 应用分为 UI/渲染线程和一个专用逻辑线程。
- `GameLoop` 会注册核心 UI 以及三个画布：
  - 主编辑画布：`Basic2DCanvas`
  - 预览画布：`Preview`
  - 时间线画布：`Timeline`
- UI 交互会通过 `EventBus` 发布 `Event::LogicCommandEvent`。
- `Logic::EditorEngine` 订阅这些事件，将命令转发到当前活动的 `BeatmapSession`，并以约 240 Hz 的频率运行后台循环。
- `BeatmapSession` 拥有用于音符和时间线的 `entt` 注册表，以及编辑/播放状态。
- 逻辑层不会直接向 ImGui 渲染，而是生成 `Logic::RenderSnapshot` 对象，并通过 `Logic::BeatmapSyncBuffer` 推送它们。
- 每个画布都有自己的同步缓冲区和相机 ID；UI 线程拉取最新快照，并基于它记录 Vulkan 绘制命令。这条快照管线是逻辑与渲染之间的核心边界。
- 纹理 UV 映射也会通过 `EditorEngine::setAtlasUVMap()` / `getAtlasUVMap()` 按相机 ID 同步，因此这些字符串 ID 必须保持一致。

### 渲染栈

- `UI::UIManager` 实现了 `Graphic::IGraphicUserHook`，因此 Vulkan 渲染器会回调 UI 代码以完成资源准备、逐帧 UI 更新和离屏命令记录。
- 各画布先渲染到离屏 Vulkan 目标，然后显示在 ImGui 窗口中。
- `UI::Brush` 和画布批处理器会按纹理合并绘制调用；添加渲染代码时请保留这种批处理行为。
- `PreviewCanvas` 和 `TimelineCanvas` 是可交互视图，而不是被动镜像：它们会将 seek、scroll 和 resize 命令回发到逻辑层。

### 项目与资源模型

- 打开项目时会扫描目录中的谱面文件（`.osu`、`.imd`、`.mc`）和音频文件（`.mp3`、`.ogg`、`.wav`、`.flac`）。
- 项目元数据保存在 `mmm_project.json` 中。
- 全局编辑器设置保存在 `user_config.json` 中。
- 皮肤/主题数据由 Lua 驱动。`SkinLoader` 会递归加载当前皮肤中的颜色、布局值、着色器模块路径、翻译、纹理资源、音频资源和特效序列。
- UI 字符串基于翻译键（`TR` / `TR_FMT`），不是硬编码文本。

## 仓库特定规则

- 不要编辑 `3rdpty/` 下的第三方代码，唯一例外是 `3rdpty/sources/IonCachyEngine`。
- 使用 `Modules/Log/include/log/colorful-log.h` 中的 `XINFO`、`XWARN`、`XERROR` 等日志宏；不要引入 `std::cout` 或 `printf` 日志。
- 新增或修改的 C++ API 应遵循项目的 Doxygen 风格注释。
- 仓库指导倾向于使用智能指针代替原始 `new` / `delete`，并优先使用基于值的错误处理（`std::expected`、`std::optional`），避免引入新的重异常风格代码。
- 保持现有命名规范：类型/命名空间使用 PascalCase，函数/局部变量使用 camelCase，成员字段使用 `m_` 前缀，常量/宏使用 `UPPER_SNAKE_CASE`。
- 保持依赖方向不变：高层 UI 代码可以依赖 `Graphic`，但底层渲染代码不应依赖 UI 层概念。
- 添加动态 ImGui 窗口标题时，请保留稳定的内部 ID，并使用 `###...`，以免破坏停靠/布局持久化。
- 如果修改了任何 `.cpp`、`.h` 或 `.hpp` 文件，结束前请对其绝对路径执行 `clang-format -i`。
- 仓库建议的提交信息风格为：`type(scope): 中文描述`。
