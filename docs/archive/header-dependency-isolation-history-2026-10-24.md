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

下面记录第二轮（`899b72e7`）结束时的候选。第三轮已处理其中的命令、配置入口和同步参数，最新数据见文末。

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

## 第三轮：配置与命令聚合边界

本轮基线：`899b72e7`。继续使用同一构建树及 514 个编译单元，原始报告为
`build/header-fanout-round3-before.json` 和 `build/header-fanout-round3-after.json`。

### 结果

| 头文件 | 第三轮前 | 第三轮后 |
| --- | ---: | ---: |
| `LogicCommands.h` | 117 | 89 |
| `EditorConfig.h` | 208 | 153 |
| `VisualConfig.h` | 232 | 186 |
| `EditorSettings.h` | 243 | 228 |
| `TimingTemplate.h` | 120 | 93 |
| `ComposeLessonNotes.h` | 122 | 95 |
| `BaseMapMeta.h` | 160 | 140 |
| 第三方队列入口 `concurrentqueue.h` | 53 | 17 |

`AppConfig.h` 仍由 123 个编译单元使用，其接口本身没有消失；变化是它不再把
完整配置定义传播给这些消费者。`EditorSettings.h` 仍有大量真正的字段访问者，
不能把完整配置边界的改善描述成所有配置修改都已局部化。

### 命令声明与载荷分离

- 新增 `common/LogicCommandFwd.h`：前向声明命令类型并保留原有 91 项 `std::variant` 类型及顺序。
- `LogicCommands.h` 仍定义全部按值载荷。构造、分派、销毁命令的实现必须显式包含它；只接收引用的引擎和会话接口使用声明头。
- `BeatmapSession::CommandState` 隔离无锁队列、延后文件命令和延后权威替换，避免公开会话头实例化完整命令变体。
- 私有状态只在会话构造时分配一次，命令继续保存在既有队列中；没有把逐个命令改为共享指针，没有增加逐命令的包装分配。
- 入队门闩、队列发布、文件重试顺序和权威替换合并算法保持原样。延后文件与权威替换字段仍由逻辑线程独占。

### 完整配置并入既有私有存储

- `AppConfig` 原有预设私有状态扩展为 `State`，其中按值拥有 `EditorConfig` 和两类预设。
- 复用原来的一次私有状态分配，不为视觉配置、编辑设置分别新建堆对象，也不更换 `load/reset` 的存储地址。
- 配置 getter 与两个读取用户倍率的缩放 getter 移到实现文件；公开头只前向声明配置类型。
- UI 热路径仍只借用稳定引用，新增成本为稳定状态指针的解引用；没有新增锁、原子操作或每帧分配。
- 各实际访问配置字段的实现显式包含所需的数据定义，避免把丢掉的依赖偷偷转交给其他聚合头。

### 同步参数独立

`SyncMode`、`SyncConfig` 及 ADL JSON 声明移到 `config/SyncConfig.h`。
`PlaybackVisualClock.h` 只前向声明参数类型，时钟实现和测试显式包含独立定义；
内联旧时钟只包含这一小组参数。枚举数值、字段、默认值和 JSON 键保持不变。

### 验证

- 满线程完整构建成功，全部 172 项 CTest 通过。
- 扩展已有音频预设磁盘往返用例：核对五个配置/预设对象在 reset、save、load 后的地址稳定性，验证 const getter 及实时 UI 缩放倍率。
- 8 个相关公开头重复包含并独立编译通过。
- 依赖探针确认引擎、会话和单例接口不包含完整命令载荷或完整配置；公开会话头不再包含第三方命令队列。
- Clang 23 和本机 GCC 均通过命令兼容探针：声明头可先独立包含，完整命令随后包含；变体类型与基线完全相同、备选仍为 91 项。
- Linux 主机构建不替代 macOS/Windows CI，不据此声称已经证明跨平台编译或冷构建耗时百分比。

### 后续顺序

高优先级入口已完成首轮边界拆分。下一步可依次处理：

1. `TimeFormatUtils.h` 的快照提取和字符串格式化实现外移，保留调用方已有的时间上下文缓存。
2. `ProjectDirectoryWatcher` 隔离线程与平台句柄；高频变更门闩继续直接读取。
3. 按实际消费者继续拆分设置值组及纯算法。仅移动文件后仍经聚合头全量包含的方案不计为完成。

### 第三轮逐文件注释统计

