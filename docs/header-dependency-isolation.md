# 头文件依赖隔离：测量与第二轮优化

记录日期：2026-10-24。分支：`fix/header-dependency-isolation`，本轮基线为 `27adb34e`。

## 测量口径

- 使用现有 Linux Debug 构建树、Clang 23、`SOURCES_BUILD=OFF`，包含主程序及全部已注册测试。
- `compile_commands.json` 中共有 514 个编译单元，修改前后目标集合相同。
- 从 `ninja -t deps` 读取编译器实际登记的依赖，再按当前编译数据库过滤对象文件。
- 同一对象经多条包含链访问一个头文件时只计一次；切分支后遗留的非活动 Ninja 记录不计入。
- 全量构建完成后重新统计，避免把未重建的旧记录当成优化结果。报告中已记录目标数为 514/514。
- 以下数字表示“修改该头会影响多少个编译单元”，不代表耗时比例，也不能把多个头的减少量简单相加。
- 本轮未测冷缓存全量构建时间；不以缓存命中率、链接时间或机器负载差异推导编译加速百分比。

## 已落地的结果

| 头文件 | 修改前 | 修改后 | 减少 |
| --- | ---: | ---: | ---: |
| `ui/layout/CLayWrapperCore.h` | 97 | 7 | 90（92.8%） |
| Clay 入口 `clay.h` | 98 | 37 | 61（62.2%） |
| `common/AsciiFontData.h` | 141 | 69 | 72（51.1%） |
| `common/UnicodeFontData.h` | 141 | 69 | 72（51.1%） |
| `common/render/RenderSnapshot.h` | 106 | 64 | 42（39.6%） |
| `common/render/CanvasRenderTypes.h` | 116 | 74 | 42（36.2%） |
| `common/render/CanvasThicknessCompensation.h` | 106 | 64 | 42（39.6%） |
| `common/render/TimingInterpolationRenderData.h` | 108 | 80 | 28（25.9%） |
| `common/FontConstants.h` | 142 | 111 | 31（21.8%） |

这里统计 Clay 等第三方头时仅检查编译器依赖路径，没有修改或分析第三方源码。

### 1. 解除会话状态对完整渲染快照的依赖

原链路：

```text
SessionContext.h
  -> RenderSnapshot.h（只为了嵌套的 WalkthroughEditEvent）
     -> 字体度量、画布顶点、绘制批次、补偿与插值渲染数据
```

`SessionContext` 实际只需要保存六个按值的教学编辑事件字段。本轮把该类型提取到
`common/walkthrough/WalkthroughEditEvent.h`，逻辑会话、画笔及拖动工具直接使用它。
`RenderSnapshot::WalkthroughEditEvent` 保留别名，保持已有快照调用点和字段布局。

`CanvasComponentRenderSystem.h` 只借用快照指针，也改为前向声明；完整定义由真正写入快照的 `.cpp` 包含。
没有把事件或快照改成共享指针，没有引入逐帧分配和额外同步。

### 2. 基础视图隔离持久 Clay 状态

原链路：

```text
大量 UI 及菜单实现 -> UIManager.h / 各视图头 -> IUIView.h
  -> CLayWrapperCore.h -> clay.h
```

- `IUIView` 的 Clay 上下文和 Arena 移到私有 `LayoutState`，构造时分配一次，完整析构定义留在 `.cpp`。
- 每帧 `LayoutContext` 仍放在栈上，拆到 `ui/layout/LayoutContext.h`；它借用 `IUIView&` 激活已有上下文。
- 原先直接切换 Arena 的侧栏和音轨面板改用 `makeLayoutCurrent()`，不暴露私有状态布局。
- 真实使用 ImGui 值类型的头文件补充自己的包含，访问单帧布局区域的实现显式包含新头。
- 基础视图头不再包含 Clay 或 ImGui。实际调用 Clay 布局 DSL 的代码继续保留其真实依赖。

这是适合 PImpl 的对象生命周期：窗口长期拥有私有资源，不能把轻量绘制 DTO 或逐帧快照按同样方式堆分配。

