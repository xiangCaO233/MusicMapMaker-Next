# MusicMapMaker-Next 智能体指令

本文档面向在 `MusicMapMaker-Next` 仓库中工作的 AI 编程智能体。请严格遵守这些规则，确保修改安全、自然，并符合项目现有约定。

## 0. 核心原则

- **最小化修改**: 只处理用户明确提出的需求，不做未授权的大规模重构。
- **先理解再修改**: 修改前优先使用 `rg`、`rg --files` 等工具了解相关代码和项目结构。
- **命令执行位置**: 运行 shell 命令时必须在项目根目录执行。
- **文件操作路径**: 文件读写、格式化等操作必须使用绝对路径。
- **第三方代码边界**: 除 `3rdpty/sources/IonCachyEngine` 外，不要修改或分析 `3rdpty/` 下的第三方源码。
- **依赖 API 防过时**: 对 Vulkan、sol2、ImGui、Clay 等更新较快的第三方库编写调用代码前，必须联网确认当前 API 签名和最佳实践。

## 1. 项目概览

- **语言**: 现代 C++，使用 C++20/C++23。
- **构建系统**: CMake，最低版本 3.31。
- **UI 与布局**: ImGui、Clay。
- **图形 API**: Vulkan (`<vulkan/vulkan.hpp>`)、GLFW。
- **脚本语言**: LuaJIT，通过 sol2 (`<sol/sol.hpp>`) 绑定。
- **音频引擎**: 自定义 `IonCachyEngine`，位于 `3rdpty/sources/IonCachyEngine`。
- **数学库**: GLM (`<glm/glm.hpp>`)。
- **日志库**: `spdlog`、`fmt`，已封装为项目日志宏。

## 2. 目录边界

- **`Modules/`**: 项目核心模块，包含 `Audio`、`Config`、`Event`、`Game`、`Log`、`Main`、`MMM`、`Network`。主要业务逻辑位于此处。
- **`tests/`**: 跨模块共享测试资源。`tests/data/` 下的资源文件必须通过 Git LFS 追踪，测试运行输出禁止写入该目录。
- **`3rdpty/`**: 第三方依赖目录。
  - **严重警告**: 绝对不要修改或分析 `3rdpty/` 下的任何代码库，唯一例外是 `3rdpty/sources/IonCachyEngine`。
  - stb、glm、lunasvg 等库均为 GitHub 原始克隆，修改它们会破坏构建过程或产生未跟踪的下游补丁。
  - `IonCachyEngine` 是项目自定义核心引擎，按本项目其他代码的风格规则处理。

## 3. 构建、测试与格式化