统计范围为本轮新增和修改的 124 个代码文件，全部达到 30%。
大量消费者仅调整直接包含；下面保留逐文件结果，供之后的批次复核。

| 文件 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Audio/src/AudioManager.cpp` | 257 | 597 | 30.09% |
| `Modules/Audio/src/AudioManager_BGM.cpp` | 274 | 639 | 30.01% |
| `Modules/Audio/src/AudioManager_Mix.cpp` | 123 | 277 | 30.75% |
| `Modules/Audio/src/AudioManager_Presets.cpp` | 32 | 74 | 30.19% |
| `Modules/Audio/src/AudioManager_SFX.cpp` | 308 | 718 | 30.02% |
| `Modules/Audio/tests/AudioManagerTimelineIntegrationTest.cpp` | 310 | 723 | 30.01% |
| `Modules/Audio/tests/AudioPresetApplyTest.cpp` | 38 | 87 | 30.40% |
| `Modules/Config/include/config/AppConfig.h` | 37 | 60 | 38.14% |
| `Modules/Config/include/config/EditorSettings.h` | 314 | 453 | 40.94% |
| `Modules/Config/include/config/SyncConfig.h` | 9 | 20 | 31.03% |
| `Modules/Config/src/AppConfig.cpp` | 116 | 222 | 34.32% |
| `Modules/Config/src/EditorSettingTypes.cpp` | 411 | 957 | 30.04% |
| `Modules/Config/src/skin/SkinLoader.cpp` | 293 | 678 | 30.18% |
| `Modules/Config/tests/AudioPresetsTest.cpp` | 56 | 129 | 30.27% |
| `Modules/Config/tests/LayoutPresetsTest.cpp` | 147 | 343 | 30.00% |
| `Modules/Game/Canvas/src/AnnotationTableWindow.cpp` | 248 | 577 | 30.06% |
| `Modules/Game/Canvas/src/Basic2DCanvas.cpp` | 1132 | 2641 | 30.00% |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 1944 | 4516 | 30.09% |
| `Modules/Game/Canvas/src/Basic2DCanvas_Rendering.cpp` | 386 | 894 | 30.16% |
| `Modules/Game/Canvas/src/MarqueeAutoScroll.cpp` | 73 | 162 | 31.06% |
| `Modules/Game/Canvas/src/PreviewCanvas.cpp` | 324 | 755 | 30.03% |
| `Modules/Game/Canvas/src/TimelineCanvas.cpp` | 551 | 1285 | 30.01% |
| `Modules/Game/Canvas/src/TimelineCanvas_Interaction.cpp` | 588 | 1368 | 30.06% |
| `Modules/Game/Canvas/src/TimelineCanvas_Interpolation.cpp` | 239 | 552 | 30.21% |
| `Modules/Game/Canvas/src/TimelineCanvas_Popups.cpp` | 1051 | 2452 | 30.00% |
| `Modules/Game/Canvas/src/TimelineCanvas_Templates.cpp` | 377 | 873 | 30.16% |
| `Modules/Game/Canvas/tests/CanvasTabTitleTest.cpp` | 78 | 181 | 30.12% |
| `Modules/Game/Canvas/tests/TimelineStopCreationTest.cpp` | 78 | 182 | 30.00% |
| `Modules/Game/Common/include/common/LogicCommandFwd.h` | 134 | 129 | 50.95% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 383 | 542 | 41.41% |
| `Modules/Game/Graphic/src/CursorManager.cpp` | 69 | 160 | 30.13% |
| `Modules/Game/Graphic/src/imguivk/VKRendererRender.cpp` | 305 | 709 | 30.08% |
| `Modules/Game/Graphic/src/imguivk/context/VKContext.cpp` | 229 | 528 | 30.25% |
| `Modules/Game/Graphic/src/imguivk/context/VKContextImguiImpl.cpp` | 2239 | 4262 | 34.44% |
| `Modules/Game/Logic/include/logic/BeatmapSession.h` | 218 | 153 | 58.76% |
| `Modules/Game/Logic/include/logic/EditorEngine.h` | 345 | 227 | 60.31% |
| `Modules/Game/Logic/include/logic/SyncClock.h` | 29 | 39 | 42.65% |
| `Modules/Game/Logic/include/logic/audio/PlaybackVisualClock.h` | 44 | 47 | 48.35% |
| `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1627 | 3795 | 30.01% |
| `Modules/Game/Logic/src/logic/ProjectCommandService.cpp` | 453 | 1050 | 30.14% |
| `Modules/Game/Logic/src/logic/ProjectController.cpp` | 674 | 1570 | 30.04% |
| `Modules/Game/Logic/src/logic/audio/PlaybackVisualClock.cpp` | 105 | 243 | 30.17% |
| `Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem.cpp` | 606 | 1411 | 30.04% |
| `Modules/Game/Logic/src/logic/session/ActionController_Editing.cpp` | 1400 | 3265 | 30.01% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 382 | 889 | 30.06% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_CommandState.h` | 8 | 14 | 36.36% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1294 | 3018 | 30.01% |
| `Modules/Game/Logic/src/logic/session/tool/MarqueeTool.cpp` | 90 | 209 | 30.10% |
| `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 215 | 500 | 30.07% |
| `Modules/Game/Logic/tests/PlaybackRestartTest.cpp` | 226 | 527 | 30.01% |
| `Modules/Game/Logic/tests/PlaybackVisualClockTest.cpp` | 250 | 583 | 30.01% |
| `Modules/Game/Logic/tests/ProjectAudioReferenceTest.cpp` | 712 | 1660 | 30.02% |
| `Modules/Game/UI/src/ui/IUIView.cpp` | 41 | 86 | 32.28% |
| `Modules/Game/UI/src/ui/UIManager.cpp` | 597 | 1369 | 30.37% |
| `Modules/Game/UI/src/ui/imgui/FloatingManagerUI.cpp` | 556 | 1289 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/MainDockSpaceUI.cpp` | 449 | 1046 | 30.03% |
| `Modules/Game/UI/src/ui/imgui/MainDockSpaceUI_Docking.cpp` | 111 | 220 | 33.53% |
| `Modules/Game/UI/src/ui/imgui/SideBarUI.cpp` | 160 | 373 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioPresetControls.cpp` | 61 | 142 | 30.05% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp` | 474 | 1105 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioTrackControllerUI.cpp` | 117 | 272 | 30.08% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioTrackControllerUI_Widgets.cpp` | 390 | 909 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp` | 221 | 510 | 30.23% |
| `Modules/Game/UI/src/ui/imgui/manager/AudioManagerView.cpp` | 728 | 1698 | 30.01% |
| `Modules/Game/UI/src/ui/imgui/manager/BeatMapManagerView.cpp` | 396 | 919 | 30.11% |
| `Modules/Game/UI/src/ui/imgui/manager/CollaborationLogWindow.cpp` | 153 | 355 | 30.12% |
| `Modules/Game/UI/src/ui/imgui/manager/CollaborationView.cpp` | 627 | 1462 | 30.01% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView.cpp` | 69 | 154 | 30.94% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView_Hierarchy.cpp` | 605 | 1410 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView_Interactions.cpp` | 105 | 243 | 30.17% |
| `Modules/Game/UI/src/ui/imgui/manager/LayoutPresetControls.cpp` | 67 | 156 | 30.04% |
| `Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp` | 594 | 1384 | 30.03% |
| `Modules/Game/UI/src/ui/imgui/manager/NewProjectWizard.cpp` | 294 | 683 | 30.09% |
| `Modules/Game/UI/src/ui/imgui/manager/ProjectAudioToolView.cpp` | 879 | 2051 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView.cpp` | 387 | 897 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp` | 129 | 299 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp` | 358 | 816 | 30.49% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_CollaborationTab.cpp` | 116 | 269 | 30.13% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_DebugTab.cpp` | 75 | 174 | 30.12% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_EditorTab.cpp` | 197 | 458 | 30.08% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_ProjectTab.cpp` | 167 | 380 | 30.53% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_ShortcutTab.cpp` | 130 | 302 | 30.09% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_SkinPackage.cpp` | 127 | 286 | 30.75% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_SoftwareTab.cpp` | 702 | 1638 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/manager/SettingsView_VisualTab.cpp` | 217 | 502 | 30.18% |
| `Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp` | 1838 | 4260 | 30.14% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/AddSelectedObjectAnnotationAction.cpp` | 123 | 281 | 30.45% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/BmsEditingToggleAction.cpp` | 28 | 58 | 32.56% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/EditSelectedObjectVolumeAction.cpp` | 94 | 205 | 31.44% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp` | 1096 | 2544 | 30.11% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MirrorAction.cpp` | 22 | 50 | 30.56% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MirrorPasteAction.cpp` | 25 | 56 | 30.86% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/PasteAction.cpp` | 21 | 49 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/PolylineEditingToggleAction.cpp` | 19 | 43 | 30.65% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/ProfessionalModeToggleAction.cpp` | 19 | 44 | 30.16% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/TogglePlaybackAction.cpp` | 57 | 133 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/file/PackBeatmapAction.cpp` | 764 | 1782 | 30.01% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/file/SaveBeatmapAction.cpp` | 177 | 387 | 31.38% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/file/SaveBeatmapAsAction.cpp` | 206 | 480 | 30.03% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/BpmMeasurementToolView.cpp` | 1560 | 3636 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/BpmMeasurementToolView_Markers.cpp` | 101 | 223 | 31.17% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/OpenBeatmapSpeedExportAction.cpp` | 303 | 701 | 30.18% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/OpenDataSourceReplaceWindowAction.cpp` | 106 | 246 | 30.11% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/ToggleOverlapCheckWindowAction.cpp` | 210 | 490 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/AnnotationDetailsToggleAction.cpp` | 15 | 34 | 30.61% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/FixedToolWindowToggleAction.cpp` | 15 | 33 | 31.25% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/ManagerLabelsToggleAction.cpp` | 14 | 32 | 30.43% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/PreviewWindowToggleAction.cpp` | 14 | 32 | 30.43% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/TimelineWindowToggleAction.cpp` | 14 | 32 | 30.43% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/view/ToolLabelsToggleAction.cpp` | 14 | 31 | 31.11% |
| `Modules/Game/UI/src/ui/imgui/menu/items/MainMenuRecentProjectsItem.cpp` | 81 | 173 | 31.89% |
| `Modules/Game/UI/src/ui/imgui/menu/items/MainMenuToolbarVisibilityItem.cpp` | 33 | 77 | 30.00% |
| `Modules/Game/UI/src/ui/imgui/menu/items/MainMenuTools.cpp` | 55 | 127 | 30.22% |
| `Modules/Game/UI/src/ui/imgui/menu/utils/MenuUtil.cpp` | 266 | 619 | 30.06% |
| `Modules/Game/UI/src/ui/imgui/windows/PgoUploadConsentWindow.cpp` | 26 | 60 | 30.23% |
| `Modules/Game/UI/src/ui/utils/UIWidgetUtils.cpp` | 639 | 1478 | 30.18% |
| `Modules/Game/UI/src/ui/walkthrough/WalkthroughPage.cpp` | 351 | 818 | 30.03% |
| `Modules/Game/UI/tests/BpmMeasurementHistoryTest.cpp` | 124 | 289 | 30.02% |
| `Modules/Game/UI/tests/ProjectAudioToolScrollTest.cpp` | 360 | 838 | 30.05% |
| `Modules/Game/UI/tests/SettingsSearchTest.cpp` | 186 | 434 | 30.00% |
| `Modules/Game/UI/tests/WorkspaceDockRestoreTest.cpp` | 398 | 928 | 30.02% |
| `Modules/Game/src/EditorApplicationService.cpp` | 64 | 113 | 36.16% |
| `Modules/Game/src/GameLoop.cpp` | 115 | 265 | 30.26% |
| `Modules/Main/src/main.cpp` | 203 | 473 | 30.03% |


