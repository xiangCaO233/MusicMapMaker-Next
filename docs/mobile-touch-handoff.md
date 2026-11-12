# 移动端触屏交互阶段交接

更新时间：2026-11-12。分支 `feat/android-sdl3`，本轮接续 `5fa8c420`。实体机测试按用户要求暂缓，使用手机和平板模拟器验收。

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

完成触控选择阶段后，继续点按单键／拖动长条的专门绘制适配，再做折线续写及 Timing，最后完善属性与辅助面板。当前不能把基础点选与多选当作全套移动编辑已完成。

一项逻辑改动保持独立提交；提交或换机前用 `git status` 核对本地与远端状态，不从旧 APK 推断当前源码。

## 本机构建与测试约束

Windows：`cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`；安卓：`scripts/android/build-windows.ps1 -Abi x86_64 -Jobs 2`。本机同时运行两个模拟器，不并发执行多个原生构建，也不把重音频测试与原生编译叠加。

手机 AVD `mmm-api36-minimal` 为 1280×720、160dpi；平板 `mmm-api36-tablet` 为 2560×1600、320dpi。测试后保持模拟器和 MMM 开启。实体机 `9125282100E7` 本轮未安装、卸载或写入应用；既有 `.next.dev` 与 `.next.touchtest` 数据不动。

APK 和证据位于已忽略的 `build-android/apk`、`build-android/evidence`。默认 APK 路径会随 ABI 覆盖，安装前核对 ABI 和签名；不要通过卸载原应用解决签名差异。五物件测试工程是模拟器私有目录中的 `files/acceptance-project` 副本，不是用户原工程。

## 本轮逐文件注释检查

仅检查本轮新增或修改代码，按 cloc 的 comment / (comment + code) 逐文件计算，均达到 30%；没有刷新全仓未修改文件基线。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2014 | 4690 | 30.04% |
| `Modules/Game/UI/tests/ProjectAudioToolScrollTest.cpp` | 442 | 1031 | 30.01% |
| `Modules/Game/UI/tests/EditorInteractionProfileTest.cpp` | 277 | 645 | 30.04% |
| `Modules/Game/UI/src/ui/interaction/EditorInteraction.cpp` | 228 | 482 | 32.11% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 189 | 439 | 30.10% |
| `Modules/Audio/include/audio/AudioManager.h` | 483 | 361 | 57.23% |
| `Modules/Game/Canvas/tests/MobileCanvasSelectionTest.cpp` | 138 | 319 | 30.20% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkbench.cpp` | 133 | 309 | 30.09% |
| `Modules/Game/Canvas/include/canvas/Basic2DCanvasInteraction.h` | 243 | 277 | 46.73% |
| `Modules/Game/Canvas/tests/MobileCanvasGestureTest.cpp` | 108 | 248 | 30.34% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.cpp` | 40 | 89 | 31.01% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasGesture.h` | 35 | 76 | 31.53% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 34 | 64 | 34.69% |
