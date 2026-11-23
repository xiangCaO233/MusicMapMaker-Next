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

- [ ] **PERF-03 P2 按拍号显示时间时，每帧多次重建并排序完整 BPM 数组**

  - 热路径入口：`Modules/Game/UI/src/ui/imgui/MainDockSpaceUI_StatusBar.cpp:289-291` 每帧格式化当前/总时间，`:313-315` 悬停再格式化一次；`Modules/Game/Canvas/src/PreviewCanvas.cpp:531`、`Modules/Game/Canvas/src/TimelineCanvas.cpp:650`、音频分析视图 `AudioSpectrumView.cpp:861` / `AudioWaveformView.cpp:592` 也调用快照重载。
  - 调用链：`Modules/Game/UI/src/ui/utils/TimeFormatUtils.cpp:286-292` pair 重复调用单值函数；Beat preference 在 `:228-229` 转发 `formatBeatTime`，`:198` 每次 `collectBpmPoints`；`:55-76` 扫描全部 `scrollSegments`、分配 vector、完整 sort 和 unique。
  - 后果：仅状态栏每帧至少两次 O(B log B)，悬停再加一次；多窗口叠加后与 BPM 数量和帧率相关。代码 warning 声称不是每帧批量路径，实际调用链违反该假设。
  - 整改：复用已有 `CanvasTimeFormatContext` 重载（`:207-211`）；按会话/BPM 修订构建归一化节点与前缀累计拍数，普通帧仅二分并格式化。不要按每帧 RenderSnapshot 地址失效缓存，否则播放发布会持续重建。
  - 验收：Beat preference 下用大量 BPM 节点谱面同时打开状态栏、时间线、预览、频谱/波形；统计 collect/sort 次数，时间推进/悬停时应为零，BPM 修改后每修订重建一次；与原结果对照负时间、同时间 BPM、分拍边界。

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

- [ ] **PERF-05 P2 SessionContext 与 EditorEngine 的公开面跨越多职责，锁与生命周期契约暴露到 UI**

  - `Modules/Game/Logic/include/logic/session/context/SessionContext.h:95-226` 同时拥有 ECS 注册表、教学状态、草稿合并、播放/导航、项目与协作资源、配置、相机、同步缓冲、音频 transport；`:308-411` 又包括框选、画笔、橡皮擦、撤销与剪贴板。文件 414 行，当前 Modules 非测试源有 50 处直接包含此头。
  - `Modules/Game/Logic/include/logic/BeatmapSession.h:101-105` 公开完整 const 与 mutable 上下文引用，没有在线程/锁类型层面限制。
  - `Modules/Game/Logic/include/logic/EditorEngine.h:365` 对外暴露递归 mutex；`:86-88` 项目指针、`:282` 活动切换、`:542` 配置广播、`:567` 项目持久化，加上公开快照/资源/同步职责，形成全局单例门面（头 707 行、主实现 5894 行）。
  - 具体泄漏：`Modules/Game/Canvas/src/AnnotationTableData.cpp:45-55` UI 邻近层获取引擎 mutex 后直读 Context；批注版本未变也先抢锁。`Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp:2698-2718` 通过 try_lock 避免等待，但仍知道 SessionEntry 和谱面模型细节。
  - 这属于维护性和通用性风险，不把文件大本身判为运行错误。后果是新 UI 查询容易再次引入第 4 项长锁等待，独立测试必须搭建全局引擎，教学等场景不断扩张核心上下文。
  - 整改：以现有发布快照/命令队列为边界，逐职责提取只读 presentation DTO 与窄服务；优先 interaction-state、批注读模型、transport lifecycle，再收紧 mutable Context 与 mutex 访问。不为缩短头文件而把轻量值随意堆分配，也不一次性重写所有 Controller。
  - 验收：每批提取后原行为测试通过；UI/Canvas 相关文件不再包含 SessionContext 或获取 session mutex；使用 Ninja 依赖图量化头文件翻译单元扇出变化；分别测试活动会话关闭、切换与快照保活。

