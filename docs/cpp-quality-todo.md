# C++ 工程整改清单

2026-11-23 · 主仓 `db00b802` · IonCachyEngine `4f54467c`。本次交付为审查清单。P1 优先处理正确性、数据完整性和交互停滞，P2 按子系统排期，P3 先量化收益或明确产品契约。已勾选项记录本次完成的实现和回归依据。

## 范围与证据

从两个仓库的已跟踪文件建立清单，覆盖 Modules、自维护 ICE 的 include/src/tests、自维护构建和工具脚本、打包代码及资产程序脚本。C++、头文件和 Objective-C++ 等 1,242 个文件进入模式扫描。第三方实现、预编译包、生成物和代理头不作业务判断。对命中的热点回读实现和上下游调用；这一步不能证明每条运行路径均已执行或每个函数均经过人工审阅。

平台条件模式匹配 67 个文件的 279 行，显式加锁相关模式匹配 69 个文件的 439 行（含注释与测试）。宏和锁各有合法用途，具体整改以下列调用链为依据。编译扇出使用本机已有 Ninja 依赖记录；614 个活动对象中有 601 个具备记录。性能结论区分每帧一定会做的工作、满足条件才会阻塞的工作及尚待采样的成本。

项目有房间目录、WebRTC 传输、协作资源 AES-256-GCM、资产同步及更新下载。入口分别见 `CollaborationDirectoryClient.cpp`、`WebRtcTransport.cpp`、`CollaborationResourceCipher.cpp`、`AssetSyncService.cpp` 和 `UpdateChecker.cpp`。本清单关注这些模块的内存、执行模型和工程边界；项目包含联网功能本身不能说明存在网络漏洞。

## 优先顺序

先处理 ICE-01 的声道状态、UI-01 的编辑截断和 PERF-01 的线程池进展条件。PERF-02 与 PERF-04 共用注册表锁，适合一起采样，再分别修正音频生命周期和 UI 读路径。之后做每拍显示、格式导入、逐块哈希等能单独验收的改动，最后按消费者逐批缩窄接口。

## 交互、线程和渲染

- [x] **PERF-01 P1 永久循环挤占逐帧任务的同一个有限线程池，存在饥饿卡死条件**

  - 容量：`Modules/Runtime/src/AppThreadPool.cpp:26` 最低工作者为 8；`:75-83` 按约 80% 核心数构造固定通用池，另有文件池。
  - 常驻任务：`Modules/Game/Logic/src/logic/EditorEngine.cpp:2232` 将整个逻辑 `loop()` 投递通用池；`Modules/Game/Logic/src/logic/ProjectDirectoryWatcher.cpp:195-198` 将生命周期监听循环投递同池。
  - 无视频也占用：`Modules/Game/Canvas/src/Basic2DCanvas.cpp:733` 每个画布构造时无条件创建 `BackgroundVideoPlayer`；`Modules/Game/Canvas/src/BackgroundVideoPlayer.cpp:19-27` 立即投递 `workerLoop`，`:133-147` 在循环内等待条件变量，等待期间工作者不会回到池中。对比画布也走此构造函数。
  - 网络：`Modules/Network/Collaboration/src/CollaborationResourceSync.cpp:753` 构造即启动；`:885-894` 同池投递工作者，`:923-930` 永久条件变量等待。
  - 必须完成的逐帧任务也使用此池：`Modules/Game/UI/src/ui/UIManager.cpp:1865-1881` 向同池投递 UI 准备任务后 `prepareLatch.wait()`；`Modules/Game/Graphic/src/imguivk/VKRendererRender.cpp:972-1022` 向同池投递离屏录制后 `offscreenLatch.wait()`。
  - 触发条件：常驻循环达到通用池工作者数，后续本帧任务只能排队，主线程等待 latch，常驻循环又不会退出；画布数量增长即可形成该条件，无需视频播放。尚未运行复现，不能声称某个固定数量在所有机器上必现。
  - 整改：将长期占用线程的服务与帧内短任务隔离；视频解码改为有待办才排有限任务、完成后归还工作者；或为实时帧任务建立有保留容量的独立执行器。只增大最低线程数不能消除无界画布数带来的同类风险。不要用 latch 超时后直接继续，因为当前 lambda 捕获栈上 snapshot/latch 引用。
  - 验收：固定小容量池，打开超过剩余工作者数的空白/Logo/对比画布，同时启用目录监听和协作同步；验证 UI、渲染、逻辑仍推进且关闭不挂。记录队列等待与执行耗时，确认空视频播放器不长期占槽；测试至少覆盖 UI 多个 prepare view 和多个 offscreen task 两条等待路径。

  - 回归已验证空视频播放器数量超过池容量，两个连续的帧任务批次仍完成。对应 Canvas、Logic、Network 测试套件已构建，MobileCanvasSelectionTest、ProjectDirectoryWatcherTest、BrushPaletteSessionStateTest、CollaborationResourceSyncTest 全部通过。

- [x] **PERF-02 P1 画布切换在 UI 调用栈同步加载完整音频，并持有跨 update 的全局会话锁**

  - UI 入口：`Modules/Game/Canvas/src/Basic2DCanvas.cpp:2970-2976` 获得焦点立即调用 `setActiveSessionIndex`；`:3090-3097` 非活动画布滚轮也直接调用。
  - 锁与重工作：`Modules/Game/Logic/src/logic/EditorEngine.cpp:3993-3996` 获取注册表锁，`:4066-4068` 重建音频描述符，`:4089-4090` 调用 `SessionUtils::activateAudioTimeline`。
  - 加载链：`Modules/Game/Logic/src/logic/session/SessionUtils.cpp:978-988` 按脏标记/指纹同步调用 `AudioManager::loadAudioTimeline`；`Modules/Audio/src/AudioManager_BGM.cpp:306-328` 提交解码后进入逐事件准备，`:368-369` 同步调用资源准备；`:238-239` 缓存未命中执行完整离线 DSP。主项目注释已明确其完整解码/准备契约，未深入 ICE 实现。
  - 卸载链：切到 Logo/失败目标会在 `EditorEngine.cpp:4105` 调用 `unloadAudioTimeline`，后者 `AudioManager_BGM.cpp:517-518` 调用 retirement 等待；`:188-198` 最多 500 ms、每 1 ms sleep。该函数自己的 warning 要求不得用于 UI 渲染/逻辑热路径，但当前焦点/滚轮调用栈可抵达。
  - 后果：一次鼠标焦点/滚轮可阻塞整个 UI 帧，并在锁内阻塞逻辑更新或其他状态查询。低频发生仍属于直接交互响应风险。
  - 整改：活动切换提交为带 generation 的请求；UI 本地焦点与目标身份即时更新，资源异步准备，逻辑安全边界只安装已完成结果；退休调度改为后续轮次非阻塞回收。旧加载结果必须按目标 generation 丢弃，不能抢占更晚切换。
  - 验收：使用多资源、开启 EQ/变速且冷缓存的实际谱面，连续点击/滚轮切换与切到 Logo；测 UI 帧与注册表锁等待，验证加载期间主界面持续刷新、最后一次切换生效、音频不串会话，停用音频回调也不会阻塞 UI 500 ms。

  - 已验证。焦点先交给逻辑线程，稳定相机身份与时间线指纹用于丢弃过期准备结果。后台执行解码和 DSP，焦点滚动走快照路由，停用旧调度无需等设备回调。仍由设备使用的旧调度留待后续调度提交或关闭时回收。
  - Linux RelWithDebInfo 主工程全量构建通过。BrushPaletteSessionStateTest 覆盖会话锁被占用时的非阻塞请求及快速请求合并。AudioManagerTimelineIntegrationTest 使用真实音频样本和 SDL 虚拟输出，覆盖后台 DSP、显式提交和非阻塞停用。PlaybackRestartTest 与 CanvasTouchNavigationTest 通过。

- [x] **PERF-03 P2 按拍号显示时间时，每帧多次重建并排序完整 BPM 数组**

  - 热路径入口：`Modules/Game/UI/src/ui/imgui/MainDockSpaceUI_StatusBar.cpp:289-291` 每帧格式化当前/总时间，`:313-315` 悬停再格式化一次；`Modules/Game/Canvas/src/PreviewCanvas.cpp:531`、`Modules/Game/Canvas/src/TimelineCanvas.cpp:650`、音频分析视图 `AudioSpectrumView.cpp:861` / `AudioWaveformView.cpp:592` 也调用快照重载。
  - 调用链：`Modules/Game/UI/src/ui/utils/TimeFormatUtils.cpp:286-292` pair 重复调用单值函数；Beat preference 在 `:228-229` 转发 `formatBeatTime`，`:198` 每次 `collectBpmPoints`；`:55-76` 扫描全部 `scrollSegments`、分配 vector、完整 sort 和 unique。
  - 后果：仅状态栏每帧至少两次 O(B log B)，悬停再加一次；多窗口叠加后与 BPM 数量和帧率相关。代码 warning 声称不是每帧批量路径，实际调用链违反该假设。
  - 整改：复用已有 `CanvasTimeFormatContext` 重载（`:207-211`）；按会话/BPM 修订构建归一化节点与前缀累计拍数，普通帧仅二分并格式化。不要按每帧 RenderSnapshot 地址失效缓存，否则播放发布会持续重建。
  - 验收：Beat preference 下用大量 BPM 节点谱面同时打开状态栏、时间线、预览、频谱/波形；统计 collect/sort 次数，时间推进/悬停时应为零，BPM 修改后每修订重建一次；与原结果对照负时间、同时间 BPM、分拍边界。

  - 已验证。逻辑侧随 BPM 缓存修订构建一次不可变索引，按原有规则累计每段整拍；各视口的快照槽仅在换修订时接管索引。状态栏、时间线、预览、频谱和波形的拍号显示只二分查询，滚动映射及快照地址改变均不触发重建。
  - Linux RelWithDebInfo 全量构建通过。BeatIndexTest、TimeFormatUtilsTest、CanvasCameraTest、ComparisonCanvasTest、CanvasTouchNavigationTest、PlaybackRestartTest、AnnotationTableBeatPositionTest、AnnotationExportTest 通过。8192 节点用例覆盖 128 轮复用、编辑后一次重建和旧快照保活；格式回归覆盖负时间、同刻、分拍及无滚动段的视口。未采集实际多窗口播放的帧耗时。
  - 本项注释／代码行数：BeatTimeIndex.h 8/14，RenderSnapshot.h 179/375，SessionContext.h 134/253，BeatmapSession_Rendering.cpp 661/1523，SessionUtils.cpp 456/1065，BeatIndexTest.cpp 61/138，TimeFormatUtils.h 59/50，TimeFormatUtils.cpp 89/196，TimeFormatUtilsTest.cpp 48/111。各文件均达到 30%。

- [x] **PERF-04 P2 Shift 持续按下时，快捷键扫描仍会等待完整会话 update 的递归锁**

  - `Modules/Game/UI/src/ui/imgui/menu/actions/edit/TogglePlaybackAction.cpp:194-199` 即使播放快捷键未命中，只要 Shift 按下且绑定无 Shift，就调用 `service->isDrawingBrush()`，之后才检测临时组合快捷键。
  - 适配链：`Modules/Game/src/EditorApplicationService.cpp:121-123` → `Modules/Game/Logic/src/logic/EditorEngine.cpp:3235-3238` 阻塞获取 SessionRegistry 锁。
  - 对侧长锁：`EditorEngine.cpp:4680-4681` 获取同锁，`:4730-4731` 在锁内执行整个 `BeatmapSession::update`，包括命令消费、ECS/快照和低频资源/保存分支。查询体只读布尔不代表等待时间短。
  - 同类入口：`EditorEngine.cpp:3196-3199`、`:3215-3218` 的框选/拖拽查询，快捷键命中且 ImGui 项目活动时 `TogglePlaybackAction.cpp:225-227` 连续调用三次。
  - 整改：逻辑更新发布一个带活动会话身份的轻量 interaction-state 快照/位掩码，UI 单次读取；或将当前 profile、按键命中判断提前以减少不必要查询，但仅提前检查不能替代最终去除热路径长锁等待。不得直接无锁读取可变 SessionContext。
  - 验收：相同实际谱面与播放区间，对比持续 Shift、画笔/框选/拖动与普通播放；记录 `onUpdateUI` 和该锁等待，确认查询不获取会话长锁，且允许/禁止播放快捷键的现有策略不变。

  - 已验证。逻辑侧在现有会话锁下计算手势位，只在变化时发布；快捷键只在命中后读取一次，持 Shift 且无主键的帧没有读取。切换在恢复最终工具后发布，关闭或新建会话同步更新状态。
  - Linux RelWithDebInfo 全量构建与 5 项 CTest 通过。锁占用回归证明读取在注册表锁释放前完成。真实键盘帧覆盖 Shift 兼容和活动控件限制，并验证每次命中只读取一次。本批验证锁等待和功能契约，实际谱面播放的帧时间未采样。
  - 修改文件注释率
    - `Modules/Game/Common/include/common/EditorGestureState.h` · 5 注释行 / 9 代码行 · 35.71%
    - `Modules/Game/Logic/include/logic/EditorEngine.h` · 357 注释行 / 238 代码行 · 60.00%
    - `Modules/Game/Logic/src/logic/EditorEngine.cpp` · 1696 注释行 / 3951 代码行 · 30.03%
    - `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` · 74 注释行 / 72 代码行 · 50.68%
    - `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` · 257 注释行 / 598 代码行 · 30.06%
    - `Modules/Game/UI/include/ui/IEditorApplicationService.h` · 31 注释行 / 48 代码行 · 39.24%
    - `Modules/Game/UI/src/ui/imgui/menu/actions/edit/TogglePlaybackAction.cpp` · 80 注释行 / 185 代码行 · 30.19%
    - `Modules/Game/UI/tests/WorkspaceDockRestoreTest.cpp` · 602 注释行 / 1403 代码行 · 30.02%
    - `Modules/Game/include/game/EditorApplicationService.h` · 12 注释行 / 24 代码行 · 33.33%
    - `Modules/Game/src/EditorApplicationService.cpp` · 57 注释行 / 106 代码行 · 34.97%