## 第四轮：时间工具实现与目录监听器状态隔离

基于第三轮提交 `810667c2` 继续处理优先级表中的时间工具和目录监听器。

### 边界与行为

- `TimeFormatUtils.h` 保留轻量时间上下文、拍位结果和函数声明；快照与时间偏好仅做前向声明。
  BPM 提取、拍位计算和字符串格式化原样移入 `TimeFormatUtils.cpp`，不再向调用方传播完整快照、配置、fmt 和算法实现。
  快照为空、负时间、毫秒进位、跨 BPM 拍号、时长格式回退和默认参数不变。
  原有 `TimeFormatDetail` 调用入口继续可用，批注表仍复用其已缓存的 BPM 节点，不新增收集或排序。
- 四个真实读取快照或快照缓冲的实现文件补齐直接包含；不把重依赖加回工具公开头。
- `ProjectDirectoryWatcher::State` 只在实现文件定义，持有 future、停止源、运行位、互斥锁与 Win32 句柄。
  状态在构造时分配一次，监听器仍不可复制或移动；停止顺序仍为请求退出、锁外等待任务完成、最后释放平台句柄。
  `m_changePending` 仍直接位于监听器，消费路径的原子 exchange 和内存序不变，不新增锁、堆分配或共享所有权。
  工作任务只借用该门闩，析构先调用 stop，等待任务退出后才销毁对象与私有状态。