- [ ] **PERF-06 P3 离屏几何容量扩张在命令录制阶段等待全 GPU 空闲**

  - `Modules/Game/Graphic/src/imguivk/VKOffScreenRenderer.cpp:134-147` 当前帧顶点/索引超过历史容量即调用 `device.waitIdle()`；`:154-160` 回收全部帧槽后重新分配，按 1.5 倍增长。
  - 这是有注释、低频高水位分支，不能直接据此断言日常播放性能差；它也不是 acquire/present 必需同步点。首次复杂视野、突然增多的选择/特效/窗口可触发整设备停顿，且可从第 1 项并行录制任务内执行。
  - 整改：先为 record/扩容埋点与真实场景取样；若尖峰明显，提前预估容量或按帧槽 fence 回收旧缓冲，不在录制回调释放全部飞行资源。不得取消同步后直接销毁 GPU 正在使用的 buffer。
  - 验收：相同谱面、相同构建/用户配置的同一播放区间，分别测 CPU、锁等待、GPU busy、Fence/acquire/present 和扩容计数；确认常态帧无扩容等待，尖峰与具体容量变化关联，再判断是否值得改造。

## 平台适配

- [ ] **PLAT-01（P2）启动区域设置使用 Windows 名称且忽略失败**

  - 位置：`Modules/Game/include/game/GlobDefs.h:79-80`。
  - 行为：全局 `RTTILogger` 构造无条件调用 `std::setlocale(LC_ALL, ".UTF-8")`，返回值被丢弃，注释却声称 C/C++ 路径与格式化已统一 UTF-8。
  - 证据：当前 Linux 隔离 Python/ctypes 进程调用相同 C API，返回 `None`，调用前后 locale 完全相同。因此 Linux 实际继承原 locale，注释保证不成立。这里尚不能推导所有中文路径都失败，因为已有 UTF-8/path 包装可能自行保证语义。
  - 整改：把 locale 初始化收入启动平台实现，定义失败回退和日志，明确只初始化真正需要的 category；不要依赖一个平台名称覆盖所有系统。
  - 验收：Windows、Linux（含 `LC_ALL=C`）、macOS 上检查成功/失败返回与最终 category，中文路径和数值解析不依赖宿主 locale；失败有明确诊断。

- [ ] **PLAT-02（P2）文本协议浮点语法随平台变化**

  - 位置：`Modules/Game/Logic/src/logic/EditorClipboardProtocol.cpp:240-249`；相关实现 `Modules/MMM/include/mmm/SafeParse.h:40-71`。
  - 行为：剪贴板协议 `parseDoubleField` 只检查完整消费与有限值，Apple 分支由 `strtod_l` 解析，其余平台由默认格式 `from_chars` 解析。输入 `0x1p2` 的十六进制语法在 Apple 可被完整消费，默认 `from_chars` 不会按该语法完整消费；于是同一外部协议文本在平台间的合法性不同。
  - 同类：`Modules/Game/Logic/src/logic/BmsPackageExportService.cpp:198-202` 对 MYO2 cue shift 使用同样的完整消费检查；`Modules/Game/UI/src/ui/imgui/SideBarUI.cpp:43-46` 允许前缀解析，对相同文本还可能得到不同数值/回退结果。后两项须先确认各字段的既有输入契约，不能机械一刀切。
  - 可复用先例：`Modules/MMM/src/beatmap/BmsFormat.hpp:69-80` 已用十进制字符白名单隔离平台扩展语法。
  - 整改：给协议明确十进制语法并在该边界验证，或定义统一 helper 语法；不修改兼容性解析器的所有调用方。
  - 验收：同一组正负号、科学计数、十六进制、NaN/Inf、尾随垃圾和溢出输入，在 Apple 低部署目标与 Linux/Windows 上得到相同协议结果；保留合法历史剪贴板往返。

