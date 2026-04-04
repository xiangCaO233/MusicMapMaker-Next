#MusicMapMaker - Next 智能体指令(Agent Instructions)

## 简介
本文档为在 `MusicMapMaker-Next` 仓库中运行的 AI 编程智能体提供核心规则和指南。请严格遵守这些指令，以确保代码修改的安全、自然和符合项目惯例。

## 项目架构与技术栈
- **语言**: 现代 C++ (C++20/C++23)
- **构建系统**: CMake (最低版本 3.31)
- **UI 与布局**: ImGui 和 Clay
- **图形 API**: Vulkan (`<vulkan/vulkan.hpp>`), GLFW
- **脚本语言**: LuaJIT (通过 `sol2` 绑定 `<sol/sol.hpp>`)
- **音频引擎**: 自定义 `IonCachyEngine` (位于 `3rdpty/sources/IonCachyEngine`)
- **数学库**: GLM (`<glm/glm.hpp>`)
- **日志库**: `spdlog` 和 `fmt` (已封装为自定义宏)

## 目录结构限制
- **`Modules/`**: 包含项目核心模块 (`Audio`, `Config`, `Event`, `Game`, `Log`, `Main`)。主要业务逻辑均位于此处。
- **`3rdpty/`**: 包含第三方依赖。
  - **严重警告**: **绝对不要** 修改或分析 `3rdpty/` 下的任何代码库，**除了** `3rdpty/sources/IonCachyEngine`。
  - 其他所有库（如 stb, glm, lunasvg 等）均为 GitHub 原始克隆，未做任何修改。修改它们会破坏构建过程或产生未跟踪的下游补丁。
  - `IonCachyEngine` 是我编写的自定义核心引擎部分，应采用与项目其他部分相同的代码风格规则。

## 构建、代码检查和测试命令

### 1. 构建项目
必须使用绝对路径进行文件操作，但在运行 shell 命令时，请在项目根目录执行。

- **配置**: `cmake -S . -B build -G Ninja`
- **构建**: `cmake --build build`
- **清理**: `cmake --build build --target clean`

### 2. 测试 (非常重要)
- **警告**: 该项目目前 **没有** 集成自动化测试框架，也没有测试套件。
- **不要尝试** 运行标准测试命令，如 `ctest`、`make test`。
- **验证方式**: 修改代码后，必须确保项目构建成功 (`cmake --build build`)。如需验证逻辑，请编写独立的最小 `.cpp` 测试脚本，或要求用户运行编译后的程序验证。

### 3. 代码格式化
- 任何修改过的 `.cpp`, `.h`, 或 `.hpp` 文件，必须在完成前运行 `clang-format -i <absolute_file_path>`。

## 代码风格与核心规范

### 1. 模块依赖与解耦 (CRITICAL)
- **依赖方向**: `UI` 模块可以依赖 `Graphic` 模块，但 `Graphic` 模块**严禁**直接引用 `UI` 模块的高层结构（如 `Brush` 或 `DrawCmd`）。
- **抽象回调**: 底层渲染器若需执行 UI 绘制，必须通过抽象接口或虚函数回调（如 `onRecordDrawCmds`）实现。
- **命名空间**: 核心业务 UI 逻辑使用 `MMM::UI`；底层图形封装使用 `MMM::Graphic`。

### 2. 类继承与内存规范
- **虚继承**: 在实现多重继承的 UI 视图（如同时继承 `ITextureLoader` 和 `IUIView`）时，**必须**对基类 `IUIView` 使用虚继承（`virtual public IUIView`），以避免成员歧义。
- **严格禁用的 C++ 特性**: 
    - **绝对禁止使用 C++ 异常机制**: 严禁使用 `throw`、`try` 和 `catch`。必须依赖返回值（`std::expected` 或 `std::optional`）。
    - **绝对禁止使用原始指针分配**: 严禁使用 `new` 和 `delete`。必须使用智能指针。

### 3. UI 与渲染规范
- **ImGui 停靠稳定性**: 对于标题动态变化的窗口，必须使用 `###` 符号固定内部 ID（例如 `ImGui::Begin("标题###StaticID")`）。
- **批处理性能**: `Brush` 绘制时应减少纹理切换，合并相同状态的 Draw Call。