- [x] **PERF-05 P2 SessionContext 与 EditorEngine 的公开面跨越多职责，锁与生命周期契约暴露到 UI**

  - `Modules/Game/Logic/include/logic/session/context/SessionContext.h:95-226` 同时拥有 ECS 注册表、教学状态、草稿合并、播放/导航、项目与协作资源、配置、相机、同步缓冲、音频 transport；`:308-411` 又包括框选、画笔、橡皮擦、撤销与剪贴板。文件 414 行，当前 Modules 非测试源有 50 处直接包含此头。
  - `Modules/Game/Logic/include/logic/BeatmapSession.h:101-105` 公开完整 const 与 mutable 上下文引用，没有在线程/锁类型层面限制。
  - `Modules/Game/Logic/include/logic/EditorEngine.h:365` 对外暴露递归 mutex；`:86-88` 项目指针、`:282` 活动切换、`:542` 配置广播、`:567` 项目持久化，加上公开快照/资源/同步职责，形成全局单例门面（头 707 行、主实现 5894 行）。
  - 具体泄漏：`Modules/Game/Canvas/src/AnnotationTableData.cpp:45-55` UI 邻近层获取引擎 mutex 后直读 Context；批注版本未变也先抢锁。`Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp:2698-2718` 通过 try_lock 避免等待，但仍知道 SessionEntry 和谱面模型细节。
  - 这属于维护性和通用性风险，不把文件大本身判为运行错误。后果是新 UI 查询容易再次引入第 4 项长锁等待，独立测试必须搭建全局引擎，教学等场景不断扩张核心上下文。
  - 整改：以现有发布快照/命令队列为边界，逐职责提取只读 presentation DTO 与窄服务；优先 interaction-state、批注读模型、transport lifecycle，再收紧 mutable Context 与 mutex 访问。不为缩短头文件而把轻量值随意堆分配，也不一次性重写所有 Controller。
  - 验收：每批提取后原行为测试通过；UI/Canvas 相关文件不再包含 SessionContext 或获取 session mutex；使用 Ninja 依赖图量化头文件翻译单元扇出变化；分别测试活动会话关闭、切换与快照保活。

  - 已完成批注表读模型。逻辑侧在会话更新边界按请求投影批注行与导出字段，同时发布拍位与播放状态。Canvas 读取完整的不可变快照，不获取会话更新锁。批注与 Timing 分别按修订复用，刷新检测到活动标签变更时清除另一会话的旧行。
  - Linux Debug 32 线程全量构建及全部 218 项 CTest 通过。回归覆盖会话更新锁占用期间刷新、相同批注修订的谱面切换、分拍修改仅更新时间格式、关闭后回到剩余谱面、会话销毁后的旧快照保活，以及 SDL 虚拟输出播放期间拒绝提交。
  - 本批逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Canvas/CMakeLists.txt` | 149 | 347 | 30.04% |
    | `Modules/Game/Canvas/include/canvas/AnnotationTableData.h` | 25 | 43 | 36.76% |
    | `Modules/Game/Canvas/src/AnnotationTableData.cpp` | 27 | 61 | 30.68% |
    | `Modules/Game/Canvas/src/AnnotationTableWindow.cpp` | 263 | 597 | 30.58% |
    | `Modules/Game/Canvas/tests/MobileAnnotationTableTest.cpp` | 211 | 492 | 30.01% |
    | `Modules/Game/Logic/include/logic/AnnotationTableSnapshot.h` | 32 | 46 | 41.03% |
    | `Modules/Game/Logic/include/logic/BeatmapSession.h` | 229 | 167 | 57.83% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 358 | 240 | 59.87% |
    | `Modules/Game/Logic/include/logic/SessionTypes.h` | 31 | 42 | 42.47% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1701 | 3967 | 30.01% |
    | `Modules/Game/Logic/src/logic/SessionRegistry.cpp` | 125 | 279 | 30.94% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 416 | 965 | 30.12% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp` | 725 | 1685 | 30.08% |

  - 已完成工具栏轨道布局批次。音效弹层一次读取玩家、草稿和 BGM 三组标量，通过短临界区取得同一代布局。键数滚轮、滑条和预设通过会话队列提交单字段变更，逻辑消费时才按当前模型计算键数，继续使用现有迁移、撤销和尾随保存流程。
  - Linux Debug 32 线程全量构建，全部 218 项 CTest 通过。回归覆盖占用会话锁时音效布局读取与键数命令入队、连续两格滚轮保留前一条标题修改和 BGM 相对轨、切换及关闭后的三组轨道同步。Ninja 活动对象记录中，`ToolbarView.cpp` 移除了 `SessionContext.h` 和 `BeatmapSession.h` 的依赖，`SessionContext.h` 的扇出由 71 降至 70。
  - 工具栏批次逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Common/include/common/EditorTrackLayout.h` | 7 | 12 | 36.84% |
    | `Modules/Game/Common/include/common/LogicCommands.h` | 434 | 602 | 41.89% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 364 | 246 | 59.67% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1716 | 4004 | 30.00% |
    | `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` | 77 | 75 | 50.66% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 417 | 970 | 30.06% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1377 | 3212 | 30.01% |
    | `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 297 | 683 | 30.31% |
    | `Modules/Game/UI/include/ui/imgui/manager/ToolbarView.h` | 202 | 188 | 51.79% |
    | `Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp` | 2022 | 4675 | 30.19% |

  - 已完成移动播放控件与活动状态发布批次。播放位、键数和活动目标代次一起发布，移动控件只读取标量。关闭或切换谱面时，旧键数及倍速弹层失效；播放、键数及倍速命令投递前复核代次，避免索引被复用时投到另一张谱面。逻辑循环在已有会话锁内确认快照条目与活动身份仍有效。
  - Linux Debug 32 线程构建及 218 项 CTest 通过。回归覆盖长锁期间读取、载入时撤回状态、同索引替换后拒绝旧命令并接受当前代次。Android ARM64 NDK 编译了移动播放控件、移动工作区外壳及引擎实现。Android 的 Ninja 历史依赖和同参数重新提取的编译依赖显示，移动播放控件去除了 `SessionContext.h` 和 `BeatmapSession.h`。Linux 活动对象中 `SessionContext.h`、`BeatmapSession.h` 和 `EditorEngine.h` 的扇出分别保持在 70、51 和 97。
  - 移动播放批次逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Event/include/event/logic/LogicCommandEvent.h` | 10 | 22 | 31.25% |
    | `Modules/Game/Common/include/common/EditorPlaybackState.h` | 5 | 11 | 31.25% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 369 | 251 | 59.52% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1721 | 4008 | 30.04% |
    | `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` | 78 | 77 | 50.32% |
    | `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 321 | 749 | 30.00% |
    | `Modules/Game/UI/include/ui/workspace/MobilePlaybackControls.h` | 21 | 29 | 42.00% |
    | `Modules/Game/UI/src/ui/workspace/MobilePlaybackControls.cpp` | 159 | 366 | 30.29% |
    | `Modules/Game/UI/src/ui/workspace/MobileWorkspacePresentation.cpp` | 372 | 866 | 30.05% |

  - 已完成桌面播放命令路由批次。播放、定位、倍速与音高使用独立发布的活动会话目标，不获取会话更新锁。控件只有实际提交命令时才短暂保活目标，由原队列和播放控制器消费。代次与目标一起发布，关图及同索引更换后拒绝旧代次。移动端带代次的键数编辑继续经过项目写入门禁。
  - Linux Debug 32 线程构建、全部 218 项 CTest 及 Android ARM64 NDK 编辑引擎编译通过。回归检查会话锁占用时的四类命令投递、定位拖动阶段、试听倍速和音高生效、索引复用及最后一个会话关闭。Ninja 活动对象中 `SessionContext.h`、`BeatmapSession.h` 和 `EditorEngine.h` 的扇出仍为 70、51 和 97。
  - 桌面播放命令批次逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 373 | 253 | 59.58% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1731 | 4026 | 30.07% |
    | `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` | 81 | 79 | 50.62% |
    | `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 336 | 783 | 30.03% |

  - 已完成画布切换音频准备批次。编辑引擎消费焦点意图，`CanvasAudioPreparation` 管理单项后台准备、提交和退出回收。活动目标改变或资源指纹变化使旧结果失效；播放定位和续播意图在提交时读取。关闭、重置或新建画布会撤销旧目标的等待和续播标记。同步工作区恢复继续使用现有激活入口。时间线弹层和设置页等仍有直接访问会话锁及上下文的路径。
  - Linux Debug 32 线程构建、全部 218 项 CTest 及 Android ARM64 NDK 编辑引擎、准备组件编译通过。相关回归覆盖同指纹换画布、当前画布资源变化、等待期间暂停及不可用后端的收尾。Ninja 活动对象中 `SessionContext.h` 扇出由 70 增至 71，新增依赖来自准备组件的实现；`BeatmapSession.h` 与 `EditorEngine.h` 仍为 51 和 97。
  - 画布切换音频准备批次逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Logic/CMakeLists.txt` | 146 | 339 | 30.10% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 373 | 249 | 59.97% |
    | `Modules/Game/Logic/include/logic/audio/CanvasAudioPreparation.h` | 29 | 41 | 41.43% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1709 | 3958 | 30.16% |
    | `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` | 76 | 73 | 51.01% |
    | `Modules/Game/Logic/src/logic/audio/CanvasAudioPreparation.cpp` | 49 | 110 | 30.82% |
    | `Modules/Game/Logic/tests/PlaybackRestartTest.cpp` | 255 | 593 | 30.07% |

  - 已完成时间点表格读模型批次。打开表格时，通过同代活动会话快照请求完整 Timing 行。逻辑线程仅在修订变化或谱面重新载入时投影并排序，表格帧借用同一代行与实例令牌，避免读取会话长锁或每帧复制全表。复制、编辑等低频入口仍保留原有会话访问。
  - Linux RelWithDebInfo 全量构建和 13 项 CTest 通过。回归覆盖持有会话锁时读取、快照复用、Timing 变更、同会话重载、换图和关闭后的保活，以及真实鼠标和触屏表格在异步发布后的交互。
  - 时间点表格批次逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Canvas/src/TimelineCanvas_Popups.cpp` | 1134 | 2646 | 30.00% |
    | `Modules/Game/Canvas/tests/TimingTableMouseSelectionTest.cpp` | 229 | 508 | 31.07% |
    | `Modules/Game/Logic/CMakeLists.txt` | 147 | 340 | 30.18% |
    | `Modules/Game/Logic/include/logic/BeatmapSession.h` | 239 | 173 | 58.01% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 374 | 252 | 59.74% |
    | `Modules/Game/Logic/include/logic/TimingTableSnapshot.h` | 7 | 15 | 31.82% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1713 | 3971 | 30.14% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 419 | 976 | 30.04% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1379 | 3214 | 30.02% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession_TimingTable.cpp` | 38 | 84 | 31.15% |
    | `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 369 | 861 | 30.00% |

  - 已完成普通关闭确认批次。窗口收到关闭请求时，逻辑侧在同一临界区读取全部会话的脏状态和活动谱面名。弹窗持有名称副本，绘制与按钮分发均不持有会话锁。Linux Clang 22 RelWithDebInfo 全量构建及 11 项相关 CTest 通过，覆盖后台脏谱面、活动切换、关闭后的值保留和保存基线清理。Ninja 活动编译对象中，`SessionContext.h` 扇出由 72 降至 71，移除 `MainDockSpaceUI.cpp`。
  - 注释／代码行数分别为 `EditorEngine.h` 375/252、`EditorEngine.cpp` 1712/3979、`MainDockSpaceUI.h` 75/112、`MainDockSpaceUI.cpp` 429/943、`BrushPaletteSessionStateTest.cpp` 387/902，均达到 30%。

  - 已完成导出对话框批次。四处会话读取改为 Logic 的按值命名及兼容性读模型。BMS、osu!、IMD 和 Malody 只检查对应目标所需的字段；文件名清理和警告格式化在会话锁外运行。Ninja 中 `MenuUtil.cpp` 已解除 `SessionContext.h` 依赖，新增 Logic 读模型使用该头，总活动对象扇出维持 71。
  - Linux Clang 22 RelWithDebInfo 全量构建、11 项导出相关回归及全部 221 项 CTest 通过。新增回归覆盖 Unicode 命名、BMS 采样音量、osu! 负 Scroll、IMD 元数据、Malody mode 与会话关闭后结果保活。
  - 注释／代码行数分别为 `BeatmapExportPresentation.h` 27/35、`BeatmapExportPresentation.cpp` 77/172、`MenuUtil.cpp` 216/473、`BrushPaletteSessionStateTest.cpp` 408/950、`Modules/Game/Logic/CMakeLists.txt` 148/341，均达到 30%。

  - 谱面设置绘制使用不可变快照；活动代次和辅助轨计数从独立发布槽读取。逻辑线程只复制当前工程与谱面字段，文件池负责路径探测，并在封面或背景下拉打开时合并枚举图片与视频。设置写命令绑定载图实例，消费时复核只读状态并按字段合并现有修改。
  - Linux Clang 22 RelWithDebInfo 全量构建与 221 项 CTest 通过。回归覆盖会话锁占用期间的读取与提交、按需目录枚举、工程相对路径、临时工程只读、外部字段更新保留、同路径重载拒绝旧指令及关闭会话后快照保活。
  - Ninja 实际依赖显示 `SettingsView_BeatmapTab.cpp` 解除 `SessionContext.h`、`BeatmapSession.h` 与 `EditorEngine.h` 依赖，重类型读取移至 Logic 实现。三者在当前活动对象中的扇出分别为 71、51 和 98；新快照头由 4 个活动对象依赖。
  - 本批逐文件注释率

    | 文件 | 注释行 | 代码行 | 比例 |
    | --- | ---: | ---: | ---: |
    | `Modules/Game/Common/include/common/LogicCommands.h` | 439 | 612 | 41.77% |
    | `Modules/Game/Logic/CMakeLists.txt` | 149 | 343 | 30.28% |
    | `Modules/Game/Logic/include/logic/BeatmapSettingsSnapshot.h` | 36 | 56 | 39.13% |
    | `Modules/Game/Logic/include/logic/EditorEngine.h` | 380 | 256 | 59.75% |
    | `Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp` | 93 | 213 | 30.39% |
    | `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1718 | 3993 | 30.08% |
    | `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1402 | 3271 | 30.00% |
    | `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 472 | 1092 | 30.18% |
    | `Modules/Game/UI/include/ui/imgui/manager/SettingsView.h` | 261 | 216 | 54.72% |
    | `Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp` | 335 | 767 | 30.40% |