- [ ] **PLAT-03（P2）三个 UI 时间格式化分支遗漏失败判断**

  - 位置：`Modules/Game/UI/src/ui/imgui/manager/AudioManagerView.cpp:301-329`、`BeatMapManagerView.cpp:202-227`、`FileManagerView_Hierarchy.cpp:273-298`。
  - 行为：三个匿名 `formatModifiedTime` 重复文件时钟近似转换和 `_WIN32` 分支。`localtime_s`/`localtime_r` 返回值没有检查，失败后仍把零初始化 `tm` 交给 `strftime`，它可能成功产生一个看似有效的日期，绕过“未知”回退。
  - 整改：提取 UI 时间显示公共 helper，集中平台签名差异，时间转换失败直接返回未知。是否换成 `file_clock::to_sys` 要按各 Runner 标准库支持度决定，不能仅凭本机 C++20 标记推断。
  - 验收：正常/负纪元/不可表示时间、时区和夏令时边界统一；三处 UI 仍使用原始文件时刻排序；失败显示未知且不抛异常。

- [ ] **PLAT-04（P2）Windows 命令行解析失败后可能读取未初始化数量**

  - 位置：`Modules/Game/src/GameLoop.cpp:274-292`。
  - 行为：`int argcW;` 没初始化；取得 `CommandLineToArgvW` 结果后直接判断 `argcW > 1` 并访问 `argvW[1]`。若 API 分配失败返回空，代码没有任何失败分支。
  - 整改：数量初始化为零，先检查返回指针，再处理第二参数；把“平台命令行到路径”收敛到启动适配器，保留事件发布在通用 GameLoop。
  - 验收：正常中文/空格/长路径参数保持一致；API 失败能够继续启动且不发布打开事件；分配结果只释放一次。

- [ ] **PLAT-05（P3）启动头重复平台可执行路径实现，边界与长路径行为已分叉**

  - 位置：`Modules/Game/include/game/GlobDefs.h:84-116` 对照 `Modules/Config/src/AppPaths.cpp:63-111`。
  - 行为：全局头直接引入 Windows/unistd/dyld 并查询进程路径，Windows 固定 `MAX_PATH`；AppPaths 已经独立实现动态扩容，macOS 还解析 canonical 路径。启动更改 cwd 和配置路径查询因而有两套不同成功边界。
  - 整改：复用低层可执行路径服务，把平台 include 与操作移出公开业务头；不要在头中继续复制第三套实现。需注意日志初始化前不能反向依赖日志与业务单例。
  - 验收：Windows 超 MAX_PATH 的可执行路径、macOS 符号链接启动、Linux procfs 失败均有明确一致结果；日志初始化顺序与资源相对路径行为保持兼容。

- [ ] **PLAT-06（P3）窗口平台状态策略仍直接驻留通用渲染文件**

  - 位置：`Modules/Game/Graphic/src/imguivk/VKRendererRender.cpp:526-612`、`:623-665`；`Modules/Game/Graphic/src/glfw/NativeWindow.cpp:760-805`、`:1700-1725`、`:1775-1815`。
  - 行为：Vulkan renderer 同时承担 HWND property、ShowWindow/SetWindowPos/SetForegroundWindow；`MMMRestoreMaximizedAfterMinimize` 常量分别出现在 renderer、NativeWindow（:274）和 Win32WindowAdapter（:31）。平台适配器已经存在，但最大化恢复协议仍跨三处维护。
  - 整改：将窗口组提升、最大化恢复意图、startup frame shape 操作逐项收入现有平台适配器；renderer 只调用抽象窗口动作。保留现有低频激活边沿机制，不把系统调用移入逐帧无条件路径。
  - 验收：Windows 任务栏/系统菜单最小化与恢复、浮动 ImGui 工具窗不抢焦点；macOS 模拟最大化；X11/Wayland 的既有行为分别验证。此项是职责治理建议，未发现仅靠宏本身即可证明的运行故障。

- [ ] **PLAT-07（P3）致命提示接口的平台实现不完整**

  - 位置：`Modules/Game/Common/include/common/MessageBox.h:21-40`；唯一已定位调用 `Modules/Game/Graphic/src/imguivk/context/VKContextDebugFeats.cpp:82`。
  - 行为：Windows 使用 `MessageBoxA`；所有非 Windows 系统都尝试 Linux 桌面命令 zenity/kdialog/notify-send，忽略全部失败。当前调用为固定可信英文文本，因此不能据此宣称存在当前可触发的命令注入，也不需要扩大成安全审计；macOS 未提供独立原生提示实现。
  - 整改：移动到平台源文件；Windows 采用明确 UTF-8 到宽字符转换，macOS 提供本机提示或可靠日志/退出降级；外部命令如需保留用参数数组避免 shell 文本协议。
  - 验收：未能初始化图形界面时三桌面平台均可见错误或有可靠诊断；引号、换行和中文正确；保持只用于低频致命路径。

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

