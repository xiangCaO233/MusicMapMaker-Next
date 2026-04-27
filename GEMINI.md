# MusicMapMaker-Next

一个使用 C++20/23 和 Vulkan 构建的现代、高性能节奏游戏谱面编辑器。

## 项目概述

- **项目目的**: 下一代节奏游戏谱面编辑器。
- **技术栈**:
  - **编程语言**: 现代 C++ (C++20/C++23)
  - **图形 API**: Vulkan (通过 `<vulkan/vulkan.hpp>`), GLFW
  - **UI 与布局**: ImGui 和 Clay
  - **ECS (实体组件系统)**: EnTT
  - **脚本语言**: LuaJIT (通过 `sol2` 绑定)
  - **音频引擎**: `IonCachyEngine` (自定义构建)
  - **数学库**: GLM
  - **日志库**: `spdlog` 和 `fmt` (封装为自定义宏)

## 目录结构

- `Modules/`: 项目核心模块。
  - `Audio/`: 音频管理与集成。
  - `Config/`: 配置与皮肤加载。
  - `Event/`: 事件系统。
  - `Game/`: 主编辑器逻辑，包含 UI、图形渲染和 ECS 系统。
  - `Log/`: 日志实用程序。
  - `Main/`: 入口点 (`main.cpp`)。
  - `MMM/`: 核心数据结构 (BeatMap, Note, Hold, Timing)。
- `3rdpty/`: 第三方依赖。
  - **警告**: 除非是 `3rdpty/sources/IonCachyEngine`，否则不要修改此目录下的文件。
- `assets/`: 纹理、皮肤和其他资源。
- `scripts/`: 环境初始化和实用脚本。

## 构建与运行

### 环境要求
- CMake 3.31+
- Ninja (推荐)
- Vulkan SDK
- 兼容 C++20/23 的编译器 (GCC/Clang/MSVC)

### 构建命令
```bash
# 配置项目
cmake -S . -B build -G Ninja

# 编译项目
cmake --build build

# 清理构建
cmake --build build --target clean
```

### 测试规范
- 项目目前没有自动化测试套件。
- 验证方式：确保项目构建成功，并手动验证编译后的二进制文件。
- 如需验证逻辑，请编写独立的最小化 `.cpp` 测试脚本。

## 开发规范

### 代码风格
- **命名空间/类/结构体**: `PascalCase` 大驼峰 (例如 `MMM::UI`, `BeatmapSession`)。
- **方法/函数/变量**: `camelCase` 小驼峰 (例如 `loadSkin()`, `m_data`)。
- **成员变量**: 以 `m_` 为前缀 (例如 `m_themeName`)。
- **常量/宏**: `UPPER_SNAKE_CASE` 全大写下划线 (例如 `XINFO`)。
- **格式化**: 严格遵守 `.clang-format`。修改文件后请运行 `clang-format -i <file>`。
- **缩进**: 4 个空格。

### C++ 使用准则
- **禁用异常**: 严禁使用 `try`, `catch`, 或 `throw`。使用 `std::expected` 或 `std::optional` 进行错误处理。
- **禁用原始指针**: 避免使用 `new` 和 `delete`。使用智能指针 (`std::unique_ptr`, `std::shared_ptr`)。
- **Doxygen 注释**: 所有类和函数必须使用 Doxygen 风格的注释 (例如 `/// @brief ...`)。

### 日志规范
- 使用 `"log/colorful-log.h"` 中的自定义宏，严禁使用 `std::cout` 或 `printf`。
- 宏命令：`XINFO(...)`, `XWARN(...)`, `XERROR(...)`, `XDEBUG(...)`。

### UI 实现规范
- 对于标题动态变化的 ImGui 窗口，使用 `###` 固定内部 ID (例如 `ImGui::Begin("标题###StaticID")`)。
- UI 模块可以依赖 Graphic 模块，但 Graphic 模块严禁依赖 UI 高层结构。

### Git 提交格式
- 格式: `type(scope): 中文描述`
- `type` 必须使用英文 (例如 `feat`, `fix`, `refactor`)。
- 冒号后的描述必须使用中文。
- 示例: `feat(render): 重构渲染管线`
