# 移动端触屏交互阶段交接

更新时间：2026-11-12。当前分支 `feat/android-sdl3`，本批接续 `23e815fb`，完成移动端折线绘制与续写，本批为独立提交。

## 本批行为

- 绘制工具下，移动工作台增加“单键／长条”和“折线”两列模式按钮，分别提供中英文短译文及图标。默认简单绘制；折线模式受既有折线编辑开关约束，禁用原因直接显示正文。
- 折线模式从空白拖动，按纵向／横向转向形成连接段；从单键、长条尾、滑键箭头或已有折线最后一段的节点或尾端拖动可续写。轻点已有物件只选择，轻点空白仍创建独立单键；长条身体起笔不截短原长条。
- 跨过触控容差后，同帧依次回放原点起笔与实际位移，防止首个拐角丢失。补交只处理指针，不重复执行快捷键；松手帧也保留转向后的真实长条长度，不依赖下一帧才能完成末段。
- 事务续写先保留正式原根和所有子实体，只在局部渲染候选中隐藏原根，由画笔展示临时几何。释放后一次删除旧对象并创建结果，一次撤销／重做覆盖完整续写；根注释、额外元数据和颜色继续保留。
- 第二指或系统取消直接丢弃未执行的删除快照，不调用普通 Undo，不清除原有重做分支。预览期间发生其他内容修订则拒绝过期提交。
- 播放／定位、只读切换、能力失效和权威替换时同步清理待提交快照。权威同步暂存普通画笔改用移动，独占动作不通过共享所有权复制。
- 教学续写的复合动作保留新产物的定向补偿能力；普通撤销恢复原物件，教学返回只移除该步骤的延伸结果。
- 经典桌面和纯鼠标继续使用原画笔、修饰键与两阶段续写历史。新命令意图默认关闭；移动实现与私有头保持隔离，公共层不包含移动布局头，也未增加平台条件宏。

## 验证

- Windows 完整构建通过；8 项相关 CTest 全部通过，完整 CTest **196/196 通过**，耗时 231.89 秒。
- 生产输入回归覆盖空白新画、四类来源续写、最后一段、第二指取消、非空重做保留、过期提交、一次撤销／重做及教程返回。夹具预热坐标与每笔起点一致，避免 ImGui 队列分帧使不同来源漏掉首个采样。
- Android x86_64 原生构建、Gradle 打包通过，已安装手机和平板模拟器。中文／英文的两列模式按钮、图标和文字完整显示；手机英文短标题允许两行，不截断。
- 两个模拟器均完成三段折线新建、末端续写为五段，逐段保存回读确认原三段前缀时间、轨道、长度精确不变，五个原始物件完整保留。
- 两个模拟器均完成“一次撤销恢复原折线 → 轻点末端 → 第二指取消续写 → 重做恢复完整五段”的数据回读，证明轻点和取消均不清空已有重做分支。
- 两个模拟器进一步通过真实拾取的单键、长条尾、滑键箭头续写；各自一次撤销完整恢复来源，简单／折线模式切换和简单长条创建正常。
- 最终保存内容恢复为原五个物件加已验收的五段折线，两个 MMM 进程保持运行，当前进程 AndroidRuntime／libc 日志无致命异常。模拟器与应用均保持开启。
- 本批证据前缀为 `build-android/evidence/touch-polyline-*`，包含构建与测试日志、注释 CSV、触摸注入脚本、截图及逐步保存回读。辅助工具在已忽略的 `build-android/tools`。

## 已完成基础与实机边界

- `23e815fb`：触控点按单键、固定轨道双向拖动长条；简单笔画不续接、不删除或合并已有物件，第二指取消不制造空历史。
- `df76335b`：触控选择的替换／追加／减选及清空；点选、分多次框选共用集合语义，桌面与纯鼠标状态独立。
- `df7fd397`：容差内轻点只改变选择，不建立空拖动历史，不清除重做；`e616b050` 修复重复试听测试的时序干扰，生产音频逻辑不改。
- NX809J `9125282100E7` 前轮使用 `df76335b` 的独立包 `.next.drawprobe`（MMM Touch Regression）完成选择回归。原 `.next.dev` 签名与本机不同，既有 `.next.dev`、`.next.touchtest` 的安装和数据没有改写。本批折线只要求两个模拟器，不把旧包实机结果当成新功能验收。
- 原始多触点、稳定手指身份、双指导航与捏合沿用已有实现。第二指接管后须全部抬起，才能重新单指编辑；取消不等于撤销已完成编辑。