- [ ] **FMT-02（P2）Malody 导入保留整文本与 JSON DOM，没有文件/对象入口预算**

  - 位置：`Modules/MMM/src/beatmap/LoadMalodyMap.hpp:359-370`。
  - 调用链：`BeatMap::loadFromFile(.mc)` → `loadMalodyMap` → `istreambuf_iterator` 完整复制到 `fileContent` → 完整 `json::parse` → 再生成领域物件和 timing。原文本和 DOM 都活到函数返回。
  - 当前行为：没有先判断输入大小，也未看到 parser 层深度/节点预算。合法但很大的文件会同时保留文本、DOM、转换中间结构和最终模型。`allow_exceptions=false` 只处理语法错误，并不能替代内存预算。
  - 整改：优先给导入入口明确可配置或产品约定的字节/节点/深度上限，读取失败需检查；解析完后尽早释放不用的原文本。只有真实负载证明需要时再设计 SAX/分段转换，避免为了“泛化所有 JSON”重写格式语义。
  - 验收：大文件、极深 JSON、超多空节点和正常大谱面均有明确结果；拒绝发生于大规模模型构造前；记录峰值 RSS，既有 Malody 扩展字段往返保留。

- [ ] **FMT-03（P2）资产 SHA-256 用两份整文件内存，且不支持校验中取消**

  - 位置：`Modules/Network/src/AssetSyncService.cpp:1828-1833`、`:430-445`。
  - 调用链：本地资源匹配（:896）、完整包下载验证（:1015）、增量资源下载验证（:1303）→ `sha256File` → `readFileBytes`（:360-379）→ `sha256Bytes`。
  - 当前行为：先按文件总长 `resize` 读入 bytes，再把 input 完整复制到 data 以追加 SHA padding。峰值至少含两份完整内容，追加填充还可能触发 vector 扩容；`readFileBytes` 没有应用级预算且整数转换未先检查可表示范围。外层在校验前后可取消，哈希内部没有取消点。
  - 整改：流式固定块 SHA-256，统一错误和可选取消回调；内存数据入口也不应复制整份输入只为末尾填充。协作资源加密已有分块 `mbedtls_sha256_update` 先例（`CollaborationResourceCipher.cpp:462-511`），可研究共享底层摘要能力，但不要把加密协议耦合到资产清单。
  - 验收：空文件、边界块长和已知向量摘要一致；多 GiB 文件内存近似常量；读取中断/错误不产生可接受摘要；取消在块边界响应。

- [ ] **FMT-04（P2）资产 ZIP 同时持有完整压缩包与完整解压条目，缺少展开预算**

  - 位置：`Modules/Network/src/AssetSyncService.cpp:1724-1734`、`:1795-1808`。
  - 调用链：下载包 SHA 校验后（:1013-1039）或本地包安装（:1112）→ `extractZipArchive` → 完整 ZIP `readFileBytes` → `mz_zip_reader_init_mem` → 每条 `mz_zip_reader_extract_to_heap`。
  - 当前行为：内存为压缩包大小加当前解压条目大小，单个大字体/图像/音频或高压缩比条目可在一次解压中占用大块内存。循环未依据 `fileStat` 检查单条/累计解压大小或条目数量预算；取消只在条目之间检查，单条大文件不可中断。已有路径安全检查不能替代资源规模限制。
  - 整改：文件 reader 加回调/文件流式解压，明确单条、总展开量和文件数预算；输出分块时检查取消，失败维持同步版本标记不提前发布。
  - 验收：合法大条目峰值内存有界；超预算压缩包在写满磁盘或大分配前拒绝；单条内取消可响应；目录穿越等既有路径约束保持。这里只证明资源预算问题，不推断其下载渠道已被攻击。