### 3. 默认参数不再强迫调用方包含字体定义

原来的 `EditorEngine::setAtlasUVMap` 对两个字体度量引用使用 `{}` 默认实参，
因此即使调用方只更新 UV，`EditorEngine.h` 也必须包含完整的 ASCII/Unicode 字体数据定义。

改为等价的二、三、四参数重载，缺省空度量只在实现文件内构造；完整四参数入口仍执行同一次注册表发布。
公开头仅前向声明字体类型。调用参数、空字体语义、UV 与字体的同步修订不变。
注册表在同步调用内复制数据，不会保留临时默认对象的栈地址。

## 下一批候选及优先级

下面是本轮结束时仍存在的传播面；这里只登记后续切入点，不声称已经消除。

| 优先级 | 入口及当前影响范围 | 原因与建议 |
| --- | --- | --- |
| 高 | `EditorSettings.h` 243，`VisualConfig.h` 232，`EditorConfig.h` 208 | 完整配置是按值协议，经过 `AppConfig`、项目设置及命令聚合头传播。先按真实使用拆分偏好枚举和纯算法；不能为少解析一个头就给所有配置字段增加堆分配。 |
| 高 | `LogicCommands.h` 117，`EditorEngine.h` 80 | 引擎只为 `pushCommand(LogicCommand&&)` 暴露整份变体；变体中 `CmdUpdateEditorConfig` 又按值携带完整配置。需研究独立的命令类型声明边界，并同步隔离会话队列，保持命令按值及跨线程所有权。不能仅拆文件后又让聚合头全量包含。 |
| 中 | `AppConfig.h` 123 | 许多调用只需 UI 缩放或设备标量，却通过内联 getter 读取完整配置。可优先把跨域内联访问移到实现，再按消费者实际需求拆接口。若扩大 PImpl，必须保持配置引用地址稳定，并记录热路径间接访问成本。 |
| 中 | `TimeFormatUtils.h` 15 | 大量格式化算法内联在公开头，同时包含完整 `RenderSnapshot`、`AppConfig` 和设置。可保留轻量时间上下文与函数声明，将依赖快照的 BPM 提取及字符串格式化移到实现，复用已有按版本缓存。 |
| 中 | `ProjectDirectoryWatcher.h` 19，`ProjectController.h` 17 | 目录监听器按值传播 future、stop token、锁和平台句柄。可把线程及平台状态私有化；高频变更门闩继续直接读取，不能在消费变更时新增长锁等待。 |
| 低 | `RenderSyncRegistry.h` 4 | 适合隔离容器和私有发布状态，但已由引擎 PImpl 限制到少数实现及测试。收益小于上述跨模块入口，不应优先扩大改动。 |

### 暂不采用的做法

- 不给 `SessionContext` 整体套一层 PImpl：真正逐帧读取 ECS 和编辑字段的系统仍需要完整定义，盲目套壳不能消除这些真实依赖。
- 不把 `EditorConfig`、渲染快照、单个音符等值对象改为 `shared_ptr` 来追求表面的包含数下降。
- 不用预编译头或 unity build 掩盖传播链。本轮目标是修改头文件后减少受影响编译单元，而非只加快首次解析。
- 不把原有真实依赖从 `.h` 删除后寄希望于另一个聚合头碰巧提供定义。13 个公开头已经逐个独立包含编译。

## 验证与复测

- 满线程构建成功：`cmake --build build --parallel "$(nproc)"`。
- 全部 CTest 通过：172/172，包含画布、教学、配置和 UI 生命周期相关测试。
- 13 个本轮相关公开头分别重复包含并独立编译通过。
- 依赖探针确认 `IUIView.h` 不包含 Clay/ImGui，`EditorEngine.h` 不包含字体数据，`SessionContext.h` 不包含完整渲染快照、画布顶点和字体数据。
- 本机验证不替代 macOS/Windows CI，也未据此声称实际 UI 帧率或冷构建耗时得到改善。

复用的统计入口：

```bash
python3 scripts/dev/header_fanout.py --build-dir build --output build/header-fanout-report.json
```

