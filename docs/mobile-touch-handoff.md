# 移动端触屏交互阶段交接

更新时间：2026-11-12。分支 `feat/android-sdl3`，绘制适配接续 `df76335b`。先完成 NX809J 的选择交互回归，再实现主画布点按单键／拖动长条。

## 当前绘制批次

- 触控主画布在起笔命令中提供轻量 `m_noteHoldOnly` 意图；逻辑工具锁存到本笔状态，固定初始轨道，只创建 Note 或 Hold。默认关闭，经典桌面和纯鼠标的原画笔不变。
- 拖动先经过既有触控容差，之后连续更新本地预览；快速移动并抬手仍补交最终 Update → End。长条支持双向时间拖动，提交时按起点与终点的最小时间及正时长归一化。
- 简单笔画不续接、删除或合并已有物件；第二指或系统接管通过正式取消入口丢弃未提交笔画，不用普通 Undo。BGM 自动采样继续沿用原逻辑。
- 绘制工具静止按住不会打开选择菜单；选择工具的长按菜单继续保留。私有手势实现仍位于移动目录，公共接口只增加中立语义，不引入平台宏或移动私有类型。
- 新增 `MobileCanvasDrawingTest`，通过生产触点解释器取得命令，再由真实 `InteractionController` 回放，检查几何、已有对象保护和一笔一次撤销。
- 教程的无修饰键提示与条件校验同步识别简单绘制策略；最终物件和路线仍由既有几何验收判断，经典桌面的 Shift／Ctrl 约束不变。
- Windows 最终完整构建及六项相关回归通过，完整 CTest **196/196 通过**；Android x86_64 原生构建与打包成功，已安装至手机和平板模拟器。
- 手机 1280×720／160dpi／50% 缩放／英文／物件磁吸开启，平板 2560×1600／320dpi／100% 缩放／中文／物件磁吸关闭，均通过点按、抖动轻点、静止长按单键、正反向长条、横向漂移不换轨、明确末端 Move 的 40ms 快速拖动、第二指取消。每一步保存回读均核对所有已有物件完全不变。
- 正反向笔画回读的时间范围一致、时长为正；手机得到 250ms 起点／250ms 时长，平板在无磁吸下得到约 250.2858ms 起点／250ms 时长。没有滑键或折线子段。
- 取消后一次撤销只移除最后有效的快速笔画，一次重做恢复完整音符数组，证明取消不产生空历史节点。最终模拟器留下已保存的绘制样例和绘制工具，供继续查看；有意重叠的快速笔画另存于验收证据中。
- 证据：`build-android/evidence/touch-drawing-*`，包括构建日志、CTest 日志、逐文件注释 CSV、触摸脚本、截图与各步谱面回读。实体机本批只回归已完成的选择交互，新绘制在两个模拟器验收。

## 实机选择回归

NX809J `9125282100E7` 使用 `df76335b` 的 arm64 构建，经 ADB 触摸注入验证，不等同于真实手指手感验收。

- “移动保存 → 撤销保存 → 点选 → 重做保存”完整回读通过；点选后页签没有未保存星号，重做恢复原来的完整音符数组。
- 追加点选与框选、点按减选、空白点按保留集合后成组移动，仅剩余 note01、note03 同步增加 250ms，其他属性及长条长度未变。再用减选框排除 note01，后续只有 note03 移动。
- 原 `.next.dev` 签名与本机不同，使用新身份 `.next.drawprobe`、名称 `MMM Touch Regression` 安装独立回归包。原 `.next.dev` 与 `.next.touchtest` 的安装、配置和工程均未改写。
- 每次注入前核对前台包名；发现用户切换应用后立即暂停，获得可用时段后继续。实机注入已结束，后续构建与绘制验收使用模拟器。
- 证据：`build-android/evidence/touch-selection-physical-result.txt`、同前缀保存回读和截图；独立回归 APK 为 `build-android/apk/touch-selection-physical-probe.apk`。

## 本轮修复与进度

- `df7fd397`：已分类的触控轻点只改变选择，不再启动空拖动或框选事务。修复点选误置未保存标记、清除重做分支的问题；通用桥的纯选择意图默认关闭，桌面行为保持原样。
- `e616b050`：重复试听回归在点击期间停止 dummy 输出回调，检查源游标精确归零并恢复输出。生产试听与保存调用不改，不放宽时间阈值；消除磁盘保存耗时污染断言的问题。
- 当前批次：触控工作台提供替换、追加、减选与清空选择，固定两列；模式只在选择工具下显示。点选和空白起手的框选复用同一集合运算，追加／减选时空白轻点保留选区。
- 追加／减选状态属于触控方案实例，经典桌面和纯鼠标方案各自保持原有状态；当前选择模式不会模拟 Ctrl、Shift 或其他键盘输入。
- 工作台使用独立的中英文短译文，英文为 Replace、Add、Remove、Clear，避免复用桌面菜单长标题使两列按钮换行过多；桌面文案保持不变。

## 已有基础与实现边界