- [ ] **FMT-05（P3）离线音频时间线在无 DSP 情况下也强制所有资源完整驻留**

  - 位置：`Modules/Audio/src/AudioTimelineExportService.cpp:158-190`、`:194-245`；`AudioTimelineResourceProcessor.cpp:318-347`。
  - 调用链：IMD 包导出（`Modules/Game/Logic/src/logic/ImdPackageExportService.cpp:405`）或首拍原点对齐（`AudioOriginAlignmentService.cpp:61-62`）→ `exportMixedAudio` → 每个不同文件创建 `CachingStrategy::CACHY` 音轨 → 每个不同处理配置产生 Prepared PCM → 混音编码。
  - 当前行为：全部原 PCM 保存在 tracksByPath，全部非中性处理 PCM 又保存在 preparedByConfig；DSP 输出还保留原 track 所有权（processor :347）。中性配置确实复用原缓存，不应误报为必然复制两份，但原点对齐这种单资源原速/无 EQ 操作仍全量解码驻留。多个配置以及 0.25 倍速度可能显著放大内存。
  - 整改：先定义离线总 PCM 预算与失败信息；对不需要 EQ/stretch 的路径探索流式资源（`AudioTimelineMixerNode.cpp:159-164` 已有 STREAMING 包装），处理资源可考虑有界磁盘暂存或分阶段释放，不能破坏实时混音缓存生命周期。
  - 验收：分别测中性、EQ、变速、多资源多配置的峰值 RSS 和输出 PCM 正确性；首拍裁头/补静音长度保持；避免只是把完整缓存从一个 map 移到另一个对象。

- [ ] **FMT-06（P3）协作文档限额检查发生在完整 CBOR 编码之后，并无条件先复制正文**

  - 位置：`Modules/Network/Collaboration/src/BeatmapDocumentCodec.cpp:140-181`，快照 :1599-1612。
  - 调用链：`CollaborationRoom.cpp:880` 本地 mutation 编码 → `BeatmapDocumentCodec::encode` → `makeDocument`/`makeMutationDocument` → `encodeDocumentPayload`。快照还复制 current 为 payload，并在成功后保留 baseline。
  - 当前行为：`Json::to_cbor(document)` 已创建完整 raw，随后才比较 64 MiB 上限；`body = raw` 又复制一份，压缩 candidate 与最终 output 可再同时存在。接收端在分配前检查 rawSize 的保护是真实有效的，不能把这里说成“没有任何协议限额”；缺口在本地发送侧的限额时机与峰值复制。
  - 整改：编码前做对象/字段预算或使用可在达到限额时停止的输出 sink；避免未压缩 body 无条件复制，未压缩时直接从 raw 构造输出。先处理局部复制，再决定是否值得重构整个编码器。
  - 验收：超限本地文档在超过预算前终止编码；压缩/不压缩快照、增量、基线成功提交与 EmptyPayload 语义不变；记录临界 64 MiB 附近峰值 RSS。

## 音频引擎

- [x] **ICE-01 · P1 · 重新准备同采样率、不同声道数时同步压缩器状态。** `3rdpty/sources/IonCachyEngine/src/ice/core/effect/IEffectNode.cpp:19` 的 prepare 更新输入缓冲；`Compresser.cpp:33、47、70` 仅在脏标记或采样率改变时调整 envelope_db，却按新输出声道逐个索引。增加声道时可超过旧状态范围。把声道状态尺寸纳入控制侧准备契约。验收覆盖重新 prepare、增减声道、相同块长和保留旧声道状态的约定。独立 ICE 源码全量构建及 IonTimeStretcherRealtimeTest 已通过，新增基类接口声道切换、保留已有声道包络及回调零分配回归。
- [x] **ICE-02 · P2 · 消除压缩器逐样本读取共享参数。** 同文件第 43、92 行计算了补偿倍率缓存，样本循环仍反复读取原子 dB 值。头文件 `Compresser.hpp:22–47、96` 的 setter 又写普通布尔脏位。先确定是否允许播放中调参；若允许，在块边界取得一致参数，控制侧完成可预计算的部分。若要求停机修改，收紧 API 并检查调用方。验收固定参数 PCM、切参边界、回调内分配次数和耗时，不将原子读取一律改成裸读。 独立 ICE 源码构建与 IonTimeStretcherRealtimeTest 通过。setter 仅在接入音频图前或停止后调用；演示程序与测试调用符合此约束。处理块读取一次补偿并复用压缩系数。新增双声道停机调参用例，逐样本验证 6 dB 补偿及回调零分配。Compresser.hpp 注释/代码为 31/46 行（40.26%），Compresser.cpp 为 26/59 行（30.59%），TimeStretcherRealtimeTest.cpp 为 566/1319 行（30.03%）。
- [ ] **ICE-03 · P2 · 让 PitchAlter 遵守准备容量。** `3rdpty/sources/IonCachyEngine/src/ice/core/effect/PitchAlter.cpp:46` 的 process 每块调用 resize，绕过 `IEffectNode.cpp:47` 的预备容量检查。核对支持的最大块长和格式切换协议，稳定路径只切有效长度；分别测同尺寸、短块、超限块和重新准备后的分配计数。调用 resize 不等于每块均发生分配。

