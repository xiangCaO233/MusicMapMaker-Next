# 头文件依赖隔离：当前状态

日期：2026-10-24。分支：`fix/header-dependency-isolation`。

## 范围与测量

目标是减少实现或私有状态修改时触及的编译单元，不改变运行行为或配置值语义。
使用现有 Linux Debug / Clang 23 / `SOURCES_BUILD=OFF` 构建树和全部已注册测试。
`scripts/dev/header_fanout.py` 从 Ninja 依赖中筛选当前 compile_commands 的活动对象，按头文件去重计数。
依赖数量不是编译耗时比例；本任务未测量冷缓存全量构建耗时。
先前设计、提交和各批次测量保存在 [历史记录](archive/header-dependency-isolation-history-2026-10-24.md)。

## 已完成的主要边界

- 引擎和会话公开接口隔离完整命令载荷；会话队列、控制器及监听器私有状态不再传播实现布局。
- AppConfig 保持配置对象地址稳定，重定义和访问实现在源文件，不向所有消费者传播完整设置。
- 布局作用域、字体默认值、时间格式化和快捷键运行实现移出公开聚合头。
- 同步参数、音效、快捷键配置提供有真实消费者的轻量值类型头；原聚合配置继续按值包含它们。

## 当前批次

基线为已提交的 `986139d3`，本批新增优化尚未提交。

- `SessionUtils.h` 仅保留参数所需的轻量标准库/ECS 声明与前向声明。
  上下文、音符/时间点组件、音频时钟和配置定义由读取字段的源文件直接包含；缓存维护和时间计算函数体不变。
- `ProjectWorkspaceState.h` 独立保存工作区 DTO；`ProjectSettings` 继续按值包含工作区及可选项目覆盖配置。
  字段顺序、类型、默认值、JSON 声明和对象拷贝行为不变，不新增堆分配或共享所有权。
- `ObjectPlacementSnap.h` 提供共用吸附枚举、默认掩码和位操作；配置与工作区使用同一套常量。
- `IEditorApplicationService` 使用独立工作区数据，配置返回类型仅前向声明；虚函数签名和应用端口行为不变。

## 当前依赖范围

本批前后活动对象均为 516/516；没有增加编译单元。

| 头文件 | 批次前 | 当前 |
| --- | ---: | ---: |
| `ProjectSettings.h` | 74 | 66 |
| `EditorConfig.h` | 153 | 151 |
| `EditorSettings.h` | 225 | 224 |
| `VisualConfig.h` | 186 | 184 |
| `SessionContext.h` | 57 | 55 |
| `NoteComponent.h` | 81 | 79 |
| `TimelineComponent.h` | 74 | 73 |
| `SessionUtils.h` | 26 | 26 |
| `IEditorApplicationService.h` | 9 | 9 |

工作区 DTO 的定义仍影响其按值消费者（独立头当前为 74 个），但项目覆盖配置的修改现在少触及八个消费者。
SessionUtils 的公开签名仍影响原来的 26 个对象；去掉隐式传递后，完整上下文少触及两个对象。
Project 本身仍有 62 个消费者，尚未改变其公开按值数据模型，不能声称已隔离全部项目状态。

## 验证边界

- Linux Debug / Clang 23 完整构建成功，CTest 172/172 通过；工作区/吸附定义与基线的代码 token 对比一致。
- 五个公开头独立、重复包含语法检查通过：SessionUtils、ObjectPlacementSnap、ProjectWorkspaceState、IEditorApplicationService、ProjectSettings。
- 依赖文件确认 SessionUtils 不包含完整会话、组件、设置或 entt 聚合头；工作区和应用端口不包含完整编辑器/视觉配置。
- 未增加运行时测试或修改缓存/序列化算法；复用已有分拍吸附、拍号、时间线、项目元数据及工作区停靠回归。
- 未执行 Windows/macOS 平台构建；不声称冷构建耗时提升。

## 本批逐文件注释率

使用 `cloc --by-file` 的 comment / (comment + code) 统计；本批所有源文件均达到 30%。

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Config/include/config/EditorSettings.h` | 236 | 330 | 41.70% |
| `Modules/Config/include/config/ObjectPlacementSnap.h` | 23 | 52 | 30.67% |
| `Modules/Game/Logic/include/logic/session/SessionUtils.h` | 133 | 94 | 58.59% |
| `Modules/Game/Logic/src/logic/session/SessionUtils_Loading.cpp` | 312 | 728 | 30.00% |
| `Modules/Game/UI/include/ui/IEditorApplicationService.h` | 35 | 45 | 43.75% |
| `Modules/Game/UI/src/ui/imgui/CanvasTabManager.cpp` | 117 | 272 | 30.08% |
| `Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp` | 594 | 1385 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/OpenBeatmapSpeedExportAction.cpp` | 303 | 702 | 30.15% |
| `Modules/Game/UI/tests/WorkspaceDockRestoreTest.cpp` | 398 | 928 | 30.02% |
| `Modules/MMM/include/mmm/project/ProjectSettings.h` | 25 | 22 | 53.19% |
| `Modules/MMM/include/mmm/project/ProjectWorkspaceState.h` | 110 | 109 | 50.23% |

## 后续优先级

1. 剩余 SessionContext 依赖中，查找只需 CameraInfo 或只需结构化工具结果的消费者；按真实消费者决定是否拆相机数据，避免无收益地拆字段。
2. Project 公开成员是按值协议，单独的元数据/入口 DTO 目前没有独立公开头消费者。继续检查跨模块应用端口的数据粒度，优先缩小只读查询返回值，避免为所有项目配置增加 PImpl 或深拷贝。
3. 调色文件入口三个消费者、RenderSyncRegistry 四个消费者，收益较低，暂不扩大重构。
