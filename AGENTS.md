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

- **构建来源固定**：Linux 与 Windows 的预编译发布产物必须在 `debsrv` 的固定 GitHub Runner 镜像环境中构建，再同步回本机提交；禁止用开发机较新的编译器、系统库或运行库生成替代产物，也禁止仅改目录标签冒充目标工具链。
- **macOS 构建来源**：使用现有原生 macOS Runner 的 AppleClang、SDK 和部署下限生成预编译库，不使用 Linux 开发机替代；同步时同样保留头文件、调试符号以及 Debug、RelWithDebInfo 配置。

- `SOURCES_BUILD` 是第三方依赖来源的唯一开关，默认值必须为 `OFF`：
  - `SOURCES_BUILD=ON` 时使用 `3rdpty/sources` 源码构建。
  - `SOURCES_BUILD=OFF` 时只使用预编译库，禁止自动回退到源码构建；缺少预编译包、头文件或库文件时必须 `FATAL_ERROR`。
- `PROJECT_LINKAGE` 是主项目依赖链接偏好，`ICE_LINKAGE` 是 `IonCachyEngine` 独立项目依赖链接偏好，二者取值只能为 `static` 或 `shared`：
  - `static` 偏好使用静态依赖库，MSVC 运行库使用 `/MT` 或 `/MTd`。
  - `shared` 偏好使用动态依赖库，MSVC 运行库使用 `/MD` 或 `/MDd`，GNU/Clang 链接参数不得包含 `-static`、`-static-libstdc++`、`-static-libgcc` 等静态运行库选项。
  - `IonCachyEngine` 作为主项目子项目时，`ICE_LINKAGE` 默认继承 `PROJECT_LINKAGE`；独立构建时默认 `static`。