## 已知边界与后续

- NX809J 第三指落下时系统会发送 Cancel；不能将系统 Cancel 当作重做。该设备继续使用长按菜单重做，支持完整事件流的设备保留二／三指分类。
- 前轮 Linux 曾有 `CanvasTouchNavigationTest` 最终缩放采纳被空闲节流跳过的问题，本批未修复，Windows 通过不代表跨平台已解决。
- 很短的 `adb input swipe` 可能没有完整末端 Move；验收使用明确的终点 Move → Up。ADB 注入验证事件与数据，不代替真实手指手感验收。
- 本批完成折线后再继续移动 Timing，最后完善属性与辅助面板；当前不能把基础选择、简单绘制和折线等同于全套移动编辑完成。

## 构建与设备

在项目根目录执行 Windows `cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`；安卓 `scripts/android/build-windows.ps1 -Abi x86_64 -Jobs 2`。同时运行两个模拟器时，不并发原生构建，也不把完整 CTest／重音频测试与原生编译叠加。

手机 `emulator-5554`／`mmm-api36-minimal`：1280×720、160dpi、50% 时间线缩放、英文、放置磁吸开启。平板 `emulator-5556`／`mmm-api36-tablet`：2560×1600、320dpi、100% 时间线缩放、中文、放置磁吸关闭。测试后保持两个模拟器和 MMM 开启。

APK 位于已忽略的 `build-android/apk`，默认文件会随 ABI 覆盖，安装前核对 ABI 与签名，禁止靠卸载用户原应用解决签名差异。测试工程为模拟器私有 `files/acceptance-project` 副本，五个原始物件和原媒体不属于用户工程。

一项逻辑修改独立提交；提交或换机前用 `git status` 核对状态，不从旧 APK 推断当前源码。本批未授权推送或公开发布。

## 本批逐文件注释检查

仅检查本批 18 个修改的 C++ 文件，按 cloc 的 `comment / (comment + code)` 逐文件计算，全部达到 30%；未重新扫描全仓基线。两份翻译 Lua 是纯资源表，文档与忽略的验收输出不计入代码统计。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2016 | 4704 | 30.00% |
| `Modules/Game/Logic/src/logic/session/ActionController_Editing.cpp` | 1405 | 3270 | 30.05% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1317 | 3060 | 30.09% |
| `Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp` | 998 | 2301 | 30.25% |
| `Modules/Game/Logic/src/logic/session/tool/DrawTool.cpp` | 704 | 1637 | 30.07% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp` | 650 | 1516 | 30.01% |
| `Modules/Game/Logic/src/logic/session/PlaybackController.cpp` | 312 | 725 | 30.09% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 406 | 566 | 41.77% |
| `Modules/Game/Canvas/tests/MobileCanvasDrawingTest.cpp` | 228 | 517 | 30.60% |
| `Modules/Game/UI/src/ui/interaction/EditorInteraction.cpp` | 234 | 492 | 32.23% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 203 | 467 | 30.30% |
| `Modules/Game/Common/include/common/render/RenderSnapshot.h` | 172 | 368 | 31.85% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkbench.cpp` | 148 | 343 | 30.14% |
| `Modules/Game/Canvas/include/canvas/Basic2DCanvasInteraction.h` | 244 | 277 | 46.83% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 115 | 233 | 33.05% |
| `Modules/Game/UI/include/ui/interaction/EditorInteraction.h` | 150 | 179 | 45.59% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.cpp` | 41 | 89 | 31.54% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 37 | 64 | 36.63% |