## 编辑器、临时实现与标准库

- [x] **UI-01 · P1 · 超长元数据进入编辑器时保全原文。** `Modules/Game/UI/src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp:273、1000、1068、1186` 以 N−1 字节截断 OSU 文本和 JSON，未把截断状态带到提交处。若 OSU 尾部恰好截在一条完整记录后，剩余文本仍可通过解析并生成不完整的属性表。改为按编辑内容扩容，或明确拒绝载入并保全原文；覆盖 32/64 KiB 边界、UTF-8 多字节边界、取消和原样确认。已改为完整字符串缓冲。UI 套件重新链接，MobileInputEditorTest 的两种 UTF-8 边界和 BpmBeatmapImportTest 均通过。
- [x] **UI-02 · P2 · 仅在编辑发生时解析字段 JSON。** 同文件第 1316、1373 行，弹窗打开且可见时，未编辑的一帧也解析同一个缓冲两次。用文本修订标记让编辑、格式化、重建和子字段操作都使缓存失效；完成按钮必须看到最后一次输入。验收静置时解析次数为零，每次修改后的错误提示与提交值一致。 已用弹窗独占文档缓存解析值和诊断，静止帧只读缓存，文字变化时解析一次，格式化与结构编辑复用已解析的 JSON。Linux RelWithDebInfo 全量构建通过，MobileInputEditorTest、BpmBeatmapImportTest、TimelineBatchMetadataTest 通过，覆盖空闲读取、无效输入、重建、字段增删、非对象及确认前的最后一次编辑。MetadataEditorWindowRenderers.cpp 注释/代码为 1083/2526 行（30.01%），MetadataJsonDocument.h 为 22/50 行（30.56%），MobileInputEditorTest.cpp 为 166/386 行（30.07%）。
- [ ] **UI-03 · P3 · 明确全局搜索面板的产品状态。** `Modules/Game/UI/src/ui/imgui/manager/SearchView.cpp:51–62` 输入事件处理为空，所有查询均显示无结果。接入索引时区分未搜索、搜索中和无结果，覆盖过期结果丢弃及跳转；若功能暂缓，应在入口说明可用性。项目配置中的未来扩展备注未发现当前行为缺口。
- [ ] **STD-01 · P2 · 梳理四个原子发布位置的所有权。** 12 处共享指针 atomic 自由函数调用位于 `Modules/Game/Logic/src/logic/SessionRegistry.cpp:266`（含两种快照）、`RenderSyncRegistry.cpp:215`、`session/BeatmapSession.cpp:578` 和 `session/BeatmapSession_Commands.cpp:2598`。分别登记写者、读者、持有时长与最后释放线程；会话/图集读者优先每轮取得一次句柄，内部传引用。观察者回调后第 2613 行重新获取用于确认身份，不能拿回调前缓存替代。验收旧快照跨关闭仍可用、图集替换、观察者回调中替换及目标工具链弃用告警；仅替换为 atomic<shared_ptr> 仍有所有权和潜在内部同步成本。
- [x] **STD-02 · P2 · 导出路径检查显式处理文件系统错误。** `Modules/Game/UI/src/ui/imgui/menu/actions/file/SaveBeatmapAsAction.cpp:213` 使用可能抛出的 exists(path)。改用 error_code 重载，访问失败保留待导出内容并报告原因；分别验证路径不存在、已有文件及父目录无访问权限。 已使用 error_code 重载，查询失败走统一导出失败反馈并中止当次导出，现有目标仍进入覆盖确认。Linux 全量构建、SaveResultFeedbackTest 和 BpmBeatmapImportTest 通过；同编译器文件系统探针验证缺失、已存在及父目录权限拒绝的三种结果。SaveBeatmapAsAction.cpp 注释 246 行、代码 570 行，30.15%。
- [ ] **STD-03 · P3 · 明确分配诊断与禁异常规范的边界。** `3rdpty/sources/IonCachyEngine/src/diagnostics/AllocationTracker.cpp:25–32、86` 在测试诊断替换的普通 operator new 中抛 bad_alloc，并使用 cout 输出。核对构建隔离并明确诊断目标例外，或迁移到可表达失败的专用测试钩子；不能简单让普通抛出型 new 在失败时返回空指针。验收生产库未链接替换符号，计数测试仍能观测分配。