- [x] **PERF-06 P3 离屏几何容量扩张在命令录制阶段等待全 GPU 空闲**

  - 离屏顶点和索引缓冲按帧槽保存容量。渲染器成功等待该槽的提交 fence 后，录制才可复用或扩张该槽；保留其他在途缓冲及固定大小 uniform。扩容记录槽、需要／原有／新增容量和分配耗时。
  - Linux Clang 22 RelWithDebInfo 全量构建和 15 项图形、画布、播放 CTest 通过。在同一 Intel UHD ADL-S、X11 和用户配置下播放《骤雨の狭間》原谱前 15 秒。排除启动和加载，仅比较完整播放统计窗口，旧版／新版扩容均为 0 次。离屏平均 0.140／0.138 ms，fence 2.969／3.097 ms，acquire 0.020／0.021 ms，present 0.022／0.022 ms。进程 GPU render engine 占用 44.8%／46.4%；逻辑线程约占满一核，渲染线程用一核的 20.1%／22.3%，其 futex 等待为 2／3 个采样点。
  - 在同一谱面副本加密集物件，确认两个离屏目标突破 81920 元素。旧版各目标一次扩大全部槽，新版各槽各扩张一次，单槽分配 0.119–0.249 ms；所测扩容窗口的离屏最大耗时 27.47／2.56 ms。该峰值比较仅覆盖录制阶段。Khronos 验证层复现了旧版同样两种已有 descriptor 诊断，未报告新的缓冲生命周期诊断。
  - 注释／代码行数分别为 `VKOffScreenRenderer.h` 117/232、`VKOffScreenRenderer.cpp` 160/377、`VKOffScreenRendererRess.cpp` 277/626、`VKRendererRender.cpp` 333/771，均达到 30%。

## 平台适配

- [x] **PLAT-01（P2）启动区域设置使用 Windows 名称且忽略失败**

  - 位置：`Modules/Game/include/game/GlobDefs.h:79-80`。
  - 行为：全局 `RTTILogger` 构造无条件调用 `std::setlocale(LC_ALL, ".UTF-8")`，返回值被丢弃，注释却声称 C/C++ 路径与格式化已统一 UTF-8。
  - 证据：当前 Linux 隔离 Python/ctypes 进程调用相同 C API，返回 `None`，调用前后 locale 完全相同。因此 Linux 实际继承原 locale，注释保证不成立。这里尚不能推导所有中文路径都失败，因为已有 UTF-8/path 包装可能自行保证语义。
  - 整改：把 locale 初始化收入启动平台实现，定义失败回退和日志，明确只初始化真正需要的 category；不要依赖一个平台名称覆盖所有系统。
  - 验收：Windows、Linux（含 `LC_ALL=C`）、macOS 上检查成功/失败返回与最终 category，中文路径和数值解析不依赖宿主 locale；失败有明确诊断。
  - 已按平台选择 UTF-8 字符设置，只修改 LC_CTYPE；不可用时保留原值并在日志就绪后告警。Linux RelWithDebInfo 全量构建和 8 项回归通过，另验证 `LC_ALL=C`、中文环境及隔离缺少 locale 数据。macOS ARM64（最低部署 11.0）原生和 debsrv MSVC 交叉构建后在 Wine 运行的启动探针通过，注入 CRT 返回空值的回退均通过。各平台探针均核对中文文件往返、数值解析和非字符类别。

- [x] **PLAT-02（P2）文本协议浮点语法随平台变化**

  - 位置：`Modules/Game/Logic/src/logic/EditorClipboardProtocol.cpp:240-249`；相关实现 `Modules/MMM/include/mmm/SafeParse.h:40-71`。
  - 行为：剪贴板协议 `parseDoubleField` 只检查完整消费与有限值，Apple 分支由 `strtod_l` 解析，其余平台由默认格式 `from_chars` 解析。输入 `0x1p2` 的十六进制语法在 Apple 可被完整消费，默认 `from_chars` 不会按该语法完整消费；于是同一外部协议文本在平台间的合法性不同。
  - 同类：`Modules/Game/Logic/src/logic/BmsPackageExportService.cpp:198-202` 对 MYO2 cue shift 使用同样的完整消费检查；`Modules/Game/UI/src/ui/imgui/SideBarUI.cpp:43-46` 允许前缀解析，对相同文本还可能得到不同数值/回退结果。后两项须先确认各字段的既有输入契约，不能机械一刀切。
  - 可复用先例：`Modules/MMM/src/beatmap/BmsFormat.hpp:69-80` 已用十进制字符白名单隔离平台扩展语法。
  - 整改：给协议明确十进制语法并在该边界验证，或定义统一 helper 语法；不修改兼容性解析器的所有调用方。
  - 验收：同一组正负号、科学计数、十六进制、NaN/Inf、尾随垃圾和溢出输入，在 Apple 低部署目标与 Linux/Windows 上得到相同协议结果；保留合法历史剪贴板往返。

  - 已实现剪贴板字段的十进制字符和完整消费校验，保留合法指数、历史版本以及可往返的非零次正规数。Linux RelWithDebInfo 全量构建及 5 项相关 CTest 通过；Linux 上的两种数值后端对照探针覆盖 28 个输入，结果一致。
  - 在 `debsrv` 固定 Runner 的原工作区，以 clang-cl 23、MSVC 14.52 和 Windows SDK 10.0.26100.0 完成 x64 RelWithDebInfo 交叉构建。两份 Windows 测试程序传回后经 SHA-256 校验，并在 Wine 10.0 下运行 5 项回归，全部通过。Runner 已恢复原 `ci` 提交及测试配置。
  - 在现有原生 Apple Silicon Runner 上针对 macOS 11.0 部署目标全量构建，5 项回归在宿主 macOS 26.5.1 通过。Windows 与 macOS 验证使用同一源提交 `4a0298f8`。注释／代码为 EditorClipboardProtocol.cpp 423/978、EditorClipboardProtocolTest.cpp 220/509，逐文件均达到 30%。

- [x] **PLAT-03（P2）三个 UI 时间格式化分支遗漏失败判断**

  - 位置：`Modules/Game/UI/src/ui/imgui/manager/AudioManagerView.cpp:301-329`、`BeatMapManagerView.cpp:202-227`、`FileManagerView_Hierarchy.cpp:273-298`。
  - 行为：三个匿名 `formatModifiedTime` 重复文件时钟近似转换和 `_WIN32` 分支。`localtime_s`/`localtime_r` 返回值没有检查，失败后仍把零初始化 `tm` 交给 `strftime`，它可能成功产生一个看似有效的日期，绕过“未知”回退。
  - 整改：提取 UI 时间显示公共 helper，集中平台签名差异，时间转换失败直接返回未知。是否换成 `file_clock::to_sys` 要按各 Runner 标准库支持度决定，不能仅凭本机 C++20 标记推断。
  - 验收：正常/负纪元/不可表示时间、时区和夏令时边界统一；三处 UI 仍使用原始文件时刻排序；失败显示未知且不抛异常。

  - 已验证。三个管理视图共用 `formatFileModifiedTime`，先以扩宽的秒数换算文件时钟与系统时钟，再检查 `time_t` 范围、本地时间转换和格式化结果。失败显示原有的本地化未知占位；排序仍使用原始时间和值的有效位。
  - Linux RelWithDebInfo 全量构建及 8 项相关 CTest 通过。TimeFormatUtilsTest 覆盖普通时刻、纪元前后、不可表示的日历时间、时区改变与夏令时跳变。Windows 返回值已对照 Microsoft CRT 文档，Windows 和 macOS 原生运行尚未验证。
  - 本项注释／代码行数：FileModifiedTime.h 7/13，FileModifiedTime.cpp 21/46，AudioManagerView.cpp 769/1791，BeatMapManagerView.cpp 398/927，FileManagerView_Hierarchy.cpp 631/1472，TimeFormatUtilsTest.cpp 70/159，UI/CMakeLists.txt 243/563。逐文件均达到 30%。

- [x] **PLAT-04（P2）Windows 命令行解析失败后可能读取未初始化数量**

  - 位置：`Modules/Game/src/GameLoop.cpp:274-292`。
  - 行为：`int argcW;` 没初始化；取得 `CommandLineToArgvW` 结果后直接判断 `argcW > 1` 并访问 `argvW[1]`。若 API 分配失败返回空，代码没有任何失败分支。
  - 整改：数量初始化为零，先检查返回指针，再处理第二参数；把“平台命令行到路径”收敛到启动适配器，保留事件发布在通用 GameLoop。
  - 验收：正常中文/空格/长路径参数保持一致；API 失败能够继续启动且不发布打开事件；分配结果只释放一次。

  - 已将参数解析移入 Windows 与 POSIX 启动适配器。Windows 先判空，参数块由局部智能指针统一回收；通用启动流程仍只为存在且可查询的路径发布打开事件。
  - Linux RelWithDebInfo 全量构建及 6 项启动路径和工作区 CTest 通过，另用实际 POSIX 适配器验证无参、含中文和空格、长路径。在 `debsrv` Runner 原工作区完成 Windows x64 交叉构建，Windows 探针在 Wine 下通过无参、空参、含中文和空格、长路径，并通过平台调用替身验证空指针失败和成功后仅释放一次。
  - 注释／代码行数：`GameLoop.cpp` 113/245、`StartupCommandLinePath.h` 5/6、`StartupCommandLinePathPosix.cpp` 5/9、`StartupCommandLinePathWin32.cpp` 7/15、`Modules/Game/CMakeLists.txt` 14/30，逐文件均达到 30%。

- [x] **PLAT-05（P3）启动头重复平台可执行路径实现，边界与长路径行为已分叉**

  - 位置：`Modules/Game/include/game/GlobDefs.h:84-116` 对照 `Modules/Config/src/AppPaths.cpp:63-111`。
  - 行为：全局头直接引入 Windows/unistd/dyld 并查询进程路径，Windows 固定 `MAX_PATH`；AppPaths 已经独立实现动态扩容，macOS 还解析 canonical 路径。启动更改 cwd 和配置路径查询因而有两套不同成功边界。
  - 整改：复用低层可执行路径服务，把平台 include 与操作移出公开业务头；不要在头中继续复制第三套实现。需注意日志初始化前不能反向依赖日志与业务单例。
  - 验收：Windows 超 MAX_PATH 的可执行路径、macOS 符号链接启动、Linux procfs 失败均有明确一致结果；日志初始化顺序与资源相对路径行为保持兼容。
  - 已复用 Config 查询，并修复原生启动探针在主线复现的日志退出析构顺序问题。Linux 全量构建、9 项 CTest、符号链接与缺少 procfs 启动探针通过。debsrv MSVC 交叉编译后，Wine 中 288 字符中文路径与正常退出均通过。AppleClang ARM64 最低部署目标 11.0 全量构建、7 项 CTest、符号链接启动和正常退出均通过。