### 4. 注释规范 (Doxygen)
- **强制使用 Doxygen 风格**: 任何新增或修改的类、函数、成员变量都必须使用 Doxygen 风格进行注释（如 `/// @brief ...`）。

### 5. 命名规范
- **命名空间**: `PascalCase` 大驼峰 (例如 `MMM`, `Config`)。
- **类和结构体**: `PascalCase` 大驼峰。
- **方法和函数**: `camelCase` 小驼峰。
- **变量**: `camelCase` 小驼峰。**成员变量**必须以 `m_` 开头 (例如 `m_data`)。
- **常量和宏**: `UPPER_SNAKE_CASE` (例如 `XINFO`)。

### 格式化与包含
- **缩进**: 4个空格。大括号换行规则遵循项目的 `.clang-format`（函数/类换行，控制流不换行，括号内需留有空格如 `if ( a == b )`）。
- **Includes**: 优先包含对应头文件，其次项目头文件，然后第三方，最后标准库。使用 `#pragma once`。

### 日志
- **严禁使用 `std::cout` 或 `printf`**。必须使用 `"log/colorful-log.h"` 中的自定义日志宏：
  ```cpp
  XINFO("Skin loaded: {}", m_data.themeName);
XERROR("Failed to load skin lua: {}", err.what());
```

### Git 提交规范
- **格式**: `type(scope): 中文描述`
- **要求**: 冒号 (`:`) 之前的部分必须使用英文（如 `feat`, `fix`, `refactor`, `docs` 等），冒号之后的部分必须使用中文描述。
- **示例**: `feat(render): 重构渲染管线`

    ##智能体最佳实践

    1. *
    *善用联网搜索防过时** : 对于本项目中使用的现代化、更新较快的第三方库（如
                                Vulkan,
    sol2, ImGui,
    Clay 等），在编写调用代码前，** 务必使用你的联网 /
            搜索工具确认最新的函数签名和最佳实践**，严防使用已过时或消失的
                API。 2. *
            *谋定而后动** : 在修改前，广泛使用 `glob` 和 `grep`了解项目。 3. *
            *保持纯粹** : 仅针对用户需求做最小化修改，不要进行未授权的大规模重构。绝对不要在代码里向用户写对话式的注释。

                          ##当前进度与备忘(Current Status)

                              ## #1. 已完成里程碑
        -
        **模块解耦** : `Graphic` 与 `UI` 模块已通过抽象回调 `onRecordDrawCmds` 彻底解耦。
        - **命名空间规范** : UI 相关逻辑已全部迁移至 `MMM::UI`。 -
        **高性能渲染** : `Brush` 类实现了状态机 API
    和自动批处理（Batching），支持矩形和圆形几何生成，Vulkan 渲染性能大幅提升。
        -
        **UI
         框架稳固** : 修复了 ImGui
                      动态标题导致的停靠丢失问题，并解决了多重继承下的菱形继承冲突。
        -
        **ECS
         交互层** : 引入 `entt` 库，建立基于 ECS
                    的拾取与交互系统。支持音符的悬停高亮、点击选择以及跨线程鼠标拖拽（实时更新时间戳与轨道）。
        -
        **音符渲染优化** : 实现了
                           Polyline（折线）的连续几何生成，支持斜向连接段和纹理批处理。
        -
        **判定线实现** : 在 `NoteRenderSystem` 中引入了判定线绘制逻辑，支持通过 `EditorConfig` 动态配置位置和线宽。
        -
        **核心模块重构** : 完成了 `BeatmapSession` 和 `NoteRenderSystem` 的职责拆分与解耦。建立了 `session/` 和 `render/` 子目录，并将内部批处理器提取为独立的 `Batcher.h`，提升了代码的可维护性和整洁度。

                           ## #2. 待办事项(Next Steps) -
        **脚本集成** : 将 `Brush` API 与 ECS 交互事件暴露给 Lua 环境。 -
        **性能压测** : 针对高频几何更新场景进行压力测试。 -
        **撤销 / 重做系统** : 基于指令队列实现编辑操作的 Undo / Redo 功能。 -
        **多摄像机支持** : 验证并完善多视口下的交互一致性。