原始多触点、稳定触点身份、第二指接管取消、双指平移与捏合已在前轮实现。取消恢复当前事务初始状态，不使用普通 Undo；剩余单指必须全部抬起才能重新编辑。

移动主画布解释器及私有头位于 `Modules/Game/Canvas/src/presentation/mobile/`。工作台布局位于移动专用 `MobileWorkbench.cpp`，公共头没有包含移动布局类型。公共输入方案只保存轻量语义值，不在桌面布局散布 Android 条件宏。

连续导航不逐帧重建全谱面滚速积分；松手后发布最终缩放配置。新选择方式只发布已有逻辑命令，不在 UI 遍历 ECS 或等待会话长锁。

## 验证状态

- 两项修复完成 Windows 完整构建与 **195/195 CTest**；试听、触控手势、触控选择三个用例各连续十次通过。
- 另增真实 `InteractionController`／历史栈回归：先建立并撤销位置动作，重放生产移动解释器的轻点命令，确认选中成功、保存点不变、重做栈保留，且重做恢复实际位置。
- Android x86_64 修复包构建和打包成功；手机和平板均复现并验证“移动保存 → 撤销保存 → 点选 → 重做保存”，完整音符数组恢复到之前的移动结果。点选后页签没有未保存星号。
- 追加／减选完成 Windows 完整构建与 **195/195 CTest**，Android x86_64 构建和打包成功；手机和平板均验证多次追加框选、点按减选、框选减选、空白轻点保留集合、替换及清空。保存回读确认只移动最终选中的物件，时间增量一致，其他属性及长条长度保持不变。
- 缩短移动按钮译文后重新完成桌面与 Android 构建，语言资源及选择交互相关 **6/6 CTest** 通过；手机英文新增按钮单行完整显示，平板中文布局复核通过。
- 桌面双击跳转和纯鼠标选择约定由既有测试继续覆盖；后续修改仍需保持这两条路径不变。

## 已知边界

- 前轮真实手指诊断已确认 NX809J 第三指落下后系统会发送 Cancel。没有关闭系统截屏手势，也不能把 Cancel 当作重做；该设备继续使用长按菜单重做。支持完整事件流的设备保留二指撤销／三指重做分类。
- 前轮 Linux 曾出现 `CanvasTouchNavigationTest` 的最终缩放采纳被空闲节流跳过。拉取验收时 Windows 单独连续十次通过，但没有修改该独立问题，不能据此宣称跨平台已修复。
- 很短的 `adb input swipe` 可能没有完整终点 Move。平板 40ms 普通 swipe 的终点曾不完整；明确发送终点 Move 后再 Up 的 40ms 注入可正确跨一轨。模拟器注入不替代真实手指体验测试。

## 后续顺序

本批完成点按单键／拖动长条后，再做折线续写及 Timing，最后完善属性与辅助面板。当前不能把基础选择与简单绘制当作全套移动编辑已完成。

一项逻辑改动保持独立提交；提交或换机前用 `git status` 核对本地与远端状态，不从旧 APK 推断当前源码。

## 本机构建与测试约束

Windows：`cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`；安卓：`scripts/android/build-windows.ps1 -Abi x86_64 -Jobs 2`。本机同时运行两个模拟器，不并发执行多个原生构建，也不把重音频测试与原生编译叠加。

手机 AVD `mmm-api36-minimal` 为 1280×720、160dpi；平板 `mmm-api36-tablet` 为 2560×1600、320dpi。测试后保持模拟器和 MMM 开启。实体机本批仅新增独立 `.next.drawprobe`，既有 `.next.dev` 与 `.next.touchtest` 数据不动。

APK 和证据位于已忽略的 `build-android/apk`、`build-android/evidence`。默认 APK 路径会随 ABI 覆盖，安装前核对 ABI 和签名；不要通过卸载原应用解决签名差异。五物件测试工程是模拟器私有目录中的 `files/acceptance-project` 副本，不是用户原工程。

## 本轮逐文件注释检查

仅检查当前绘制批次修改的代码与构建脚本，按 cloc 的 comment / (comment + code) 逐文件计算，均达到 30%；未刷新全仓基线。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2014 | 4692 | 30.03% |
| `Modules/Game/Logic/src/logic/session/tool/DrawTool.cpp` | 667 | 1555 | 30.02% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 404 | 565 | 41.69% |
| `Modules/Game/UI/src/ui/interaction/EditorInteraction.cpp` | 232 | 490 | 32.13% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 191 | 441 | 30.22% |
| `Modules/Game/Canvas/tests/MobileCanvasDrawingTest.cpp` | 125 | 290 | 30.12% |
| `Modules/Game/Canvas/CMakeLists.txt` | 122 | 283 | 30.12% |
| `Modules/Game/Canvas/tests/MobileCanvasGestureTest.cpp` | 109 | 248 | 30.53% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 111 | 230 | 32.55% |
| `Modules/Game/UI/include/ui/interaction/EditorInteraction.h` | 148 | 178 | 45.40% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasGesture.h` | 36 | 77 | 31.86% |