### 3.1 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
cmake --build build --target clean
```

### 3.2 测试

- **CTest 已集成**: 配置和构建完成后，使用 `ctest --test-dir build --output-on-failure` 运行测试套件。
- **查看测试列表**: 使用 `ctest --test-dir build -N`。
- **新增测试可执行文件**: 必须通过顶层 `CMakeLists.txt` 中的 `mmm_add_test_executable(<ModuleName> <TargetName> ...)` 创建，确保二进制输出到 `${CMAKE_BINARY_DIR}/tests/<ModuleName>`。
- **测试资源目录**: 跨模块测试资源根目录为 `${MMM_TEST_RESOURCE_DIR}`，默认指向 `${CMAKE_SOURCE_DIR}/tests/data`。
- **测试输出目录**: 测试生成文件应写入 `${CMAKE_BINARY_DIR}/test_output` 或测试传入的输出目录，禁止写入源码资源目录。
- **资源覆盖测试约定**:
  - `BeatmapSpeedTransformTest` 覆盖 `tests/data/ma` 下的 `.mc`、`.mmm`、`.osu`、`.imd` 谱面资源。
  - `AudioSpeedExportServiceTest` 覆盖 `tests/data/ma` 下的音频资源，并支持通过 `MMM_AUDIO_PROBE_FILE=/path/to/audio` 对单个外部音频文件进行解码探针诊断。

### 3.3 修改后的验证

- 修改代码后，必须确保 `cmake --build build` 成功。
- 修改测试或测试相关逻辑后，还必须运行相关 CTest。
- 测试范围不明确时，运行完整测试套件：`ctest --test-dir build --output-on-failure`。

### 3.4 格式化

- 修改过的 `.cpp`、`.h`、`.hpp` 文件，完成前必须运行：

```bash
clang-format -i <absolute_file_path>
```

- 修改过的项目自维护 CMake 脚本，完成前必须运行：

```bash
cmake-format -i <absolute_file_path>
```

- CMake 格式化范围仅包括以下项目自维护脚本：
  - `CMakeLists.txt`
  - `cmake/**/*.cmake`
  - `3rdpty/cmake/**/*.cmake`
  - `Modules/**/CMakeLists.txt`
  - `3rdpty/sources/cmake/*.cmake`
  - `3rdpty/sources/IonCachyEngine/CMakeLists.txt`
  - `3rdpty/sources/IonCachyEngine/src/CMakeLists.txt`
  - `3rdpty/sources/IonCachyEngine/cmake/*.cmake`
  - `3rdpty/sources/IonCachyEngine/3rdpty/cmake/**/*.cmake`
  - `3rdpty/sources/IonCachyEngine/3rdpty/sources/cmake/*.cmake`
- 除上述路径外，仍然禁止格式化或修改 `3rdpty/` 下的其它第三方源码 CMake 文件。
- 新增或修改项目自维护 CMake 脚本时，注释必须使用中文，并为入口开关、公共 helper 函数、复杂目标分组、预编译库布局和配置映射写清楚维护性说明。

### 3.5 预编译库

- `SOURCES_BUILD` 是第三方依赖来源的唯一开关，默认值必须为 `OFF`：
  - `SOURCES_BUILD=ON` 时使用 `3rdpty/sources` 源码构建。
  - `SOURCES_BUILD=OFF` 时只使用预编译库，禁止自动回退到源码构建；缺少预编译包、头文件或库文件时必须 `FATAL_ERROR`。
- `PROJECT_LINKAGE` 是主项目依赖链接偏好，`ICE_LINKAGE` 是 `IonCachyEngine` 独立项目依赖链接偏好，二者取值只能为 `static` 或 `shared`：
  - `static` 偏好使用静态依赖库，MSVC 运行库使用 `/MT` 或 `/MTd`。
  - `shared` 偏好使用动态依赖库，MSVC 运行库使用 `/MD` 或 `/MDd`，GNU/Clang 链接参数不得包含 `-static`、`-static-libstdc++`、`-static-libgcc` 等静态运行库选项。
  - `IonCachyEngine` 作为主项目子项目时，`ICE_LINKAGE` 默认继承 `PROJECT_LINKAGE`；独立构建时默认 `static`。
- PGO 只允许作用于本项目业务模块；预编译库二进制以及用于生成预编译库的第三方依赖源码构建均禁止 PGO 插桩。
- 主项目预编译库目录布局：
  - 共享头文件：`3rdpty/prebuilts/<package>/include`
  - 静态库：`3rdpty/prebuilts/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库导入库：`3rdpty/prebuilts/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 动态运行时文件：`3rdpty/prebuilts/<platform>/<package>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 静态库 PDB：`3rdpty/prebuilts/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库 PDB：`3rdpty/prebuilts/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
- `IonCachyEngine` 内部依赖使用同样布局：
  - 共享头文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<package>/include`
  - 静态库：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库导入库：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 动态运行时文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<platform>/<package>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 静态库 PDB：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库 PDB：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
- Windows x86_64 的参考静态布局为 `libs/x86_64/msvc/2026/<config>`、`libs/x86_64/mingw/clang64/<config>`、`libs/x86_64/mingw/ucrt64/<config>`；动态布局在工具链标签后额外包含 `shared/<config>`。
- 预编译库必须区分构建配置，至少提供 `Debug` 与 `Release` 目录；`RelWithDebInfo`、`MinSizeRel` 优先使用同名目录，未提供时使用 `Release`。
- MSVC 的 `Debug` 与 `RelWithDebInfo` 预编译二进制如果生成了外置 PDB，必须一并放入对应 `symbols` 目录；缺少外置 PDB 不影响链接，但会降低依赖库调试质量。
- 新增或调整预编译包时，必须为每个具体库编写独立的 `Findxxx.cmake`，并在该文件内显式创建对应 target；禁止使用一个大一统的 `Find*.cmake` 或通用 `add_header_target` / `add_static_target` helper 代替具体库的查找脚本。
- 每个 `Findxxx.cmake` 必须导出与源码构建一致的 CMake target 名称，业务模块不得感知依赖来源差异。
- `IonCachyEngine` 作为独立项目，其内部 `3rdpty/cmake/**/*.cmake` 和 `3rdpty/CMakeLists.txt` 禁止包含 `MMM` 相关文件名、变量名、函数名或注释标识。
- 所有预编译库相关文件必须由 Git LFS 追踪，路径至少包含主仓 `.gitattributes` 中的 `3rdpty/prebuilts/**`、`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/**`，以及 `IonCachyEngine` 仓库自身 `.gitattributes` 中的 `3rdpty/prebuilts/**`。

## 4. 架构与模块依赖

- **依赖方向**: `UI` 模块可以依赖 `Graphic` 模块，但 `Graphic` 模块严禁直接引用 `UI` 模块的高层结构，例如 `Brush` 或 `DrawCmd`。
- **抽象回调**: 底层渲染器若需执行 UI 绘制，必须通过抽象接口或虚函数回调实现，例如 `onRecordDrawCmds`。
- **命名空间**: 核心业务 UI 逻辑使用 `MMM::UI`；底层图形封装使用 `MMM::Graphic`。

## 5. C++ 代码规范

### 5.1 继承与内存

- **多重继承 UI 视图**: 同时继承 `ITextureLoader` 和 `IUIView` 等基类时，必须对 `IUIView` 使用虚继承：`virtual public IUIView`。
- **禁止异常机制**: 严禁使用 `throw`、`try`、`catch`，必须依赖返回值、`std::expected` 或 `std::optional` 表达失败。
- **禁止原始指针分配**: 严禁使用 `new` 和 `delete`，必须使用智能指针或值语义资源管理。

### 5.2 热路径规则

以下场景及其每帧、每 update 调用链均视为热路径：

- 主渲染循环。
- `VKRenderer::render`。
- `UIManager::onUpdateUI`。
- 离屏命令录制。
- `EditorEngine::loop`。
- `BeatmapSession::update`。

热路径必须遵守以下限制：

- **共享智能指针**: 避免 `std::shared_ptr` / `std::weak_ptr` 的所有权复制。能使用 `T&`、`const T&`、稳定索引、稳定 ID 或明确生命周期的观察指针时，优先使用非拥有访问。
- **共享所有权例外**: 确实需要共享所有权保证跨线程生命周期时，必须在 Doxygen `@warning` 中说明原因、执行频率和替代方案阻塞点。
- **原子操作**: 渲染路径和逻辑循环中尽量避免 `std::atomic`。不可避免的原子标志必须使用最弱可证明正确的 memory order，并在成员变量和热路径访问函数的 Doxygen `@warning` 中说明写入者、读取者、用途和不可避免原因。
- **Doxygen 标注**: 热路径函数必须单独添加 Doxygen `@warning`，简短说明执行频率以及禁止引入的操作。
- **阻塞操作标注**: 热路径附近若存在 `waitIdle`、Fence 等待、阻塞式 acquire/present、sleep/yield、跨线程 join 等耗时或不可中断操作，必须用 Doxygen `@warning` 标明触发条件，并确保它只处于低频路径或 Vulkan 必需同步点。
- **绝对禁止**: 每帧或每 update 执行文件系统操作、完整遍历 entt 对象、完整排序、可能抛出异常的操作，以及任何 `try` / `catch` 行为。需要这些行为时，必须改为脏标记、缓存、增量索引、预排序数据或低频资源重载流程。

### 5.3 注释

- 新增或修改的类、函数、成员变量必须使用 Doxygen 风格注释，例如 `/// @brief ...`。
- 代码注释应说明意图和约束，不要写面向用户的对话式注释。

### 5.4 命名

- **命名空间**: `PascalCase`，例如 `MMM`、`Config`。
- **类和结构体**: `PascalCase`。
- **方法和函数**: `camelCase`。
- **变量**: `camelCase`。
- **成员变量**: 必须以 `m_` 开头，例如 `m_data`。
- **常量和宏**: `UPPER_SNAKE_CASE`，例如 `XINFO`。

### 5.5 格式与包含

- **缩进**: 4 个空格。
- **大括号与空格**: 遵循项目 `.clang-format`。函数和类换行，控制流不换行，括号内保留空格，例如 `if ( a == b )`。
- **Includes 顺序**: 对应头文件优先，其次项目头文件，然后第三方头文件，最后标准库头文件。
- **头文件保护**: 使用 `#pragma once`。

### 5.6 日志

- 严禁使用 `std::cout` 或 `printf`。
- 必须使用 `"log/colorful-log.h"` 中的自定义日志宏：

```cpp
XINFO("Skin loaded: {}", m_data.themeName);
XERROR("Failed to load skin lua: {}", err.what());
```

## 6. UI 与渲染规范

- **ImGui 停靠稳定性**: 标题动态变化的窗口必须使用 `###` 固定内部 ID，例如 `ImGui::Begin("标题###StaticID")`。
- **批处理性能**: `Brush` 绘制时应减少纹理切换，合并相同状态的 Draw Call。

## 7. Git 与测试资源

- **提交格式**: `type(scope): 中文描述`。
- **提交要求**: 冒号 (`:`) 之前必须使用英文，例如 `feat`、`fix`、`refactor`、`docs`；冒号之后必须使用中文描述。
- **提交示例**: `feat(render): 重构渲染管线`。
- **测试资源 LFS**: `tests/data/**`、音频、图片、字体、`.mcz`、`.zip` 等资源由 Git LFS 追踪。
- **新增资源前检查**: 新增测试资源前先确认 `.gitattributes` 中已有对应规则，必要时使用 `git lfs track` 补充。

## 8. 当前状态备忘

### 8.1 已完成里程碑

- **模块解耦**: `Graphic` 与 `UI` 模块已通过抽象回调 `onRecordDrawCmds` 解耦。
- **命名空间规范**: UI 相关逻辑已迁移至 `MMM::UI`。
- **高性能渲染**: `Brush` 类实现状态机 API 和自动批处理，支持矩形和圆形几何生成。
- **UI 框架稳固**: 修复 ImGui 动态标题导致的停靠丢失问题，并解决多重继承下的菱形继承冲突。
- **ECS 交互层**: 引入 `entt`，建立基于 ECS 的拾取与交互系统，支持音符悬停高亮、点击选择和跨线程鼠标拖拽。
- **音符渲染优化**: 实现 Polyline 连续几何生成，支持斜向连接段和纹理批处理。
- **判定线实现**: `NoteRenderSystem` 已引入判定线绘制逻辑，支持通过 `EditorConfig` 动态配置位置和线宽。
- **核心模块重构**: 完成 `BeatmapSession` 和 `NoteRenderSystem` 职责拆分与解耦，建立 `session/`、`render/` 子目录，并将内部批处理器提取为独立 `Batcher.h`。
- **预览区系统实现**: 实现独立 `PreviewCanvas` 视口，支持主画布视野包围框与同步判定线绘制。
- **配置系统升级**: 重构 `SkinLoader` 颜色解析逻辑，支持递归读取嵌套 Lua 颜色表，例如 `colors.preview.box`。
- **渲染修复与适配**: `NoteRenderSystem` 已完成所有物件在预览缩放下的适配，并修复 Polyline 在压缩比例下的几何断裂问题。
- **音频引擎集成**: 初步集成 `AudioManager`，实现逻辑线程与音频播放进度同步。
- **自动化测试基础**: 接入 CTest，统一测试可执行文件输出目录为 `${CMAKE_BINARY_DIR}/tests/<ModuleName>`，并建立 `tests/data/ma` 资源覆盖测试。
- **音频解码诊断覆盖**: `AudioSpeedExportServiceTest` 支持资源目录批量解码探针和 `MMM_AUDIO_PROBE_FILE` 单文件诊断入口。

### 8.2 待办事项

- **预览区交互跳转**: 实现点击预览区任意位置，主画布平滑跳转至对应时间点。
- **UI 细节优化**: 在预览区边缘添加时间刻度或小节线提示。
- **性能压测**: 针对大规模谱面进行压力测试，验证预览区渲染负载及剔除效率。
- **脚本集成**: 将 `Brush` API 与 ECS 交互事件暴露给 Lua 环境。
- **撤销 / 重做系统**: 基于指令队列实现编辑操作的 Undo / Redo。