- [x] **PLAT-06（P3）窗口平台状态策略仍直接驻留通用渲染文件**

  - 位置：`Modules/Game/Graphic/src/imguivk/VKRendererRender.cpp:526-612`、`:623-665`；`Modules/Game/Graphic/src/glfw/NativeWindow.cpp:760-805`、`:1700-1725`、`:1775-1815`。
  - 行为：Vulkan renderer 同时承担 HWND property、ShowWindow/SetWindowPos/SetForegroundWindow；`MMMRestoreMaximizedAfterMinimize` 常量分别出现在 renderer、NativeWindow（:274）和 Win32WindowAdapter（:31）。平台适配器已经存在，但最大化恢复协议仍跨三处维护。
  - 整改：将窗口组提升、最大化恢复意图、startup frame shape 操作逐项收入现有平台适配器；renderer 只调用抽象窗口动作。保留现有低频激活边沿机制，不把系统调用移入逐帧无条件路径。
  - 验收：Windows 任务栏/系统菜单最小化与恢复、浮动 ImGui 工具窗不抢焦点；macOS 模拟最大化；X11/Wayland 的既有行为分别验证。此项是职责治理建议，未发现仅靠宏本身即可证明的运行故障。

  - 窗口组激活、浮动视口外观、启动窗裁剪和最大化恢复状态已迁到 GLFW 窗口层及 Win32 适配器。恢复意图只维护一份 HWND 属性。最小化的工具窗以无激活方式恢复，组内最后一次前台请求交给主窗。通用 Vulkan 录制代码只收集视口并发起窗口动作。
  - Linux Clang 22 RelWithDebInfo 全量构建和 11 项相关 CTest 通过。X11 与 Weston Wayland 窗口探针通过；X11 的由窗口管理器决定的焦点表现与原操作序列一致。原生 macOS arm64、部署目标 11.0 的探针验证了启动窗、模拟最大化和还原。
  - Windows 在 debsrv 现有 MSVC-clang Runner 交叉编译，Wine 探针通过任务栏所有者、`WM_SYSCOMMAND` 恢复最大化、最小化浮窗恢复及主窗焦点验证。两台 Runner 已恢复到验证前提交。
  - 以 `a7ea17b3` 在 debsrv 原有 Runner 容器重新交叉编译窗口探针，链接 #218 的实际 `NativeWindow` 和 Win32 适配器。Windows 10 22H2（19045）原生 Explorer 回归通过。实际点击任务栏完成最小化及恢复，再从 Explorer 窗口缩略图的系统菜单最小化并从任务栏恢复。两轮均恢复为最大化，浮动视口正常恢复，主窗保持前台和键盘焦点。探针针对原生窗口行为，不初始化 Vulkan 渲染。
  - 注释／代码行数分别为 `NativeWindow.h` 123/102、`NativeWindow.cpp` 578/1110、`Win32WindowAdapter.h` 86/65、`Win32WindowAdapter.cpp` 301/634、`VKRendererRender.cpp` 292/659，均达到 30%。

- [x] **PLAT-07（P3）致命提示接口的平台实现不完整**

  - 位置：`Modules/Game/Common/include/common/MessageBox.h:21-40`；唯一已定位调用 `Modules/Game/Graphic/src/imguivk/context/VKContextDebugFeats.cpp:82`。
  - 行为：Windows 使用 `MessageBoxA`；所有非 Windows 系统都尝试 Linux 桌面命令 zenity/kdialog/notify-send，忽略全部失败。当前调用为固定可信英文文本，因此不能据此宣称存在当前可触发的命令注入，也不需要扩大成安全审计；macOS 未提供独立原生提示实现。
  - 整改：移动到平台源文件；Windows 采用明确 UTF-8 到宽字符转换，macOS 提供本机提示或可靠日志/退出降级；外部命令如需保留用参数数组避免 shell 文本协议。
  - 验收：未能初始化图形界面时三桌面平台均可见错误或有可靠诊断；引号、换行和中文正确；保持只用于低频致命路径。

  - 致命提示移入按平台编译的实现，统一先记录完整诊断。Windows 用 UTF-16 消息框，Linux 通过进程参数数组尝试桌面通知，macOS 和其他平台走日志及现有启动失败退出路径。
  - Linux RelWithDebInfo 全量构建及 5 项相关 CTest 通过。独立探针验证中文、引号和换行在 Linux 通知参数中保持原样，全部工具不可用时仍保留日志；通用日志后备也通过同机独立探针。debsrv 使用既有 Runner 和 clang-cl 交叉构建 Win32 适配器及测试探针，Windows x64 探针在 Wine 下通过 UTF-16 参数和非法 UTF-8 回退，使用替身拦截弹窗。macOS 后备路径未在原生宿主运行。
  - 注释／代码行数：`MessageBox.h` 4/6、`MessageBoxFallback.cpp` 5/9、`MessageBoxLinux.cpp` 21/48、`MessageBoxWin32.cpp` 24/56、`VKContextDebugFeats.cpp` 40/86、`Modules/Game/Common/CMakeLists.txt` 26/58，均达到 30%。

## 谱面、音频、资产和协作文档

- [x] **FMT-01（P2）Malody 时间查询每次从头扫描全部 BPM**

  - 位置：`Modules/MMM/src/beatmap/LoadMalodyMap.hpp:816-840`。
  - 调用链：`BeatMap::loadFromFile`（`BeatMap.cpp:154-156`）→ `loadMalodyMap` → 每个 note 的 `getAbsTime`（:927），折线首段/子段/尾部（:1048、:1097、:1151）；timing 转换也调用查询（:850、:859）。
  - 当前行为：BPM 已排序并预计算 timestamp（:789-814），但 `getAbsTime`/`getBpmAtBeat` 每次从 vector 开头线性找到最后一个不晚于目标 beat 的事件。因此大量 note/折线段配合大量 BPM 时，导入最坏为 O((物件与子段数量 + 其他时间事件数) × BPM 数)，不是只遍历一次谱面。
  - 整改：先保留既有稳定同拍排序及 `1e-9` 语义，将时间查询改为有序锚点二分；或对已按 beat 排序的独立序列使用单调游标。不能全局复用游标却假设来自不同字段的查询一定单调。
  - 验收：同拍多 BPM、负拍、delay、包裹首音频相位及折线逐段时间完全一致；用不同 BPM/物件规模证明查询成本不再呈两者乘积增长，并检查实际导入耗时。

  - 已改为在稳定排序后的 BPM 锚点上二分，保留同拍顺序、容差和首锚点前外推。Linux RelWithDebInfo 全量构建及 7 项 Malody CTest 通过。四组合成谱面的音符与 timing 数量、音符时间戳及 timing 时间戳和 BPM 的聚合哈希均相同。
  - 使用同机的实际加载入口，每组预热 1 次并取 5 次中位数。512 个 BPM 节点下，8,192 / 65,536 个音符从 10.66 / 80.03 ms 降至 10.08 / 75.03 ms。8,192 个 BPM 节点下，两种音符规模从 40.56 / 219.26 ms 降至 25.98 / 90.22 ms。
  - 修改文件 `LoadMalodyMap.hpp` 注释 377 行、代码 838 行，31.03%。

- [x] **FMT-02（P2）Malody 导入保留整文本与 JSON DOM，没有文件/对象入口预算**

  - 位置：`Modules/MMM/src/beatmap/LoadMalodyMap.hpp:359-370`。
  - 调用链：`BeatMap::loadFromFile(.mc)` → `loadMalodyMap` → `istreambuf_iterator` 完整复制到 `fileContent` → 完整 `json::parse` → 再生成领域物件和 timing。原文本和 DOM 都活到函数返回。
  - 当前行为：没有先判断输入大小，也未看到 parser 层深度/节点预算。合法但很大的文件会同时保留文本、DOM、转换中间结构和最终模型。`allow_exceptions=false` 只处理语法错误，并不能替代内存预算。
  - 整改：优先给导入入口明确可配置或产品约定的字节/节点/深度上限，读取失败需检查；解析完后尽早释放不用的原文本。只有真实负载证明需要时再设计 SAX/分段转换，避免为了“泛化所有 JSON”重写格式语义。
  - 验收：大文件、极深 JSON、超多空节点和正常大谱面均有明确结果；拒绝发生于大规模模型构造前；记录峰值 RSS，既有 Malody 扩展字段往返保留。

  - 已限制为 64 MiB、200 万个 JSON 结构项（含对象键）及 64 层容器。字节超限在分配前拒绝，SAX 预检在超限处停止，通过后才构建 DOM，进入领域转换前释放原文。Linux RelWithDebInfo 全量构建及 8 项相关回归通过，覆盖深度临界值、超限、BOM、注释和扩展字段。
  - 同机使用实际加载入口，预热后取三次中位数。65,536 音符仍全部载入，耗时由 129.5 增至 137.3 ms，峰值 RSS 为 81.1 / 81.2 MiB；超限宽数组的峰值 RSS 从 170.4 降至 18.6 MiB，超限字节样本从 125.0 降至 16.3 MiB。
  - 注释／代码行数：`LoadMalodyMap.hpp` 409/922（30.73%），`MalodyEdgeCaseTest.cpp` 1171/2732（30.00%），`Modules/MMM/CMakeLists.txt` 57/131（30.32%）。

- [x] **FMT-03（P2）资产 SHA-256 用两份整文件内存，且不支持校验中取消**

  - 位置：`Modules/Network/src/AssetSyncService.cpp:1828-1833`、`:430-445`。
  - 调用链：本地资源匹配（:896）、完整包下载验证（:1015）、增量资源下载验证（:1303）→ `sha256File` → `readFileBytes`（:360-379）→ `sha256Bytes`。
  - 当前行为：先按文件总长 `resize` 读入 bytes，再把 input 完整复制到 data 以追加 SHA padding。峰值至少含两份完整内容，追加填充还可能触发 vector 扩容；`readFileBytes` 没有应用级预算且整数转换未先检查可表示范围。外层在校验前后可取消，哈希内部没有取消点。
  - 整改：流式固定块 SHA-256，统一错误和可选取消回调；内存数据入口也不应复制整份输入只为末尾填充。协作资源加密已有分块 `mbedtls_sha256_update` 先例（`CollaborationResourceCipher.cpp:462-511`），可研究共享底层摘要能力，但不要把加密协议耦合到资产清单。
  - 验收：空文件、边界块长和已知向量摘要一致；多 GiB 文件内存近似常量；读取中断/错误不产生可接受摘要；取消在块边界响应。

  - 已验证。资产使用 mbedTLS 分块 SHA-256 和固定 32 KiB 缓冲，本地差异检查、整包和增量文件摘要均响应取消。长度不符、读取失败或取消不会发布部分摘要，原有公开调用仍返回小写十六进制或空值。
  - Linux RelWithDebInfo 全量构建及相关 6 项 CTest 通过，覆盖已知向量、读取和填充边界、读途中截短及一次性取消。32 MiB 与 3 GiB 稀疏文件的摘要均与 sha256sum 一致，实测进程峰值 RSS 分别为 12,292 KiB 和 12,248 KiB。
  - 注释／代码为 AssetSyncService.cpp 486/1132、AssetSyncServiceTest.cpp 348/811、Network/CMakeLists.txt 25/54，逐文件均达到 30%。

- [x] **FMT-04（P2）资产 ZIP 同时持有完整压缩包与完整解压条目，缺少展开预算**

  - 位置：`Modules/Network/src/AssetSyncService.cpp:1724-1734`、`:1795-1808`。
  - 调用链：下载包 SHA 校验后（:1013-1039）或本地包安装（:1112）→ `extractZipArchive` → 完整 ZIP `readFileBytes` → `mz_zip_reader_init_mem` → 每条 `mz_zip_reader_extract_to_heap`。
  - 当前行为：内存为压缩包大小加当前解压条目大小，单个大字体/图像/音频或高压缩比条目可在一次解压中占用大块内存。循环未依据 `fileStat` 检查单条/累计解压大小或条目数量预算；取消只在条目之间检查，单条大文件不可中断。已有路径安全检查不能替代资源规模限制。
  - 整改：文件 reader 加回调/文件流式解压，明确单条、总展开量和文件数预算；输出分块时检查取消，失败维持同步版本标记不提前发布。
  - 验收：合法大条目峰值内存有界；超预算压缩包在写满磁盘或大分配前拒绝；单条内取消可响应；目录穿越等既有路径约束保持。这里只证明资源预算问题，不推断其下载渠道已被攻击。

  - ZIP 以原生路径打开，按偏移读取并按块解压。写盘前预检最多 10,000 个条目、单条 512 MiB、累计 4 GiB。miniz 单次工作缓冲分配上限为 16 MiB。每条文件在暂存区通过长度、CRC 和关闭检查后再重命名；块内取消保留原同名资源，整包失败不推进版本标记。
  - Linux RelWithDebInfo 全量构建及 3 项资源相关回归通过，覆盖块内取消、暂存清理、重试与原有路径约束。独立进程探针中，160 MiB 条目解压前后 SHA-256 相同，峰值 RSS 由 169.4 降至 20.6 MiB。单条或累计超预算一字节均在创建输出目录前拒绝。
  - 注释／代码：`AssetSyncService.cpp` 434/1012、`AssetZipExtraction.cpp` 103/240、`AssetPathSafety.h` 21/43、`AssetSyncServiceTest.cpp` 381/887、`Modules/Network/CMakeLists.txt` 25/56，均达到 30%。