- 监听器测试新增未启动停止、重复停止、重新启动和由析构收尾的生命周期回归；只监听测试源目录，不写用户项目。

### 实际依赖范围

继续使用 Ninja 已记录依赖和当前 compile_commands；基线 514/514，新增时间工具实现后为 515/515 个编译单元。
这是依赖范围计数，不是编译耗时或运行时加速比例。

| 头文件 | 修改前 | 修改后 |
| --- | ---: | ---: |
| `RenderSnapshot.h` | 64 | 59 |
| `CanvasRenderTypes.h` | 74 | 69 |
| `RenderSnapshotBuffer.h` | 106 | 102 |
| `AppConfig.h` | 123 | 120 |
| `EditorSettings.h` | 228 | 226 |
| `TimeFormatUtils.h` | 15 | 16 |
| `ProjectDirectoryWatcher.h` | 19 | 19 |

时间工具公开 API 仍影响原有调用方，新增实现也包含自己的公开头，因此该头数量增加一；
算法或格式化实现的修改现在只重编译一个实现单元。监听器公开 API 的影响范围不变，
线程/平台状态布局和后台实现的修改不再触及这 19 个公开头消费者。

### 验证与限制

- Linux Debug / Clang 23 完整构建成功，CTest 172/172 通过。
- 两个公开头分别独立、重复包含的语法检查成功；依赖文件确认时间工具不再包含完整快照、配置或 fmt，监听器不再包含 future、mutex 或 stop_token。
- 既有批注拍位测试覆盖独立上下文、变速分段及无 BPM 回退；监听器生命周期用例通过。
- Windows 后台等待与句柄释放代码仅移动归属并按原次序访问，本轮未执行 Windows/macOS 平台构建。
- 不声称冷构建时间提升；源文件实现收敛和头文件传播减少是本轮可复核的结果。