先保存修改前报告，完成同配置构建后再输出修改后报告。原始本轮数据保存在构建树的
`header-fanout-before.json`、`header-fanout-after.json` 和对应 Ninja deps 文本；
独立头探针及依赖文件在 `build/test_output/header-isolation/`，不进入源码资源目录。

## 本批文件注释统计

仅统计本轮新增和修改的自维护代码文件；文档和生成探针不计入。
口径为 `cloc --by-file` 的 `comment / (comment + code)`，全部文件达到 30%。

| 文件 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas.cpp` | 1131 | 2638 | 30.01% |
| `Modules/Game/Canvas/src/PreviewCanvas.cpp` | 323 | 752 | 30.05% |
| `Modules/Game/Canvas/src/TimelineCanvas.cpp` | 550 | 1281 | 30.04% |
| `Modules/Game/Common/include/common/render/RenderSnapshot.h` | 171 | 366 | 31.84% |
| `Modules/Game/Common/include/common/walkthrough/WalkthroughEditEvent.h` | 9 | 15 | 37.50% |
| `Modules/Game/Logic/include/logic/EditorEngine.h` | 345 | 222 | 60.85% |
| `Modules/Game/Logic/include/logic/ecs/system/CanvasComponentRenderSystem.h` | 44 | 48 | 47.83% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 100 | 221 | 31.15% |
| `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1626 | 3794 | 30.00% |
| `Modules/Game/Logic/src/logic/ecs/system/render/CanvasComponentRenderSystem.cpp` | 223 | 519 | 30.05% |
| `Modules/Game/Logic/src/logic/session/tool/DrawTool.cpp` | 653 | 1523 | 30.01% |
| `Modules/Game/Logic/src/logic/session/tool/GrabTool.cpp` | 757 | 1764 | 30.03% |
| `Modules/Game/UI/include/ui/IRenderableView.h` | 75 | 149 | 33.48% |
| `Modules/Game/UI/include/ui/IUIView.h` | 27 | 43 | 38.57% |
| `Modules/Game/UI/include/ui/UIManager.h` | 169 | 173 | 49.42% |
| `Modules/Game/UI/include/ui/imgui/CanvasTabManager.h` | 42 | 62 | 40.38% |
| `Modules/Game/UI/include/ui/imgui/MainDockSpaceUI.h` | 69 | 104 | 39.88% |
| `Modules/Game/UI/include/ui/imgui/manager/ProjectAudioToolView.h` | 160 | 166 | 49.08% |
| `Modules/Game/UI/include/ui/imgui/menu/actions/tools/BpmMeasurementToolView.h` | 383 | 303 | 55.83% |
| `Modules/Game/UI/include/ui/layout/LayoutContext.h` | 11 | 22 | 33.33% |
| `Modules/Game/UI/src/ui/IUIView.cpp` | 41 | 85 | 32.54% |
| `Modules/Game/UI/src/ui/imgui/FloatingManagerUI.cpp` | 556 | 1288 | 30.15% |
| `Modules/Game/UI/src/ui/imgui/SideBarUI.cpp` | 160 | 372 | 30.08% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp` | 474 | 1103 | 30.06% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioTrackControllerUI.cpp` | 116 | 270 | 30.05% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp` | 221 | 508 | 30.32% |
| `Modules/Game/UI/src/ui/imgui/manager/AudioManagerView.cpp` | 727 | 1695 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/manager/BeatMapManagerView.cpp` | 396 | 918 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView.cpp` | 69 | 153 | 31.08% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView_Hierarchy.cpp` | 604 | 1409 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView_Interactions.cpp` | 104 | 241 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/manager/ProjectAudioToolView.cpp` | 879 | 2049 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView.cpp` | 387 | 896 | 30.16% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/BpmMeasurementToolView.cpp` | 1560 | 3633 | 30.04% |
| `Modules/Game/UI/src/ui/layout/box/CLayBox.cpp` | 73 | 167 | 30.42% |
| `scripts/dev/header_fanout.py` | 27 | 61 | 30.68% |