- [x] **FMT-05（P3）离线音频时间线在无 DSP 情况下也强制所有资源完整驻留**

  - IMD 混音和首拍原点对齐通过同步解码按块写入分声道暂存文件。原声按文件路径去重，EQ 和变速／变调结果按处理配置去重；混音仍按独立事件调度。实时音频回调只读取原有内存缓存。
  - 导出暂存的原始和处理后 PCM 总计上限为 4 GiB。需要 DSP 时，一次只装入一个配置，输入与变速输出的平面数组合计上限为 256 MiB。超限或暂存 IO 失败时返回诊断；编码及混音成功后才在同一文件系统发布目标文件。
  - Linux Clang 22 RelWithDebInfo 全量构建、10 项相关 CTest 通过。回归回读裁头、补静音和覆盖旧输出的 PCM，并检查 EQ 输入以及变速输入加输出的预算边界。
  - 两份 120 秒、48 kHz 立体声夹具的旧版／新版对照中，中性、EQ、0.5 倍速和多资源多配置的输出帧数及 PCM SHA-256 全部一致。进程峰值 RSS 分别由 60704、103452、198340、150044 KiB 降至 15372、54720、150452、53828 KiB。
  - 注释／代码行数分别为 `AudioTimelineExportService.cpp` 177/412、`AudioTimelineMixerNode.cpp` 432/1012、`AudioTimelineMixerNode.h` 327/219、`AudioTimelineResourceProcessor.cpp` 121/271、`AudioTimelineResourceProcessor.h` 24/28、`AudioOriginAlignmentServiceTest.cpp` 58/137、`AudioTimelineResourceProcessorTest.cpp` 78/169。各文件达到 30%。

- [x] **FMT-06（P3）协作文档限额检查发生在完整 CBOR 编码之后，并无条件先复制正文**

  - 位置：`Modules/Network/Collaboration/src/BeatmapDocumentCodec.cpp:140-181`，快照 :1599-1612。
  - 调用链：`CollaborationRoom.cpp:880` 本地 mutation 编码 → `BeatmapDocumentCodec::encode` → `makeDocument`/`makeMutationDocument` → `encodeDocumentPayload`。快照还复制 current 为 payload，并在成功后保留 baseline。
  - 当前行为：`Json::to_cbor(document)` 已创建完整 raw，随后才比较 64 MiB 上限；`body = raw` 又复制一份，压缩 candidate 与最终 output 可再同时存在。接收端在分配前检查 rawSize 的保护是真实有效的，不能把这里说成“没有任何协议限额”；缺口在本地发送侧的限额时机与峰值复制。
  - 整改：编码前做对象/字段预算或使用可在达到限额时停止的输出 sink；避免未压缩 body 无条件复制，未压缩时直接从 raw 构造输出。先处理局部复制，再决定是否值得重构整个编码器。
  - 验收：超限本地文档在超过预算前终止编码；压缩/不压缩快照、增量、基线成功提交与 EmptyPayload 语义不变；记录临界 64 MiB 附近峰值 RSS。

  - CBOR 直接写入预留协议头的有界缓冲。流达到 64 MiB 后停止保存字节，JSON 库仍可走访余下节点。压缩仅采用更短的结果，未压缩分支直接返回原缓冲。主发送入口的快照成功后移动到基线，拒绝超限时旧基线仍有效。
  - Linux RelWithDebInfo 全量构建，5 项协作回归通过。回归覆盖 64 MiB 整、超出一字节、失败后 EmptyPayload，以及压缩快照与增量往返。独立进程探针的小快照和临界压缩快照线格式哈希一致。临界成功样本峰值 RSS 为 400.6 → 264.6 MiB，超出一字节样本为 328.4 → 264.2 MiB。
  - 注释／代码行数：`BeatmapDocumentCodec.cpp` 706/1647，`BeatmapDocumentCodecTest.cpp` 364/849，均达到 30%。

## 音频引擎

- [x] **ICE-01 · P1 · 重新准备同采样率、不同声道数时同步压缩器状态。** `3rdpty/sources/IonCachyEngine/src/ice/core/effect/IEffectNode.cpp:19` 的 prepare 更新输入缓冲；`Compresser.cpp:33、47、70` 仅在脏标记或采样率改变时调整 envelope_db，却按新输出声道逐个索引。增加声道时可超过旧状态范围。把声道状态尺寸纳入控制侧准备契约。验收覆盖重新 prepare、增减声道、相同块长和保留旧声道状态的约定。独立 ICE 源码全量构建及 IonTimeStretcherRealtimeTest 已通过，新增基类接口声道切换、保留已有声道包络及回调零分配回归。
- [x] **ICE-02 · P2 · 消除压缩器逐样本读取共享参数。** 同文件第 43、92 行计算了补偿倍率缓存，样本循环仍反复读取原子 dB 值。头文件 `Compresser.hpp:22–47、96` 的 setter 又写普通布尔脏位。先确定是否允许播放中调参；若允许，在块边界取得一致参数，控制侧完成可预计算的部分。若要求停机修改，收紧 API 并检查调用方。验收固定参数 PCM、切参边界、回调内分配次数和耗时，不将原子读取一律改成裸读。 独立 ICE 源码构建与 IonTimeStretcherRealtimeTest 通过。setter 仅在接入音频图前或停止后调用；演示程序与测试调用符合此约束。处理块读取一次补偿并复用压缩系数。新增双声道停机调参用例，逐样本验证 6 dB 补偿及回调零分配。Compresser.hpp 注释/代码为 31/46 行（40.26%），Compresser.cpp 为 26/59 行（30.59%），TimeStretcherRealtimeTest.cpp 为 566/1319 行（30.03%）。
- [x] **ICE-03 · P2 · 让 PitchAlter 遵守准备容量。** `3rdpty/sources/IonCachyEngine/src/ice/core/effect/PitchAlter.cpp:46` 的 process 每块调用 resize，绕过 `IEffectNode.cpp:47` 的预备容量检查。核对支持的最大块长和格式切换协议，稳定路径只切有效长度；分别测同尺寸、短块、超限块和重新准备后的分配计数。调用 resize 不等于每块均发生分配。 已复用基类容量检查和活动帧数，变调后端按固定倍率在控制侧构造并预热；倍率只可停机修改。独立 ICE 源码全量构建和 IonTimeStretcherRealtimeTest 通过，覆盖满块、短块、超限、格式重配、首次变调、断开上游及回调零堆操作。主仓使用既有预编译依赖全量构建，PlaybackRestartTest、CanvasTouchNavigationTest 和 AudioManagerTimelineIntegrationTest 通过。PitchAlter.hpp 注释／代码为 23/29 行、PitchAlter.cpp 为 38/84 行、TimeStretcherRealtimeTest.cpp 为 603/1405 行，均达到 30%。

## 编辑器、临时实现与标准库

- [x] **UI-01 · P1 · 超长元数据进入编辑器时保全原文。** `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp:273、1000、1068、1186` 以 N−1 字节截断 OSU 文本和 JSON，未把截断状态带到提交处。若 OSU 尾部恰好截在一条完整记录后，剩余文本仍可通过解析并生成不完整的属性表。改为按编辑内容扩容，或明确拒绝载入并保全原文；覆盖 32/64 KiB 边界、UTF-8 多字节边界、取消和原样确认。已改为完整字符串缓冲。UI 套件重新链接，MobileInputEditorTest 的两种 UTF-8 边界和 BpmBeatmapImportTest 均通过。
- [x] **UI-02 · P2 · 仅在编辑发生时解析字段 JSON。** 同文件第 1316、1373 行，弹窗打开且可见时，未编辑的一帧也解析同一个缓冲两次。用文本修订标记让编辑、格式化、重建和子字段操作都使缓存失效；完成按钮必须看到最后一次输入。验收静置时解析次数为零，每次修改后的错误提示与提交值一致。 已用弹窗独占文档缓存解析值和诊断，静止帧只读缓存，文字变化时解析一次，格式化与结构编辑复用已解析的 JSON。Linux RelWithDebInfo 全量构建通过，MobileInputEditorTest、BpmBeatmapImportTest、TimelineBatchMetadataTest 通过，覆盖空闲读取、无效输入、重建、字段增删、非对象及确认前的最后一次编辑。MetadataEditorWindowRenderers.cpp 注释/代码为 1083/2526 行（30.01%），MetadataJsonDocument.h 为 22/50 行（30.56%），MobileInputEditorTest.cpp 为 166/386 行（30.07%）。
- [x] **UI-03 · P3 · 明确全局搜索面板的产品状态。** `Modules/Game/UI/src/ui/imgui/manager/SearchView.cpp:51–62` 输入事件处理为空，所有查询均显示无结果。接入索引时区分未搜索、搜索中和无结果，覆盖过期结果丢弃及跳转；若功能暂缓，应在入口说明可用性。项目配置中的未来扩展备注未发现当前行为缺口。 索引服务接入前，侧栏明确显示本地化的“全局搜索暂未开放”，移除不可执行的输入框，保留布局标识。Linux RelWithDebInfo 全量构建通过，默认翻译同步、语言键对齐、皮肤翻译覆盖及桌面和移动停靠回归共 6 项通过。SearchView.cpp 注释／代码为 16/23 行（41.03%），SearchView.h 为 14/15 行（48.28%）。
- [x] **STD-01 · P2 · 梳理四个原子发布位置的所有权。** 12 处共享指针 atomic 自由函数调用位于 `Modules/Game/Logic/src/logic/SessionRegistry.cpp:266`（含两种快照）、`RenderSyncRegistry.cpp:215`、`session/BeatmapSession.cpp:578` 和 `session/BeatmapSession_Commands.cpp:2598`。分别登记写者、读者、持有时长与最后释放线程；会话/图集读者优先每轮取得一次句柄，内部传引用。观察者回调后第 2613 行重新获取用于确认身份，不能拿回调前缓存替代。验收旧快照跨关闭仍可用、图集替换、观察者回调中替换及目标工具链弃用告警；仅替换为 atomic<shared_ptr> 仍有所有权和潜在内部同步成本。 已移除 12 处弃用调用。会话逻辑每轮借用同一代会话快照，主画布普通帧仅取得一次 UI 快照，UV 别名移交读者持有权。写者、读者、持有期及最后回收线程记录在四个发布槽旁；libc++ 兼容锁只覆盖一次指针复制或交换。Linux 全量构建及 15 项回归通过，含旧快照跨关闭、图集替换和回调内换绑。libstdc++、libc++、AppleClang（最低部署 11.0）并发／重入探针通过；debsrv MSVC 交叉编译及 Wine 探针通过。
- [x] **STD-02 · P2 · 导出路径检查显式处理文件系统错误。** `Modules/Game/UI/src/ui/imgui/menu/actions/file/SaveBeatmapAsAction.cpp:213` 使用可能抛出的 exists(path)。改用 error_code 重载，访问失败保留待导出内容并报告原因；分别验证路径不存在、已有文件及父目录无访问权限。 已使用 error_code 重载，查询失败走统一导出失败反馈并中止当次导出，现有目标仍进入覆盖确认。Linux 全量构建、SaveResultFeedbackTest 和 BpmBeatmapImportTest 通过；同编译器文件系统探针验证缺失、已存在及父目录权限拒绝的三种结果。SaveBeatmapAsAction.cpp 注释 246 行、代码 570 行，30.15%。
- [x] **STD-03 · P3 · 明确分配诊断与禁异常规范的边界。** `3rdpty/sources/IonCachyEngine/src/diagnostics/AllocationTracker.cpp:25–32、86` 在测试诊断替换的普通 operator new 中抛 bad_alloc，并使用 cout 输出。核对构建隔离并明确诊断目标例外，或迁移到可表达失败的专用测试钩子；不能简单让普通抛出型 new 在失败时返回空指针。验收生产库未链接替换符号，计数测试仍能观测分配。 独立 ICE 诊断可执行文件通过私有编译开关启用全局替换，诊断源文件缺少开关时拒绝编译；保留抛出型 operator new 的 bad_alloc 契约，输出改用独立演示程序现有的 fmt。Linux ICE 全量构建及 IonTimeStretcherRealtimeTest 通过。符号检查确认生产静态库无替换定义；诊断探针验证零字节成功、失败抛出和 2 次分配尝试／1 次释放的计数。主仓全量构建与 PlaybackRestartTest、CanvasTouchNavigationTest、AudioManagerTimelineIntegrationTest 通过。AllocationTracker.cpp 注释／代码 41/48 行，ICE src/CMakeLists.txt 为 24/52 行，均达到 30%。

## 头文件与注释