- PGO 只允许作用于本项目业务模块；预编译库二进制以及用于生成预编译库的第三方依赖源码构建均禁止 PGO 插桩。
- 主项目预编译库目录布局：
  - 共享头文件：`3rdpty/prebuilts/headers/<package>/include`
  - 静态库：`3rdpty/prebuilts/binaries/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库导入库：`3rdpty/prebuilts/binaries/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 动态运行时文件：`3rdpty/prebuilts/binaries/<platform>/<package>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 静态库外置符号文件：`3rdpty/prebuilts/binaries/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库外置符号文件：`3rdpty/prebuilts/binaries/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
- `IonCachyEngine` 内部依赖使用同样布局：
  - 共享头文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/headers/<package>/include`
  - 静态库：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/binaries/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库导入库：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/binaries/<platform>/<package>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 动态运行时文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/binaries/<platform>/<package>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
  - 静态库外置符号文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/binaries/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/<config>`
  - 动态库外置符号文件：`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/binaries/<platform>/<package>/symbols/<arch>/<toolchain>/<compiler-tag>/shared/<config>`
- Windows x86_64 的参考静态布局为 `libs/x86_64/msvc/2026/<config>`、`libs/x86_64/mingw/clang64/<config>`、`libs/x86_64/mingw/ucrt64/<config>`、`libs/x86_64/mingw/gcc14-win32/<config>`；动态布局在工具链标签后额外包含 `shared/<config>`。
- Linux x86_64 的参考静态布局为 `libs/x86_64/gcc/gcc14/<config>`、`libs/x86_64/clang/clang19/<config>`；动态布局在工具链标签后额外包含 `shared/<config>`。
- 预编译库必须区分构建配置，至少提供 `Debug` 与 `RelWithDebInfo` 目录；`Release`、`RelWithDebInfo`、`MinSizeRel` 等发布型 CMake 配置优先使用 `RelWithDebInfo` 预编译目录，缺失时才回退到 `Release`。
- `Debug` 与 `RelWithDebInfo` 预编译二进制必须保留调试符号：MSVC 生成的外置 `.pdb` 必须一并放入对应 `symbols` 目录；MinGW 静态 `.a` 通常没有旁路 `.pdb`，调试信息应保留在归档内的 COFF CodeView 段中；Linux 静态 `.a` 调试信息应保留在 ELF `.debug*` 段中；禁止对这些预编译库执行 `strip`。
- 新增或调整预编译包时，必须为每个具体库编写独立的 `Findxxx.cmake`，并在该文件内显式创建对应 target；禁止使用一个大一统的 `Find*.cmake` 或通用 `add_header_target` / `add_static_target` helper 代替具体库的查找脚本。
- 每个 `Findxxx.cmake` 必须导出与源码构建一致的 CMake target 名称，业务模块不得感知依赖来源差异。
- `IonCachyEngine` 作为独立项目，其内部 `3rdpty/cmake/**/*.cmake` 和 `3rdpty/CMakeLists.txt` 禁止包含 `MMM` 相关文件名、变量名、函数名或注释标识。
- 所有预编译库相关文件必须由 Git LFS 追踪，路径至少包含主仓 `.gitattributes` 中的 `3rdpty/prebuilts/**`、`3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/**`，以及 `IonCachyEngine` 仓库自身 `.gitattributes` 中的 `3rdpty/prebuilts/**`。

### 3.6 macOS 编译兼容

- **sol2 的 Lua nil**：判断可选 `sol::object` 是否存在时优先使用 `valid()`；确需按类型判断时使用 `sol::type::lua_nil`，向 Lua 写入 nil 时使用 `sol::lua_nil`。禁止使用 `sol::type::nil`：它在 Apple/Objective-C 环境中不可用，会导致 macOS 编译失败。
- **浮点文本解析**：面向 macOS 26 之前系统的代码不得直接用 `std::from_chars` 解析 `float` / `double`，因为该重载不能在这些部署目标上使用。统一调用 `mmm/SafeParse.h` 中的 `MMM::Internal::parseFloatingPrefix`，按字段语义检查错误码、消费长度、有限值及范围；要求严格十进制时，还须限制输入字符，避免 Apple 分支的 `strtod_l` 接受十六进制等扩展形式。整数 `std::from_chars` 可照常使用。
- **同类问题排查**：修复平台编译错误时，检查项目自维护代码中的同类 API 用法，并按最低支持的部署目标验证；本机较新的 SDK 或其他平台构建通过，不能代替目标平台验证。

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
- **禁止等待长持锁区**: 每帧或每 update 路径不得等待另一线程可能跨整轮 `BeatmapSession::update`、渲染或其他重计算持有的互斥锁；锁内即使只做常量级检查、状态未变化也不例外。新增热路径加锁前必须追查同一把锁的所有持有范围，并用性能采样或锁等待记录确认实际等待。优先读取已发布的不可变快照；简单展示状态可使用有明确写入者和内存序的原子值；状态未变化时应在锁外快速返回，真正变化再进入低频加锁路径并复核。不得为避锁而无同步读取可变的 `SessionContext`。`setSessionCanvasVisible` 曾因每帧等待会话更新长锁使播放帧率明显下降，是这一规则的回归案例。
- **播放性能回归定位**: 必须用用户实际出现问题的谱面和播放过程复现，在相同硬件、构建配置和用户配置下比较播放区间；启动、载图和停留界面的样本不得混入常态播放结论。分别测量 CPU 热点、线程锁等待、GPU 忙碌度和 Fence/acquire/present 阶段耗时，再决定优化方向；低 GPU 占用配合高 `updateUI` 时间时，应优先检查 UI 热路径及其跨线程等待。每轮修改后用同一负载复测，避免把局部函数变快误判为整帧性能恢复。
- **禁止固定时长阻塞同步**: 逻辑、渲染、UI 与联机同步调用链严禁用硬编码的毫秒/秒级窗口配合 `sleep`、阻塞式 `wait_for` 或忙等来等待数据、视野、时间、缩放、物件状态或网络同步完成；连续交互必须逐次立即更新本地视觉与逻辑状态。非阻塞消抖、合并和限频可以用于推迟低频完整同步、资源重建或网络发送，但等待期间线程必须继续正常推进，并且只能覆盖保存最新待处理状态，不能延迟本地交互反馈；鼠标松开、手势结束或命令提交时必须立即提交最终网络状态。用户显式配置的 FPS/UPS 限制、定时自动保存、音频缓冲/预读、协议超时/重连以及文件系统批量变更合并同样不得反向阻塞本地逻辑或渲染。
- **绝对禁止**: 每帧或每 update 执行文件系统操作、完整遍历 entt 对象、完整排序、可能抛出异常的操作，以及任何 `try` / `catch` 行为。需要这些行为时，必须改为脏标记、缓存、增量索引、预排序数据或低频资源重载流程。

### 5.3 注释

- 新增或修改的类、函数、成员变量必须使用 Doxygen 风格注释，例如 `/// @brief ...`。
- 代码注释应说明意图和约束，不要写面向用户的对话式注释。
- 新增或修改的代码注释必须使用中文；API、类型名、宏名、命令、协议标识等专有名词可以保留原文，但说明性文字必须使用中文。

#### 5.3.1 全项目注释率

- **最低要求**：全项目每一个自维护代码文件的注释率都必须独立 **大于等于 30%**。头文件（如 `.h`、`.hpp`）与实现文件（如 `.cpp`）分别计算、分别达标；同名配套文件、同一类、同一模块或不同语言之间均不得合并计算或互相抵扣，项目整体比例不能代替逐文件达标。
- **计算口径**：使用 `cloc --by-file` 的逐文件 `comment` 和 `code` 行数，对每个文件分别按 `comment / (comment + code) × 100%` 计算；空行不计入分母，混合代码与注释的行遵循 `cloc` 分类，不重复计算。禁止用汇总比例或平均比例判定单个文件达标；分母为零的空文件标记为不适用，不作为达标文件。
- **统计范围**：包括自维护的业务代码、头文件、测试代码、构建脚本和工具脚本；包含本项目维护的 `IonCachyEngine` 代码，但排除其内部第三方依赖。主项目自维护的 `3rdpty/cmake`、`3rdpty/sources/cmake` 等构建脚本仍需计入。
- **排除范围**：第三方源码、预编译库、构建目录、自动生成代码、代理头文件、文档及纯数据/资源文件不参与统计。不得将 `.mc` 谱面按 Windows Message File 计入源码；资源目录内实际维护的 Lua 等程序脚本仍属于代码，不能仅因位于资源目录就排除。
- **注释质量**：优先解释设计意图、接口契约、边界条件、算法、不变量、线程安全、生命周期及性能约束。禁止通过重复代码含义、无意义分隔行、复制模板、堆积空注释或保留大段注释掉的废弃代码凑比例；已有 Doxygen 和热路径说明要求继续生效。
- **函数内外分工**：函数声明或定义前的 Doxygen 注释说明职责、参数、返回值、前置条件及整体约束，不能代替函数体内的实现说明。函数体中的关键算法步骤、非显然的条件分支、边界与失败处理、状态转换、资源生命周期和线程同步等关键部分，必须在对应实现附近添加中文注释，解释为什么这样处理以及需要维持的约束。禁止将注释全部集中堆放在函数外部开头来满足比例；逐文件注释率达标后，仍须检查函数体关键实现是否得到说明。简单直白的语句无需逐行复述，也不得为凑比例增加无意义注释。
- **修改后检查**：日常仅检查本批新增或修改文件，简短报告路径、注释行数、代码行数和比例；任一文件不足 30% 时必须明确说明。全项目逐文件统计及其他未达标清单仅在阶段收尾或用户明确要求时刷新，平时引用最近基线并注明日期，不重复扫描未修改文件。
- **任务边界**：注释补充不等于全面安全、异常、实时性或跨平台审计。每批按一个职责处理，通常 5～10 个文件，大型文件可独立成批；阅读范围以准确解释实现所需为限。发现行为问题只简短登记，修复或重构须另行授权，不作为注释任务完成条件。
- **验证与记录成本**：纯注释修改按批统一格式化、检查差异并构建，避免每补一处就重复构建；测试按风险选择，测试相关变更仍遵循 3.3。不得为纯注释任务新增故障注入、压力或跨平台探针。任务文档只维护当前状态、短批次清单和必要验证结论，不追加逐轮流水账；历史长篇记录归档，非定位具体历史问题无需读取。跨模块大规模补注释仍须用户授权。

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
- **前向声明优先**: 公开头文件中的类型若只以指针、引用、`std::shared_ptr` 或 `std::unique_ptr` 形式出现，优先使用前向声明；只有按值成员、继承、内联实现、模板实例化或其他确实要求完整类型的场景才直接包含定义头。对 scoped enum 使用 opaque declaration 时必须保持既有底层类型和 ABI。
- **禁止依赖传递包含**: 每个 `.cpp` 必须显式包含其实际使用类型、函数、宏和第三方 API 的定义头；公开头必须可以自包含，但不得通过引入无关的聚合头或上层模块头来“碰巧编译通过”。
- **保持值语义**: 轻量 DTO、配置值和固定大小状态应继续按值存储并优先分配在栈上，不得仅为减少 include 将其无意义地改为堆对象；多处共享的轻量类型应提取到职责单一的轻量头。
- **PImpl 使用边界**: PImpl 或指向 incomplete type 的 `std::unique_ptr` 仅用于不需要暴露的私有重状态、第三方容器和稳定的长生命周期管理器；析构函数必须在完整类型可见的 `.cpp` 中定义，并避免为热路径引入逐帧分配。
- **序列化声明归属**: 类型的 `to_json` / `from_json` 等 ADL 声明必须与类型放在同一个轻量头中；头文件使用 `nlohmann/json_fwd.hpp`，具体序列化实现和需要构造 JSON 的实现文件显式包含 `nlohmann/json.hpp`。
- **依赖治理验证**: 收窄高扇出头文件后必须执行满线程构建和相关测试，并使用 Ninja 依赖图或等价手段复查受影响头文件的翻译单元扇出，防止通过新的传递包含抵消改进。

### 5.6 日志

- 严禁使用 `std::cout` 或 `printf`。
- 必须使用 `"log/colorful-log.h"` 中的自定义日志宏：

```cpp
XINFO("Skin loaded: {}", m_data.themeName);
XERROR("Failed to load skin lua: {}", err.what());
```

## 6. UI 与渲染规范

- **ImGui 停靠稳定性**: 标题动态变化的窗口必须使用 `###` 固定内部 ID，例如 `ImGui::Begin("标题###StaticID")`。
- **按钮反馈统一入口**: 项目业务 UI 中所有可见 ImGui 按钮必须使用 `MMM::UI::FeedbackButton` 或 `MMM::UI::FeedbackSmallButton`，并通过 `ui/utils/UIWidgetUtils.h` 引入；禁止直接使用 `ImGui::Button` / `ImGui::SmallButton`，以确保悬浮音效、点击音效和悬浮色渐变统一生效。只有 `FeedbackButton` / `FeedbackSmallButton` 的实现内部，或明确没有可见按钮外观的自定义 hit zone，才允许直接调用原始 ImGui 按钮 API。
- **批处理性能**: `Brush` 绘制时应减少纹理切换，合并相同状态的 Draw Call。

## 7. Git 与测试资源

- **提交格式**: `type(scope): 中文描述`。
- **提交要求**: 冒号 (`:`) 之前必须使用英文，例如 `feat`、`fix`、`refactor`、`docs`；冒号之后必须使用中文描述。
- **提交示例**: `feat(render): 重构渲染管线`。
- **测试资源 LFS**: `tests/data/**`、音频、图片、字体、`.mcz`、`.zip` 等资源由 Git LFS 追踪。
- **新增资源前检查**: 新增测试资源前先确认 `.gitattributes` 中已有对应规则，必要时使用 `git lfs track` 补充。

### 7.0 0.6.2 到 0.6.3 的过渡期例外

- **当前优先规则**：0.6.3 正式发布完成前，暂不启用下文的新 Git 分支工作流；日常功能、修复和构建配置修改直接在 `develop` 上累积规范提交，不为这些任务创建新的功能或修复分支，也不强制通过 PR 整合。凡创建 PR，合并方式仍须遵守 7.1，不受本过渡期例外影响。
- **现有发布入口**：过渡期沿用当前 CI 与发布流程。本条不授权自动发布；发布仍须由用户明确指定范围，完成构建、测试和公开下载验证。
- **启用时机**：0.6.3 正式发布并完成 tag 后，后续开发才执行 7.1～7.6 的新工作流；届时核对并整合 `develop` 的既有历史，不删除或强推丢弃过渡期提交。
- 本条优先于下文有关新任务必须从 `main` 创建短期分支、经 PR 合入及退出 `develop` 的规定；提交格式、第三方边界和修改后的验证要求继续生效。

### 7.1 工作流的强制原则

- **主线职责**：`main` 是已整合、应当可构建的开发主线，可以领先于最新正式发布；正式发布身份由不可变 tag 和精确提交 SHA 定义。禁止声称拉取任意时刻的 `main` 就等于网站最新正式版。
- **正式发布一致性**：正式发布 tag、发布构建的源码提交、`main` 上最终发布提交必须是同一个 SHA。网站与 GitHub Release 必须消费同一次成功构建的产物。
- **合并方式**：所有 PR 一律使用普通 **merge commit**（GitHub 的 **Create a merge commit**，CLI 使用 `gh pr merge --merge`），保留分支原始提交和双亲合并提交；功能、修复、文档、构建、版本更新及前向移植均遵守本规则。禁止使用 **Squash and merge** 或 **Rebase and merge**。每个 PR 仍须对应一个完整逻辑改动。
- **仓库规则兼容**：分支保护必须允许普通 merge commit，不得启用与之冲突的 `required_linear_history`；禁止强推、禁止删除主线及要求经 PR 合入等其它保护继续保留。
- **历史保护**：禁止强推 `main`、移动已发布 tag，或在发布后通过 squash/rebase 改写发布提交身份。禁止“先在 ci 发布并打 tag，再将 ci 合回 main 作为正式发布提交”。
- **执行授权**：用户明确要求发布并确定版本、范围后，Agent 必须执行本节的完整发布与验收，不得只构建、只创建 PR、只触发任务或只发布一个渠道就宣称完成。已有授权覆盖本次范围内的提交、推送、PR、合并、CI、tag、网站及 GitHub 发布和安全清理，无需逐步重复确认；未确定的版本类型或发布范围必须先澄清。
- **任务边界**：普通开发、修复、检查和文档修改不自动触发公开发布；本节本身不表示要求立即发布当前工作区。不得夹带用户未选择的未完成功能、降低测试要求或修改版本策略来绕过发布失败。

### 7.2 分支职责与生命周期

| 分支 | 起点 | 职责 | 结束方式 |
| --- | --- | --- | --- |
| `main` | 项目主线 | 保存逐项整合的功能、修复及版本更新提交 | 保护历史，不强推 |
| `feat/<问题>` | 最新 `main` | 一个新功能及其开发期间的修复 | PR 通过 merge commit 合入 `main` 后删除 |
| `fix/<问题>` | 最新 `main` | 一个已有 bug、构建、测试或发布流程问题的修复 | PR 通过 merge commit 合入 `main` 后删除 |
| `release/<版本>` | 已通过候选检查的 `main` | 版本号更新与必要发布准备 | PR 通过 merge commit 合入 `main` 后删除 |
| `hotfix` | 当前正式 release tag | 已发布版本影响使用的紧急修复及资产热更新 | 保留到下一正式版本并及时前向移植修复 |
| `ci` | 需要构建的精确 `main` 提交 | 兼容现有 Runner 的构建指针 | 不保存独有业务修改或版本更新 |

- 不建立永久的 `feature` 或 `bugfix` 汇总分支，不把无关问题堆进一个 PR。`feat/` 与 `fix/` 是短期任务分支的命名前缀。
- 创建任务分支前先 fetch 并核对远端 `main`，不得使用过期本地主线作为默认起点。不得为遵守分支命名而丢弃已有工作。
- `develop` 在 7.0 规定的过渡期继续承载日常提交。新工作流启用后，遗留提交须先核对并经 PR 整合，不得直接删除或强推清空；此后的新任务采用本节规则，不再回到长期 `develop`。
- 合并完成后删除对应短期分支。不要每次发布后重建并强推永久 `feature`、`bugfix` 分支；下一个任务开始时才从最新 `main` 创建新分支。
- 强制更新 `ci` 时只允许使用带明确远端旧 SHA 的 `--force-with-lease`；先确认没有其他构建或发布占用该指针。优先使用接受精确 SHA 的人工 CI 入口，避免不必要的分支强推。

### 7.3 PR、提交信息与依赖

- PR 标题必须按最终 merge commit 的标题编写：`type(scope): 中文描述`，职责明确时可省略 scope。版本更新的分支内提交及 PR 标题必须为 **`ci: 版本更新`**。
- **PR 跳转链接必须保留**：最终 merge commit 标题必须为 **`<PR 标题> (#<实际 PR 编号>)`**，例如 `fix(audio): 修复播放偏移错误 (#128)`；版本更新合并后为 `ci: 版本更新 (#129)`。这里的 `#编号` 是 GitHub 可跳转的 PR 引用，不在 Git 提交标题中写 Markdown 链接；分支原始提交继续保留各自标题。
- PR 标题本身不预填未知编号；合并时使用实际 PR 编号追加一次，禁止遗漏、使用 issue 编号代替 PR 编号或重复追加。GitHub 网站自动生成的正确 `(#编号)` 后缀应保留；Agent 使用 CLI/API 合并时也须显式保证该格式。
- 合并前核对实际 merge commit message，合并后读取最终提交标题再次核验正文和 `(#编号)`。禁止直接接受由多个临时提交自动拼接的默认信息。PR 正文须说明问题、最终行为、验证结果及关联 issue/依赖 PR。
- 必须先完成相关构建、测试与审阅，再确认 PR 已成功合并并读取合并后的实际 SHA；不能用“已提交合并请求”代替“已确认合并”。核对 merge commit 的两个父提交，第二父提交必须为审查通过的 PR head，保留 PR 合并边界和分支提交的完整追溯。
- 新功能依赖已有 bug 修复时，先建立独立 `fix/<问题>` PR 并优先合入 `main`，随后从更新后的 `main` 开发功能。
- 必须并行推进时允许从依赖修复分支创建功能分支，但须标明依赖并暂时将功能 PR 指向依赖分支。修复通过 merge commit 合入后，将功能 PR 目标改回 `main`；需要同步主线时将最新 `main` 合入功能分支，重新检查差异和测试。依赖修复的原始提交已保留在历史中，不得重复 cherry-pick 或再次计为功能改动。
- 冲突必须按代码意图逐项解决并重新验证，不能统一选择 ours/theirs、删测试或删除功能来使合并通过。

### 7.4 热修复、合并时机与前向移植

- **前向移植的定义**：将旧正式版本上的某项修复适配到正在开发下一版本的 `main`，避免同一个 bug 在新版本中再次出现。例如在 `v0.6.3` 的 `hotfix` 修好问题后，将该修复及测试带入开发 `v0.6.4` 的主线。它不等于合并整个旧版本分支，也不意味着把 hotfix 分支变基到新主线。
- `hotfix` 必须从当前已发布的稳定 release tag 初始化；更新 `hotfix-base.json` 的 tag 和精确提交 ID，并在 tag 后的首个维护提交声明基线。必须通过既有基线验证器，禁止为了通过检查而放宽验证。
- 程序热修复从 0.6.3 起使用独立 `hotfix_revision`；0.6.2 到 0.6.3 继续走普通版本号更新。程序与资产各自核对修订严格递增，入口及资产选择遵循 `docs/hotfix.md`。
- **明确合并时机**：每项 hotfix 修复及相关测试完成后，立即准备对应的主线修复 PR；该准备可以与旧版热修复发布并行。紧急旧版发布可以先完成，但同一热修复任务不得在主线移植尚未合并时宣称全部完成，禁止等到下一次正式发布前才集中搬运。
- **执行顺序**：在旧版 `hotfix` 完成修复和测试 → 按用户授权发布旧版热修复 → 从最新 `main` 创建 `fix/<问题>` → 选择性带入该修复及测试，解决与新主线的差异 → 验证并创建指向 `main` 的 PR → 通过 merge commit 合并、确认实际合并提交和 `(#PR编号)` → 记录旧版修复与主线提交的对应关系。路径为 **`hotfix 的修复内容 → fix/<问题> → main`**，不得直接合入功能分支或将整个 `hotfix` 分支合入 `main`。
- 移植可以使用选择性的 cherry-pick 或等价补丁适配；仅复制源码而不补测试、只创建未合并 PR、仅把修复写进发布日志，都不算完成。若主线已经用其他实现解决该问题，应核对并运行回归测试，记录对应主线提交/PR，而不是重复应用旧补丁。
- 当前任务因权限、冲突或其他外部条件无法合入主线时，明确报告“旧版已修复/发布、主线移植未完成”、源提交、待处理 PR 与具体阻塞；下一次正式发布必须在准备版本 PR 之前关闭这些遗漏。未授权旧版公开发布时可以先完成主线移植，但不能擅自触发热修复发布。
- 只前向移植真正的修复和必要测试，不覆盖主线版本号、`hotfix-base.json` 或旧版发布专用配置。保留源 hotfix 提交/PR 的引用，证明下一正式版本包含这些修复。
- 热修复构建记录其自身精确源码 SHA、基准 release tag 和修订号；它不同于普通正式发布提交。不得移动原正式 tag 或覆盖原正式版本的不可变归档来掩盖这种差异。
- 切换新正式版本的 hotfix 基线前，确认旧维护分支没有未保存、未发布或未前向移植的工作及打开的 PR；先保留可恢复引用，再从新 tag 初始化。确需替换远端 `hotfix` 时使用明确旧 SHA 的 `--force-with-lease`，禁止无条件强推。

### 7.5 正式发布的执行顺序

1. **确定范围**：核对用户要求的目标版本和本次要发布的 PR/issue；检查本地改动、远端分支、未完成 PR、hotfix 前向移植及已有 tag/Release。先保留非本次范围的工作。
2. **逐项整合**：按依赖顺序合入已完成的功能和修复 PR，每次确认实际合并结果。运行候选 `main` 的完整 CI 编译矩阵及各平台注册的完整测试；失败不得进入发布准备。
3. **版本 PR**：从候选 `main` 建立 `release/<版本>`，修改最外层 CMake 的版本定义并检查所有派生版本信息。将版本更新独立提交为 `ci: 版本更新`，经 PR 以 merge commit 合入 `main`，确认合并完成。
4. **固定发布身份**：读取版本 PR 合入 `main` 后的精确 merge commit SHA，记为 `R`。后续构建、网站发布、GitHub 发布及 tag 均绑定 `R`，不得临时改为“最新 main”“最新 ci”或“目录中最新产物”。
5. **最终构建**：从 `R` 运行完整发布构建、CI 矩阵、全部平台测试及打包。需要 `ci` 触发时只让它指向 `R`，不再在 `ci` 上提交版本更新。记录 CI run ID、源码 SHA、版本、hotfix 修订、工具链、依赖/预编译 ABI、构建选项及产物 SHA256。
6. **保存产物**：将本次成功产物保存为按版本、SHA 和运行身份可确定的发布集合；核对所有要求的平台、安装包、自动更新主包、独立更新器、资产完整包与清单。不得发布 PGO 采样构建、调试构建或工具链选项不明的产物，除非用户明确选择这种发布类型。
7. **网站发布**：调用现有网站发布入口，确保 checkout 和输入产物都对应 `R`。完成网站、CDN、默认资源及适用的 APT 发布，校验公开下载与本次产物摘要一致，确认更新清单的版本、修订、大小、摘要和链接正确。
8. **GitHub 发布**：使用同一批产物及显式目标 `R` 调用 GitHub Release 入口，创建正确版本 tag 和 Release。确认 tag 解引用后的提交为 `R`，Release 状态正确、资产齐全且公开下载摘要一致。禁止重新构建另一批文件给 GitHub，禁止删除或重建已发布 tag 来处理失败。
9. **完整验收**：确认网站和 GitHub 两个渠道均成功，适用的软件源及客户端更新信息一致；记录发布版本、`R`、CI run ID、PR、tag、Release URL 和验证结论。触发 CI 成功或 Release 创建成功都不能代替下载验收。
10. **清理**：拉取最新 `main` 和 tag，检查 `R` 仍在 `main` 历史中，清理已确认合并的短期任务及发布分支，按 7.4 初始化下一轮 hotfix 基线。不再创建“ci → main”的发布后回合并 PR。

- 版本的 major/minor/patch 是整数分量，按用户指定的发布类型更新；例如补丁 `0.6.9 → 0.6.10`，不是小数进位。无法确定是补丁、功能版本还是不兼容变更时不得自行猜测。
- 若最终构建失败，暂停发布，从当前 `main` 建立新的 `fix/<问题>` PR，逐项修复或按授权撤销错误改动并重新确认合并。读取新的 `R` 后重新完整构建；旧失败提交的产物不得沿用，不需要重复增加版本号。
- 本地构建通过不能替代跨平台 CI；平台失败不能以排除平台、跳过测试或使用旧二进制绕过。
- 现有脚本若仍固定同步某个分支、读取共享 `release` 目录或无法核对产物来源，Agent 必须先核验并在本次授权范围内补齐来源一致性保护；禁止只因旧脚本返回零就声称符合本节发布契约。

### 7.6 发布失败、重试与自动化边界

- 发布过程中 `main` 出现新提交不改变 `R`。其他任务不得移动本次使用的 ci 指针或改写已保存的产物集合；共享 Runner、目录和远端清单须串行保护。
- 某一发布渠道失败时明确报告已完成与未完成的阶段；核对远端实际状态，重试同一个 `R` 和同一批产物，不盲目重新创建 tag/Release、不覆盖另一轮发布、不删除成功上传的历史对象。
- 两个渠道无法原子发布，必须保留可恢复的清单和产物身份，核对部分成功状态。权限、网络或外部服务阻塞时如实报告具体阻塞，禁止以“已完成发布”结束。
- 用户已授权完整发布时，Agent 自主完成机械步骤、等待 CI、处理范围内的编译/测试失败并完成验收；不得反复询问已经明确的合并、触发 CI、打 tag 和发布操作。超出范围的功能修改、版本类型变更或破坏性处理须重新取得明确指示。
- 仅删除本次确认已合并且不再需要的分支或临时文件；不得丢弃其他任务、贡献者、未合并 PR、工作树或 stash 中的内容。清理须保持发布 tag 和历史制品可追溯。
- 源码 SHA 一致是发布追溯的最低要求，不等于任意环境都能生成逐字节相同的二进制。向 Gentoo 等发行方提供不可变 tag/源码归档及构建信息；追踪 `main` 的包属于开发版，严格可复现结论必须由相同环境与产物哈希验证。

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