### 本轮逐文件注释率

使用 `cloc --by-file`，按 comment / (comment + code) 统计。

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Logic/include/logic/ProjectDirectoryWatcher.h` | 26 | 26 | 50.00% |
| `Modules/Game/Logic/src/logic/ProjectDirectoryWatcher.cpp` | 131 | 289 | 31.19% |
| `Modules/Game/Logic/tests/ProjectDirectoryWatcherTest.cpp` | 23 | 50 | 31.51% |
| `Modules/Game/UI/CMakeLists.txt` | 182 | 423 | 30.08% |
| `Modules/Game/UI/include/ui/utils/TimeFormatUtils.h` | 63 | 52 | 54.78% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp` | 475 | 1107 | 30.03% |
| `Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp` | 221 | 512 | 30.15% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp` | 1096 | 2545 | 30.10% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/BpmMeasurementToolView.cpp` | 1560 | 3637 | 30.02% |
| `Modules/Game/UI/src/ui/utils/TimeFormatUtils.cpp` | 95 | 201 | 32.09% |

### 后续优先级

1. 按剩余消费者审视 `EditorSettings` 的领域偏好边界：优先抽出确有跨模块消费者的枚举/轻量值类型，不机械拆出无人独立使用的头。
2. 检查纯算法工具与数据容器依赖，保持热路径已缓存数据的访问方式；以依赖图判断收益。
3. `RenderSyncRegistry` 当前仅影响四个实现/测试单元，继续保持低优先级；除非与下一轮边界直接相关，不为 PImpl 本身扩大改动。


## 第五轮：音效、快捷键配置边界与快捷键实现隔离

先提交第四轮为 `beca0953`，随后按领域偏好和纯算法边界继续处理。

### 改动与约束

- 新增 `config/SfxConfig.h`：音效模式、音效配置、增益规范化与 JSON ADL 声明原样迁入。
  `AudioPresets.h` 仅包含该头并前向声明 `EditorSettings`；完整设置读取留在 `AudioPresets.cpp`。
  `SfxConfigTest` 改为仅包含独立音效头，实际调用原有序列化实现，检验声明完整性。
- 新增 `config/ShortcutConfig.h`：绑定、冲突判定与整套快捷键配置原样迁入。
  `EditorSettings` 继续按值包含这两类数据，不引入配置字段堆分配、虚调用或共享所有权。
  成员类型、顺序、默认值、命名空间以及 JSON 键和 ADL 类型身份保持不变；旧聚合头继续包含新类型头，兼容原调用入口。