- [x] **HDR-01 · P2 · 从只借用配置的接口切断聚合头。** `Modules/Config/include/config/EditorConfig.h:3、69` 同时定义按值容器和引用视图；内联重置方法又将默认值构造暴露给所有消费者。Ninja 记录 EditorConfig 191 个对象、EditorSettings 271 个、BeatLinePalette 和 NotePalette 各 278 个。优先分离引用视图并外移依赖完整定义的实现，保留按值成员；相同构建配置比较实际对象数和增量耗时。 已将只读视图及拖动实体列表拆为独立头，恢复默认值的实现移出聚合头。保留配置所有权、字段默认值和视口渲染行为。Linux RelWithDebInfo 全量构建及 21 项相关 CTest 通过。同一 Ninja 构建树有效依赖中，EditorConfig.h 由 198 降至 191 个对象，EditorSettings.h 由 279 降至 275，VisualConfig.h 为 234；只触碰 EditorConfig.h 时，三个抽查的渲染对象由需编译变为无工作，耗时由 4.334 s（含一次 CMake 重生成）降至 0.027 s。新增和修改的代码逐文件注释率均达到 30%。
- [x] **HDR-02 · P3 · 调色文件接口独立于完整编辑设置和 JSON 实现。** `Modules/Config/include/config/ColorPaletteFile.h:3、34` 仅为一个按值方案引入 EditorSettings，并内联 JSON 转换。提取方案类型及 ADL 声明，转换移入 cpp；该文件本身仅传播到 3 个已记录对象，收益单独计量。验收合法/非法配色导入与现有文件往返。 已提取按值配色方案和 JSON ADL 声明，并将分享文件转换移入实现。Linux RelWithDebInfo 全量构建及 6 项回归通过，覆盖旧示例往返、同路径覆盖、坏版本与坏颜色保持旧值。Ninja 同配置记录 EditorSettings.h 从 281 降至 279 个对象，去掉调色文件实现及测试两条依赖；ColorPaletteFile.h 仍为 3 个对象，新的窄方案头为 281 个对象。注释／代码为 EditorSettings.h 238/329、ColorPaletteScheme.h 7/15、ColorPaletteFile.h 19/22、ColorPaletteFile.cpp 75/166、ColorPaletteFileTest.cpp 72/165，均达到 30%。
- [x] **HDR-03 · P2 · 用真实消费者验收会话拆分。** SessionContext 对应 73 个对象、EditorEngine 对应 96 个；PERF-05 列出 UI 直接取锁访问的实例。先迁移批注读模型或交互状态，再对照活动对象清单；降低直接 include 个数不足以证明扇出下降。 批注表已改用窄接口。同一 Linux Debug Ninja 构建树中，按活动对象与实际依赖记录交集统计，SessionContext.h 从 72 降至 71，EditorEngine.h 从 98 降至 97。AnnotationTableData.cpp 的编译记录仅保留批注发布头作为 Logic 依赖。
- [x] **DOC-01 · P2 · 修订失真的契约说明。** `Modules/MMM/include/mmm/SafeParse.h:141` 的旧 stod 兼容描述需写明前导加号、消费长度和平台语法；`Modules/Game/include/game/GlobDefs.h:79` 的 UTF-8 声明需以初始化结果为准；对照 PERF-03 更正时间格式化调用频率。每条用相应输入或调用链验收。 已分别注明浮点前缀的空白、符号、消费长度与平台差异，启动 C locale 设置的失败语义，以及热路径使用快照索引。确认三处非注释代码一致。Linux 全量构建和 LC_ALL=C 下 6 项相关 CTest 通过；独立探针验证八个解析断言及本机 .UTF-8 失败后保留 C locale，并对照状态栏、预览、时间线及音频视图调用链。注释／代码为 SafeParse.h 49/102、GlobDefs.h 43/95、TimeFormatUtils.cpp 90/196，逐文件均达 30%。
- [x] **DOC-02 · P3 · 让函数体注释解释选择依据。** `Modules/Game/Graphic/src/imguivk/context/VKContextDebugFeats.cpp:15–33` 中多条注释逐词复述 eError、eWarning、eGeneral 等枚举。保留需要解释的筛选理由、回调生存期和不可重入约束；无需为显然枚举逐项写释义。修订后逐文件检查 cloc 和函数体关键实现。 已说明实例创建与运行期共用的过滤配置、调用线程及并发进入、数据借用寿命和不可重入约束。前后非注释代码 token 一致。Linux RelWithDebInfo 全量构建，以及 VulkanHandleFwdTest、CanvasCameraTest、ComparisonCanvasTest 通过。VKContextDebugFeats.cpp 注释／代码 39/87 行（30.95%）。
- [x] **STACK-01 · P3 · 补齐不定长栈分配的编译器证据。** 1,242 文件文本扫描未命中显式 alloca、_alloca、_malloca 或 __builtin_alloca 调用；主仓常见声明模式未定位 VLA。模板、别名和宏展开仍需以各平台编译器诊断确认。用支持的 GCC/Clang 配置开启 VLA 诊断并检查平台分支；对 `ProjectController.cpp:815`、`BeatmapSession_Commands.cpp:880` 等固定 64 KiB 局部缓冲用栈使用报告核对同一调用链峰值和工作线程栈预算。固定数组、对象成员与输入决定长度的栈分配要分别判断。

  - 在 `c87ab1b4` 启用 `-Werror=vla -Werror=alloca` 检查当前构建启用的自维护 C++ 编译命令，包含自维护 ICE 源码和已注册测试的生成入口。Linux Clang 22 为 611 个，Windows MSVC ABI clang-cl 为 613 个，macOS AppleClang ARM64（部署目标 11.0）为 612 个，Android ARM64 NDK 为 416 个，均通过。
  - 以 RelWithDebInfo 优化参数重编相关五个翻译单元并生成 `-fstack-usage` 报告。哈希路径依次经过编辑循环、会话更新和保存或加载命令，临时谱包解压由项目打开路径准备。两个固定数组位于不同调用链，所核对的项目静态帧分别计入各自路径。数据为编译器报告及线程预算对照，未测量第三方调用或整进程运行时高水位。

    | 平台 | 读谱包的含缓冲静态帧 | 文件哈希静态帧 | 工作线程栈预算 |
    | --- | ---: | ---: | --- |
    | Linux x86_64 | 66,744 B | 66,120 B | 8 MiB，本机线程探针 |
    | Windows x64 | 66,232 B | 65,912 B | 1 MiB，PE 保留值与 Wine 线程探针 |
    | macOS ARM64 | 66,752 B | 66,208 B | 536,576 B，Mac Runner 线程探针 |
    | Android ARM64 | 66,560 B | 65,984 B | AOSP Bionic 默认 992 KiB，未在 Android 设备上采样 |

  - Linux 上临时谱包、Unicode 路径、IMD/BMS 导出、保存反馈及会话流程共 8 项 CTest 通过。Android 的默认栈大小依据 [AOSP Bionic 源码](https://android.googlesource.com/platform/bionic/+/d4c94194416f0c3727bdae51163932694dccca24/libc/bionic/pthread_internal.h)。

## 注释率逐文件待办

以下使用 cloc 2.08 的 comment / (comment + code)，按文件独立计算。比例只用于检查项目门槛。长期任务应按职责分批补充实现约束，避免用逐句翻译提高比例。

本次取得 1478 份逐文件计数，另有 5 个零字节头文件不适用。低于 30% 的 69 个文件如下。包含资源中的 Lua 与文档中的可运行 Lua 示例，外部谱面样例脚本应先认定维护归属再决定是否修改。

| 待办文件 | 注释行 | 代码行 | 比例 |
|---|---:|---:|---:|
| `3rdpty/cmake/modules/FindEnTT.cmake` | 1 | 13 | 7.14% |
| `3rdpty/cmake/modules/FindImGuiFileDialog.cmake` | 1 | 44 | 2.22% |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | 3 | 54 | 5.26% |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | 2 | 57 | 3.39% |
| `3rdpty/cmake/modules/Findclay.cmake` | 1 | 8 | 11.11% |
| `3rdpty/cmake/modules/Findconcurrentqueue.cmake` | 4 | 13 | 23.53% |
| `3rdpty/cmake/modules/Findcurl.cmake` | 5 | 59 | 7.81% |
| `3rdpty/cmake/modules/Findfftw.cmake` | 1 | 34 | 2.86% |
| `3rdpty/cmake/modules/Findfmt.cmake` | 1 | 37 | 2.63% |
| `3rdpty/cmake/modules/Findfreetype.cmake` | 2 | 49 | 3.92% |
| `3rdpty/cmake/modules/Findglfw.cmake` | 2 | 49 | 3.92% |
| `3rdpty/cmake/modules/Findglm.cmake` | 1 | 13 | 7.14% |
| `3rdpty/cmake/modules/Findimgui.cmake` | 2 | 47 | 4.08% |
| `3rdpty/cmake/modules/Findimplot.cmake` | 2 | 37 | 5.13% |
| `3rdpty/cmake/modules/Findlame.cmake` | 1 | 31 | 3.12% |
| `3rdpty/cmake/modules/Findlibdatachannel.cmake` | 1 | 53 | 1.85% |
| `3rdpty/cmake/modules/Findlibjuice.cmake` | 1 | 39 | 2.50% |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | 1 | 37 | 2.63% |
| `3rdpty/cmake/modules/Findluajit.cmake` | 1 | 47 | 2.08% |
| `3rdpty/cmake/modules/Findlunasvg.cmake` | 1 | 78 | 1.27% |
| `3rdpty/cmake/modules/Findmbedtls.cmake` | 1 | 136 | 0.73% |
| `3rdpty/cmake/modules/Findminiz.cmake` | 1 | 31 | 3.12% |
| `3rdpty/cmake/modules/Findnfd.cmake` | 3 | 56 | 5.08% |
| `3rdpty/cmake/modules/Findnlohmann_json.cmake` | 1 | 15 | 6.25% |
| `3rdpty/cmake/modules/Findrubberband.cmake` | 1 | 52 | 1.89% |
| `3rdpty/cmake/modules/Findsol2.cmake` | 1 | 13 | 7.14% |
| `3rdpty/cmake/modules/Findspdlog.cmake` | 2 | 43 | 4.44% |
| `3rdpty/cmake/modules/Findstb.cmake` | 1 | 8 | 11.11% |
| `3rdpty/cmake/modules/Findusrsctp.cmake` | 2 | 44 | 4.35% |
| `3rdpty/cmake/modules/Findzlib.cmake` | 1 | 38 | 2.56% |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 33 | 430 | 7.13% |
| `3rdpty/sources/cmake/GLFW.cmake` | 5 | 17 | 22.73% |
| `3rdpty/sources/cmake/ImGuiFileDialog.cmake` | 2 | 24 | 7.69% |
| `3rdpty/sources/cmake/PatchLuaJITMsvcRuntime.cmake` | 1 | 25 | 3.85% |
| `3rdpty/sources/cmake/clay.cmake` | 0 | 5 | 0.00% |
| `3rdpty/sources/cmake/entt.cmake` | 0 | 3 | 0.00% |
| `3rdpty/sources/cmake/freetype.cmake` | 15 | 47 | 24.19% |
| `3rdpty/sources/cmake/glm.cmake` | 0 | 17 | 0.00% |
| `3rdpty/sources/cmake/implot.cmake` | 4 | 19 | 17.39% |
| `3rdpty/sources/cmake/libdatachannel.cmake` | 6 | 26 | 18.75% |
| `3rdpty/sources/cmake/mbedtls.cmake` | 3 | 33 | 8.33% |
| `3rdpty/sources/cmake/miniz.cmake` | 1 | 21 | 4.55% |
| `3rdpty/sources/cmake/nlohmann_json.cmake` | 0 | 3 | 0.00% |
| `3rdpty/sources/cmake/sol2.cmake` | 7 | 24 | 22.58% |
| `3rdpty/sources/cmake/stb.cmake` | 0 | 2 | 0.00% |
| `Modules/Config/tests/PastePositionConfigTest.cpp` | 3 | 27 | 10.00% |
| `Modules/Game/Logic/CMakeLists.txt` | 144 | 338 | 29.88% |
| `Modules/Game/Logic/src/logic/AudioBatchExportService.cpp` | 43 | 468 | 8.41% |
| `Modules/Game/Logic/src/logic/BmsPackageExportService.cpp` | 180 | 522 | 25.64% |
| `Modules/Game/Logic/src/logic/ProjectResourceService.cpp` | 867 | 2093 | 29.29% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1349 | 3204 | 29.63% |
| `Modules/Game/Logic/tests/AudioBatchExportServiceTest.cpp` | 10 | 153 | 6.13% |
| `Modules/Game/Logic/tests/BmsPackageExportServiceTest.cpp` | 299 | 776 | 27.81% |
| `Modules/Game/Logic/tests/CanvasCameraTest.cpp` | 2601 | 6219 | 29.49% |
| `Modules/Game/UI/src/ui/imgui/manager/BuiltinSettingsSearch.h.in` | 2 | 5 | 28.57% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/file/PackBeatmapAction.cpp` | 860 | 2032 | 29.74% |
| `Modules/Game/UI/src/ui/plugin/BuiltinToolPlugins.h.in` | 2 | 7 | 22.22% |
| `Modules/Game/UI/src/ui/plugin/ToolPluginView.cpp` | 483 | 1287 | 27.29% |
| `Modules/MMM/src/beatmap/BeatMap.cpp` | 118 | 277 | 29.87% |
| `Modules/MMM/src/beatmap/BmsFormat.hpp` | 138 | 416 | 24.91% |
| `Modules/MMM/src/beatmap/LoadMMMMap.hpp` | 318 | 748 | 29.83% |
| `Modules/MMM/src/beatmap/SaveBmsMap.hpp` | 137 | 518 | 20.92% |
| `Modules/MMM/tests/BmsFormatTest.cpp` | 254 | 746 | 25.40% |
| `Modules/Network/Collaboration/src/BeatmapDocumentCodec.cpp` | 687 | 1604 | 29.99% |
| `assets/plugins/tools/audio.lua` | 97 | 297 | 24.62% |
| `assets/translations/en_us.lua` | 20 | 1823 | 1.09% |
| `assets/translations/zh_cn.lua` | 20 | 1823 | 1.09% |
| `docs/plugin/examples/ivm.lua` | 12 | 111 | 9.76% |
| `tests/data/ma/驟雨の狭間/r.lua` | 6 | 475 | 1.25% |

## 2026-11-24 复查追加待办

主仓 origin/main 为 `19c4550d5ce0971a76eb9d020423aaf5c6692ffe`，gitlink 为 `a2b6be99a881072ef851ef197862d447a980100b`。独立 ICE origin/main 为 `038f3ff`，两者 Git 源码树一致。以主仓清单的具体调用入口和验收项去重。P1 涉及文件完整性或对象寿命，P2 涉及功能、交互、性能或跨平台构建，P3 为维护性及低频故障。

该报告以受版本控制的自维护代码为范围，按模块搜索候选并追调用链；排除外部第三方源码、预编译内容和生成文件。两项更新器故障及 ICE 公开头消费者做了独立复现。其他运行影响按触发条件列出；性能项尚无实测帧时间。

### P1 · R01 · 保存写入失败可能清除脏状态

- [x] 写出器只查打开结果或只在关闭前查流状态。调用方收到成功就推进保存点，原路径可能只剩半份谱面。逐格式注入短写和关闭错误，验证返回失败、保留旧文件及未保存状态；备份也应拒绝残缺结果。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 14 项回归通过。对五种格式注入真实短写，保存返回失败且旧谱面逐字节保留。

[Modules/MMM/src/beatmap/SaveMMMMap.hpp:678](../Modules/MMM/src/beatmap/SaveMMMMap.hpp#L678) [Modules/MMM/src/beatmap/SaveRMMap.hpp:506](../Modules/MMM/src/beatmap/SaveRMMap.hpp#L506) [Modules/MMM/src/beatmap/SaveOSUMap.hpp:452](../Modules/MMM/src/beatmap/SaveOSUMap.hpp#L452) [Modules/MMM/src/beatmap/SaveMalodyMap.hpp:1410](../Modules/MMM/src/beatmap/SaveMalodyMap.hpp#L1410) [Modules/MMM/src/beatmap/SaveBmsMap.hpp:655](../Modules/MMM/src/beatmap/SaveBmsMap.hpp#L655) [Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp:4242](../Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp#L4242)

### P1 · R02 · 资源导入可能覆盖另一处编辑

- [x] 弹窗按工程根和谱面路径校验，拿旧草稿补几个实时字段后提交整份元数据，绕过 PERF-05 的实例及差量保护。打开选择器后修改标题、同路径重载，再导入封面，验证只改变目标资源且拒绝旧实例。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 3 项回归通过，覆盖导入期间排队的标题更新和同路径重载后拒绝旧请求。

[Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:204](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L204) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:221](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L221) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:273](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L273) [Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp:2731](../Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp#L2731)

### P1 · R03 · UI 借用可由逻辑线程释放的工程对象

- [x] 向导在绘制期间遍历音源，工程设置页还直接修改配置。关闭工程时逻辑线程移走并销毁唯一所有者。两条调用链缺少跨线程寿命保证，可能访问失效内存或写错工程。用带工程代次的只读快照及写命令，交错切换、关闭和编辑，检查寿命及写入归属。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 6 项回归通过。覆盖同目录重开后旧代次拒绝、快照跨关闭保活、不同设置字段合并，以及向导导入和改名。

[Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1470](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1470) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1577](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1577) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_ProjectTab.cpp:67](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_ProjectTab.cpp#L67) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_ProjectTab.cpp:563](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_ProjectTab.cpp#L563) [Modules/Game/Logic/src/logic/ProjectController.cpp:2206](../Modules/Game/Logic/src/logic/ProjectController.cpp#L2206) [Modules/Game/Logic/src/logic/EditorEngine.cpp:2196](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L2196)

### P1 · R04 · Linux 更新失败可破坏原有安装

- [x] 跨文件系统 rename 失败后直接覆盖正式可执行文件。Linux 临时目录实测，源 65536 B、旧目标 4096 B，限制单文件写入 1024 B 后更新器报错退出，旧目标只剩 1024 B。在目标目录写临时文件，验证权限及关闭结果后原子替换；同一短写条件下旧文件应逐字节保留。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 2 项更新回归通过。原生更新器跨卷短写后保留旧安装与下载源，未留下成功标记。

[Modules/Updater/main.cpp:644](../Modules/Updater/main.cpp#L644)

### P2 · R05 · 独立配置文件的保存请求互相覆盖

- [x] 所有目标共用一个待写槽和全局代次；后排的 B 可以使尚未落盘的 A 失效，flush 仍结束。按目标路径合并，交错两路径的异步和同步保存并核对两个文件。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 3 项回归通过。验证两个目标的异步交错、同步覆盖及 flush 顺序。

[Modules/Config/src/ConfigSnapshotWriter.cpp:116](../Modules/Config/src/ConfigSnapshotWriter.cpp#L116) [Modules/Config/src/ConfigSnapshotWriter.cpp:134](../Modules/Config/src/ConfigSnapshotWriter.cpp#L134) [Modules/Config/src/ConfigSnapshotWriter.cpp:202](../Modules/Config/src/ConfigSnapshotWriter.cpp#L202) [Modules/Config/src/AppConfig.cpp:339](../Modules/Config/src/AppConfig.cpp#L339)

### P2 · R06 · 排序比较器违反严格弱序

- [x] 相邻时间差在 ε 内时切换到次键。取时间 0、0.75ε、1.5ε、轨道依次 2、1、0，可同时得到第三个排第二个之前、第二个排第一个之前、第一个又排第三个之前。影响物件索引与多格式导出。建立可传递的排序键，测三元组及输入排列。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 11 项回归通过。循环三元组的六种输入排列在物件索引、原生采样和批注输出中均按时间先后排列。

[Modules/MMM/src/beatmap/BeatMap.cpp:393](../Modules/MMM/src/beatmap/BeatMap.cpp#L393) [Modules/MMM/src/beatmap/SaveOSUMap.hpp:428](../Modules/MMM/src/beatmap/SaveOSUMap.hpp#L428) [Modules/MMM/src/beatmap/SaveMMMMap.hpp:266](../Modules/MMM/src/beatmap/SaveMMMMap.hpp#L266) [Modules/MMM/src/beatmap/SaveMMMMap.hpp:654](../Modules/MMM/src/beatmap/SaveMMMMap.hpp#L654) [Modules/MMM/src/beatmap/SaveMalodyMap.hpp:1302](../Modules/MMM/src/beatmap/SaveMalodyMap.hpp#L1302) [Modules/MMM/src/beatmap/SaveMalodyMap.hpp:1366](../Modules/MMM/src/beatmap/SaveMalodyMap.hpp#L1366)

### P2 · R07 · IMD 读取前未核实长度

- [x] seek/tell 失败可将负位置转成巨大的 size_t，正常大文件也按整文件一次分配。读取前检查定位、长度预算和实际读完状态；回归无法定位、短读和超过预算的输入。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 3 项回归通过。IMD 读入检查定位、64 MiB 预算和实际读取长度，覆盖定位失败、短读、增长、超限和正常往返。

[Modules/MMM/src/beatmap/LoadRMMap.hpp:244](../Modules/MMM/src/beatmap/LoadRMMap.hpp#L244)

### P2 · R08 · 更新器两次进程启动都可能误报成功

- [x] POSIX 父进程只知道 fork 成功就退出；Linux 更新器还忽略执行位设置失败。用不可执行格式在 Linux 临时目录实测，更新器退出 0，留下成功标记，stderr 为空。两次交接分别确认 exec 结果，测试失败后主程序的存活、错误及标记语义。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 2 项更新回归通过。测试实际装载失败后主程序继续运行，新程序无法装载时更新器返回错误并撤下标记，成功启动可立即读取该标记。macOS open 返回状态检查已审阅，原生 macOS 回归未执行。

[Modules/Network/src/UpdateChecker.cpp:689](../Modules/Network/src/UpdateChecker.cpp#L689) [Modules/Updater/main.cpp:458](../Modules/Updater/main.cpp#L458) [Modules/Updater/main.cpp:665](../Modules/Updater/main.cpp#L665)

### P2 · R09 · Windows 更新器路径在进程边界丢失 Unicode

- [x] 主程序用宽字符启动更新器，后者从窄 argv 解析安装与下载路径，又调用 ShellExecuteA。安装路径超出 ANSI 代码页时可能无法替换或重启。用含非当前代码页字符的路径走完整更新回归。

  已验证。Linux 全量构建和 2 项更新回归通过。debsrv 上 clang-cl 23 与 llvm-mingw 的 Windows x64 构建均通过，两个产物在原生 Win10 ACP 936 上均验证含空格和非 BMP 字符的下载路径、程序替换、成功标记及实际重启路径和工作目录。

[Modules/Network/src/UpdateChecker.cpp:657](../Modules/Network/src/UpdateChecker.cpp#L657) [Modules/Updater/main.cpp:549](../Modules/Updater/main.cpp#L549) [Modules/Updater/main.cpp:118](../Modules/Updater/main.cpp#L118)

### P2 · R10 · 更新模块仍有一套固定长度的可执行路径实现

- [x] Windows 使用 MAX_PATH，macOS 使用固定 PATH_MAX，缓冲不足直接返回空；启动模块已有动态读取路径。长路径影响更新定位及依赖此值的调用方。统一路径查询，验证长路径、符号链接及失败。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建及 4 项回归通过。原生探针覆盖 406 字节路径、从符号链接启动及可执行文件删除后的空结果。debsrv 交叉构建的探针在 Win10 返回完整的 357 UTF-16 单元路径；未单独运行 macOS 原生探针。

[Modules/Network/src/UpdateChecker.cpp:524](../Modules/Network/src/UpdateChecker.cpp#L524) [Modules/Config/src/AppPaths.cpp:27](../Modules/Config/src/AppPaths.cpp#L27)

### P2 · R11 · 新谱面向导每帧带锁复制模板，下拉中同步扫目录

- [x] 模板未展开也逐帧取会话锁并复制候选；封面或背景展开期间每帧在 UI 线程递归枚举项目。按会话代次复用模板，展开或失效时后台枚举；验证持锁时仍能绘制、下拉静置不再重复遍历。

  已验证。Linux Clang 22 RelWithDebInfo 全量构建和 5 项回归通过。覆盖会话锁占用时向导仍可绘制、源实例关闭或替换后拒绝旧模板、两个资源下拉复用扫描结果，以及同目录重开后拒绝旧列表。

[Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:328](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L328) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1565](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1565) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:134](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L134) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1807](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1807) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1845](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1845)

### P2 · R12 · 资源导入和改名持会话锁做文件 I/O

- [ ] 确认导入、共享资源复制改名及向导导入时，音频探测或文件复制发生在 UI 调用栈和会话锁内。大文件或慢盘可同时阻塞绘制与逻辑更新。拆成预检、锁外文件操作、复核身份并提交；慢盘测试期间逻辑应继续推进。

[Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:196](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L196) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:367](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L367) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp:590](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapResourceImport.cpp#L590) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1300](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1300)

### P2 · R13 · 目录监视去抖后仍在逻辑线程持锁全量扫描

- [ ] 外部改文件即可触发完整目录扫描，有变化时锁内还保存工程。文件数与存储延迟都会延长更新停顿。后台扫描，按项目代次应用差异；阻塞枚举期间测试逻辑推进及切工程。

[Modules/Game/Logic/src/logic/EditorEngine.cpp:5111](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L5111) [Modules/Game/Logic/src/logic/EditorEngine.cpp:6099](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L6099) [Modules/Game/Logic/src/logic/EditorEngine.cpp:6107](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L6107) [Modules/Game/Logic/src/logic/EditorEngine.cpp:6123](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L6123)

### P2 · R14 · 新谱面向导静默截断多字节元数据

- [ ] 从工程等来源复制到定长字符数组，再将截短文本写入新谱；UTF-8 字符可能被从中间切断。验证长标题、艺术家名和多字节边界；未编辑字段需原样保全。

[Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:88](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L88) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:548](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L548) [Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp:1476](../Modules/Game/UI/src/ui/imgui/manager/NewBeatmapWizard.cpp#L1476) [Modules/Game/UI/include/ui/imgui/manager/NewBeatmapWizard.h:274](../Modules/Game/UI/include/ui/imgui/manager/NewBeatmapWizard.h#L274)

### P2 · R15 · 设置页静置仍重建资源与路径快照

- [ ] 可见页约每 150 ms 有机会再次请求；锁内遍历工程音频表，文件线程探测当前引用，对带历史目录前缀的主音频再做存在性检查。递归目录枚举已按需，剩余开销与资源数量有关。以资源修订和文件变更失效，大工程静置统计任务数及路径探测数。

[Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp:212](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp#L212) [Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp:183](../Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp#L183) [Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp:204](../Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp#L204) [Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp:225](../Modules/Game/Logic/src/logic/BeatmapSettingsSnapshot.cpp#L225)

### P2 · R16 · 音频分析面板静置时逐帧重建 EQ

- [ ] 启用 EQ 时，每个面板每帧对 B 个频段各设两次参数；每次设置都加锁并重建各声道完整滤波链。单面板每帧约 2B 次发布，系数构造随声道数和 B² 增长。按修订批量更新；静置时发布数应为零，调参后核对 PCM。

[Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp:456](../Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp#L456) [Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp:1080](../Modules/Game/UI/src/ui/imgui/audio/AudioSpectrumView.cpp#L1080) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:360](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L360) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:683](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L683) [3rdpty/sources/IonCachyEngine/src/ice/core/effect/GraphicEqualizer.cpp:78](../3rdpty/sources/IonCachyEngine/src/ice/core/effect/GraphicEqualizer.cpp#L78) [3rdpty/sources/IonCachyEngine/src/ice/core/effect/GraphicEqualizer.cpp:232](../3rdpty/sources/IonCachyEngine/src/ice/core/effect/GraphicEqualizer.cpp#L232)

### P2 · R17 · 波形重算在绘制回调同步处理整曲

- [ ] 计算标志在同一次同步调用中设置并清除，长曲处理期间 UI 无法更新进度或响应输入。后台读取、按音轨和参数代次交付结果，回归长曲期间关闭窗口及切换曲目。

[Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:349](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L349) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:718](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L718) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:742](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L742) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:799](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L799)

### P2 · R18 · 波形统计忽略实际读取帧数

- [ ] 末桶或短读时仍按请求长度复制、滤波和取极值，未覆盖区域可保留上桶样本。使用实际返回长度，比较短读、零帧、非整桶曲尾与参考包络。

[Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:758](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L758) [Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp:783](../Modules/Game/UI/src/ui/imgui/audio/AudioWaveformView.cpp#L783) [3rdpty/sources/IonCachyEngine/src/ice/manage/AudioTrack.cpp:33](../3rdpty/sources/IonCachyEngine/src/ice/manage/AudioTrack.cpp#L33)

### P2 · R19 · 流式解码将估计长度当作硬上限

- [ ] 小估值会拒绝末尾页请求，零估值直接拒绝加载，即便后端实际仍可读。以 mock 提供三页 PCM，分别上报一页及零页，核对真实 EOF 和结尾音频。

[3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:114](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L114) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:252](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L252) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:315](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L315) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp:793](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp#L793)

### P2 · R20 · 流式长曲跳转顺序解码全部跨过的页

- [ ] 跳到远处缺页时后台从当前位置逐块追赶，后退还要回到零；前台在缺页期间推进静音。衡量远距离拖动所解码帧数及恢复有声延迟，保留定位精度回归。

[3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:161](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L161) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:168](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L168) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:325](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L325)

### P2 · R21 · 音轨缓存池在共享写锁内探测、解码和析构

- [ ] 慢资源创建及释放预读线程会挡住其他资源的缓存命中。锁外准备和析构，按资源协调并发创建；验证一条慢轨加载或回收时可立即取得另一条已缓存轨道。

[3rdpty/sources/IonCachyEngine/include/ice/manage/AudioPool.hpp:82](../3rdpty/sources/IonCachyEngine/include/ice/manage/AudioPool.hpp#L82) [3rdpty/sources/IonCachyEngine/include/ice/manage/AudioPool.hpp:138](../3rdpty/sources/IonCachyEngine/include/ice/manage/AudioPool.hpp#L138) [3rdpty/sources/IonCachyEngine/src/ice/manage/AudioTrack.cpp:88](../3rdpty/sources/IonCachyEngine/src/ice/manage/AudioTrack.cpp#L88) [3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp:224](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/StreamingDecoder.cpp#L224)

### P2 · R22 · ICE 的 AVX 要求泄漏到公开头和非 x86 构建

- [ ] 非 Apple/Android 的非 MSVC 构建无条件加 -mavx2。Linux x86_64 用 GCC 16 默认目标编译只 include 公共头并调用 operator+= 的独立消费者，实际报 target specific option mismatch。按架构选择实现，覆盖默认参数消费者和 Linux ARM64 配置。

[3rdpty/sources/IonCachyEngine/cmake/configure-project.cmake:9](../3rdpty/sources/IonCachyEngine/cmake/configure-project.cmake#L9) [3rdpty/sources/IonCachyEngine/include/ice/manage/AudioBuffer.hpp:462](../3rdpty/sources/IonCachyEngine/include/ice/manage/AudioBuffer.hpp#L462) [3rdpty/sources/IonCachyEngine/include/ice/manage/AudioBuffer.hpp:491](../3rdpty/sources/IonCachyEngine/include/ice/manage/AudioBuffer.hpp#L491)

### P3 · R23 · 设置页每帧复制整份元数据基线

- [ ] 含多个字符串和路径的 BaseMapMeta 在表单静置时也复制，仅提交时用到。按实际编辑保留原字段；用长元数据核对空闲帧分配和多字段合并。

[Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp:290](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp#L290) [Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp:1162](../Modules/Game/UI/src/ui/imgui/manager/SettingsView_BeatmapTab.cpp#L1162)

### P3 · R24 · 菜单声明传播完整逻辑命令头

- [ ] 头中只借用 LogicCommand，完整变体又引入配置和谱面定义。本机 Ninja 活动对象里 MenuUtil.h 为 28 个、LogicCommands.h 为 129 个。改用既有前置声明头并直接包含 MalodyMode 所需声明；以变更前后实际对象交集验收，28 并非保证能减少的重编数。

[Modules/Game/UI/include/ui/imgui/menu/utils/MenuUtil.h:3](../Modules/Game/UI/include/ui/imgui/menu/utils/MenuUtil.h#L3) [Modules/Game/UI/include/ui/imgui/menu/utils/MenuUtil.h:23](../Modules/Game/UI/include/ui/imgui/menu/utils/MenuUtil.h#L23) [Modules/Game/Common/include/common/LogicCommandFwd.h:9](../Modules/Game/Common/include/common/LogicCommandFwd.h#L9)

### P3 · R25 · 三份路径规范化均直接把 char 传给 ::tolower

- [ ] 遇非 ASCII 扩展名且 char 为有符号时，负值可能超出标准库允许的参数范围。提取一个按无符号字节处理 ASCII 的函数，覆盖高位字节。

[Modules/Game/Logic/src/logic/EditorEngine.cpp:803](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L803) [Modules/Game/Logic/src/logic/ProjectCommandService.cpp:1602](../Modules/Game/Logic/src/logic/ProjectCommandService.cpp#L1602) [Modules/Game/Logic/src/logic/ProjectResourceService.cpp:2090](../Modules/Game/Logic/src/logic/ProjectResourceService.cpp#L2090)

### P3 · R26 · 崩溃转储未写成功也提示已保存

- [ ] Windows 路径忽略创建或写转储失败，时间转换也未核实，用户可能被引导寻找不存在或不完整的转储。分别验证不可写目录、写出失败及成功文案。

[Modules/Game/include/game/CrashHandler.h:28](../Modules/Game/include/game/CrashHandler.h#L28) [Modules/Game/include/game/CrashHandler.h:50](../Modules/Game/include/game/CrashHandler.h#L50) [Modules/Game/include/game/CrashHandler.h:63](../Modules/Game/include/game/CrashHandler.h#L63)

### P3 · R27 · 逻辑循环线程归属的注释已过时

- [ ] start 的 Doxygen 声称向全局线程池提交常驻循环，而实现创建独立 std::thread。更正线程归属及停止时由谁 join，避免后续维护重新引入 PERF-01。

[Modules/Game/Logic/src/logic/EditorEngine.cpp:2204](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L2204) [Modules/Game/Logic/src/logic/EditorEngine.cpp:2232](../Modules/Game/Logic/src/logic/EditorEngine.cpp#L2232)

### P3 · R28 · 公开 Clipper 是不写输出的占位节点

- [ ] 可构造并接入效果链，却不写目标缓冲；复用目标可保留旧样本。主仓未定位到实际使用者。明确可用性或实现削波，以非零旧目标缓冲测试预期输出。

[3rdpty/sources/IonCachyEngine/include/ice/core/effect/Clipper.hpp:9](../3rdpty/sources/IonCachyEngine/include/ice/core/effect/Clipper.hpp#L9) [3rdpty/sources/IonCachyEngine/src/ice/core/effect/Clipper.cpp:10](../3rdpty/sources/IonCachyEngine/src/ice/core/effect/Clipper.cpp#L10) [3rdpty/sources/IonCachyEngine/src/ice/core/effect/IEffectNode.cpp:71](../3rdpty/sources/IonCachyEngine/src/ice/core/effect/IEffectNode.cpp#L71)

### P3 · R29 · 全量解码失败与空音轨无法区分

- [ ] 生产解码路径捕获 future 异常并永久缓存空结果，调用者失去故障原因。给资源准备结果保留失败状态，分别回归空文件、合法空轨、后台失败。

[3rdpty/sources/IonCachyEngine/src/ice/manage/dec/CachyDecoder.cpp:153](../3rdpty/sources/IonCachyEngine/src/ice/manage/dec/CachyDecoder.cpp#L153)

### P3 · R30 · 更新逐文件注释率清单

cloc 2.08 逐文件计数 1520 项，68 项低于 30%，其中 3 项原表未列。其余既有欠项参见本页的注释率逐文件待办。统计包含自维护构建和打包脚本、头模板、资源脚本与文档中的 Lua 示例，排除外部测试谱面附带的 Lua、预编译内容、生成的 SPIR-V 和空文件。翻译 Lua 仍沿用原清单口径。

- [ ] 按职责补充这些文件缺少的说明并逐文件复算。此次新增如下。

- `Modules/Audio/src/AudioTimelineMixerNode.cpp`，432 行注释、1012 行代码，29.92%。
- `Modules/Audio/tests/AudioOriginAlignmentServiceTest.cpp`，58 行注释、137 行代码，29.74%。
- `Modules/Game/Graphic/src/imguivk/VKOffScreenRenderer.cpp`，160 行注释、377 行代码，29.80%。

## 验收边界

原始清扫以静态分析为主。首批四项均已勾选。最终 Linux 主工程全量构建、9 项相关 CTest、独立 ICE 源码构建及 1 项 ICE CTest 全部通过。实际谱面的播放帧时间、GPU 等待及各目标平台运行结果需按对应待办验收。ICE 声道修复以独立源码构建验证，主项目默认配置使用仓库提供的预编译引擎，发布产物需由既定 Runner 另行生成。

### 首批 P1 修改文件的注释率

以下为本次修改后的逐文件数据，上面的全仓列表保留清扫时基线。

| 文件 | 注释行 | 代码行 | 比例 |
|---|---:|---:|---:|
| `3rdpty/sources/IonCachyEngine/include/ice/core/effect/Compresser.hpp` | 41 | 54 | 43.16% |
| `3rdpty/sources/IonCachyEngine/include/ice/core/effect/IEffectNode.hpp` | 47 | 39 | 54.65% |
| `3rdpty/sources/IonCachyEngine/src/ice/core/effect/Compresser.cpp` | 29 | 61 | 32.22% |
| `3rdpty/sources/IonCachyEngine/src/ice/core/effect/IEffectNode.cpp` | 20 | 45 | 30.77% |
| `3rdpty/sources/IonCachyEngine/tests/TimeStretcherRealtimeTest.cpp` | 552 | 1284 | 30.07% |
| `Modules/Audio/include/audio/AudioManager.h` | 493 | 372 | 56.99% |
| `Modules/Audio/src/AudioManager_BGM.cpp` | 291 | 678 | 30.03% |
| `Modules/Audio/src/AudioManager_SFX.cpp` | 330 | 769 | 30.03% |
| `Modules/Audio/tests/AudioManagerTimelineIntegrationTest.cpp` | 330 | 770 | 30.00% |
| `Modules/Game/Canvas/include/canvas/BackgroundVideoPlayer.h` | 37 | 41 | 47.44% |
| `Modules/Game/Canvas/include/canvas/Basic2DCanvas.h` | 203 | 250 | 44.81% |
| `Modules/Game/Canvas/src/BackgroundVideoPlayer.cpp` | 60 | 133 | 31.09% |
| `Modules/Game/Canvas/src/Basic2DCanvas.cpp` | 1275 | 2973 | 30.01% |
| `Modules/Game/Canvas/tests/MobileCanvasSelectionTest.cpp` | 291 | 677 | 30.06% |
| `Modules/Game/Logic/include/logic/EditorEngine.h` | 360 | 238 | 60.20% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 131 | 248 | 34.56% |
| `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1695 | 3948 | 30.04% |
| `Modules/Game/Logic/src/logic/EditorEngine_Impl.h` | 71 | 70 | 50.35% |
| `Modules/Game/Logic/src/logic/ProjectDirectoryWatcher.cpp` | 126 | 276 | 31.34% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 414 | 965 | 30.02% |
| `Modules/Game/Logic/src/logic/session/PlaybackController.cpp` | 345 | 805 | 30.00% |
| `Modules/Game/Logic/src/logic/session/SessionUtils.cpp` | 439 | 1023 | 30.03% |
| `Modules/Game/Logic/tests/BrushPaletteSessionStateTest.cpp` | 233 | 542 | 30.06% |
| `Modules/Game/UI/include/ui/utils/UIWidgetUtils.h` | 324 | 178 | 64.54% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp` | 1088 | 2537 | 30.01% |
| `Modules/Game/UI/src/ui/utils/UIWidgetUtils.cpp` | 699 | 1631 | 30.00% |
| `Modules/Game/UI/tests/MobileInputEditorTest.cpp` | 151 | 351 | 30.08% |
| `Modules/Network/Collaboration/src/CollaborationResourceSync.cpp` | 544 | 1268 | 30.02% |
| `Modules/Runtime/include/runtime/AppThreadPool.h` | 28 | 35 | 44.44% |
| `Modules/Runtime/src/AppThreadPool.cpp` | 45 | 100 | 31.03% |