## 头文件与注释

- [ ] **HDR-01 · P2 · 从只借用配置的接口切断聚合头。** `Modules/Config/include/config/EditorConfig.h:3、69` 同时定义按值容器和引用视图；内联重置方法又将默认值构造暴露给所有消费者。Ninja 记录 EditorConfig 191 个对象、EditorSettings 271 个、BeatLinePalette 和 NotePalette 各 278 个。优先分离引用视图并外移依赖完整定义的实现，保留按值成员；相同构建配置比较实际对象数和增量耗时。
- [ ] **HDR-02 · P3 · 调色文件接口独立于完整编辑设置和 JSON 实现。** `Modules/Config/include/config/ColorPaletteFile.h:3、34` 仅为一个按值方案引入 EditorSettings，并内联 JSON 转换。提取方案类型及 ADL 声明，转换移入 cpp；该文件本身仅传播到 3 个已记录对象，收益单独计量。验收合法/非法配色导入与现有文件往返。
- [ ] **HDR-03 · P2 · 用真实消费者验收会话拆分。** SessionContext 对应 73 个对象、EditorEngine 对应 96 个；PERF-05 列出 UI 直接取锁访问的实例。先迁移批注读模型或交互状态，再对照活动对象清单；降低直接 include 个数不足以证明扇出下降。
- [ ] **DOC-01 · P2 · 修订失真的契约说明。** `Modules/MMM/include/mmm/SafeParse.h:141` 的旧 stod 兼容描述需写明前导加号、消费长度和平台语法；`Modules/Game/include/game/GlobDefs.h:79` 的 UTF-8 声明需以初始化结果为准；对照 PERF-03 更正时间格式化调用频率。每条用相应输入或调用链验收。
- [ ] **DOC-02 · P3 · 让函数体注释解释选择依据。** `Modules/Game/Graphic/src/imguivk/context/VKContextDebugFeats.cpp:15–33` 中多条注释逐词复述 eError、eWarning、eGeneral 等枚举。保留需要解释的筛选理由、回调生存期和不可重入约束；无需为显然枚举逐项写释义。修订后逐文件检查 cloc 和函数体关键实现。
- [ ] **STACK-01 · P3 · 补齐不定长栈分配的编译器证据。** 1,242 文件文本扫描未命中显式 alloca、_alloca、_malloca 或 __builtin_alloca 调用；主仓常见声明模式未定位 VLA。模板、别名和宏展开仍需以各平台编译器诊断确认。用支持的 GCC/Clang 配置开启 VLA 诊断并检查平台分支；对 `ProjectController.cpp:815`、`BeatmapSession_Commands.cpp:880` 等固定 64 KiB 局部缓冲用栈使用报告核对同一调用链峰值和工作线程栈预算。固定数组、对象成员与输入决定长度的栈分配要分别判断。

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