- `ShortcutUtils` 的运行时按键查找、展示、修饰键查询、录制状态和工具字段映射移到独立源文件。
  公开头保留 constexpr 按键表和函数声明，完整设置仅前向声明，ImGui 内部结构只在源文件读取。
  未新增第三方 API 调用，已有输入查询与门禁次序不变；录制布尔状态仍是 UI 线程共享的一个静态对象。
  按键查找继续返回生命周期覆盖进程的固定表元素；工具映射仍借用配置成员，Layout 继续回退 Move。

### 实际收益与限制

Ninja 当前活动编译单元从 515/515 增至 516/516（新增快捷键实现）。

| 头文件 | 修改前 | 修改后 |
| --- | ---: | ---: |
| `EditorSettings.h` | 226 | 225 |
| `BeatLinePalette.h` | 232 | 231 |
| `NotePalette.h` | 232 | 231 |
| `TimingInterpolationPreferences.h` | 229 | 228 |
| `SyncConfig.h` | 228 | 227 |
| `AudioPresets.h` | 10 | 10 |
| `ShortcutUtils.h` | 15 | 17 |

三个原有对象移除了完整设置依赖：`SfxConfigTest`、`SettingsView_Search`、`MainMenuView`。
新实现和扩展后的快捷键回归各增加一个设置消费者，因此净减少一。
快捷键工具的公开头新增自身实现和回归测试两个消费者；修改函数签名或 constexpr 按键表仍会触及这些消费者，
运行时算法实现的修改则由原来的 15 个消费者收敛为一个实现单元。

新类型头仍经 `EditorSettings` 按值包含，不能声称修改 SfxConfig 或 ShortcutConfig 的数据布局只影响少数对象。
本轮主要建立真实独立消费者的类型边界，并缩小运行时实现变更的重编译范围，不把拆文件数量当作耗时收益。

### 验证

- Linux Debug / Clang 23 完整构建成功；CTest 172/172 通过。
- 四个公开头独立、重复包含检查通过：SfxConfig、ShortcutConfig、AudioPresets、ShortcutUtils。
  依赖文件确认均不传播完整 EditorSettings；ShortcutUtils 不再传播 imgui_internal。
- 既有音效配置、完整配置和音频预设序列化回归通过；新增快捷键回归检验五种工具的可写/只读引用身份、
  Layout 回退、未知键显示、禁用显示、静态按键表地址以及录制状态的 setter/getter/引用入口一致性。
- 格式化和差异空白检查通过；本轮未执行 Windows/macOS 构建，未测量冷构建耗时。

### 本轮逐文件注释率

使用 `cloc --by-file`，按 comment / (comment + code) 统计。

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Config/include/config/AudioPresets.h` | 29 | 43 | 40.28% |
| `Modules/Config/include/config/EditorSettings.h` | 252 | 375 | 40.19% |
| `Modules/Config/include/config/SfxConfig.h` | 24 | 33 | 42.11% |
| `Modules/Config/include/config/ShortcutConfig.h` | 38 | 59 | 39.18% |
| `Modules/Config/src/AudioPresets.cpp` | 92 | 213 | 30.16% |
| `Modules/Config/tests/SfxConfigTest.cpp` | 72 | 164 | 30.51% |
| `Modules/Game/UI/CMakeLists.txt` | 183 | 424 | 30.15% |
| `Modules/Game/UI/include/ui/imgui/ShortcutUtils.h` | 75 | 117 | 39.06% |
| `Modules/Game/UI/src/ui/imgui/ShortcutUtils.cpp` | 87 | 150 | 36.71% |
| `Modules/Game/UI/tests/PlaybackShortcutRoutingTest.cpp` | 40 | 91 | 30.53% |

### 下一轮优先级

1. `SessionUtils.h` 当前影响 26 个编译单元，却通过纯声明传播完整 `SessionContext`（57 个消费者）和 ECS 聚合头。
   检查能否仅前向声明上下文、组件和配置，使用轻量 ECS 声明头；真实字段访问在实现端显式包含，保持现有缓存算法不变。
2. `ProjectSettings.h` 74、`Project.h` 62：研究工作区 DTO 与项目配置覆盖的实际使用分布。
   先找仅需工作区字段或项目元数据的消费者，避免给所有按值项目配置引入不必要的 PImpl 和深拷贝。
3. 单独的调色文件入口目前只影响三个单元，`RenderSyncRegistry` 四个单元；维持低优先级，不机械扩大拆分。
