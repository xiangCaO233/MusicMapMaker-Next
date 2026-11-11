# 移动端触屏交互阶段交接

更新时间：2026-11-11。状态：按用户要求暂停，后续换机器继续。

## 分支与已完成范围

当前分支：`feat/android-sdl3`。本阶段没有创建 PR 或合并 main。

- `b6b02c05`：原始多触点快照；稳定设备／手指身份、手势代际和峰值、系统取消、第二指加入时的基线。固定容量值状态，同 UI 线程发布和借用。
- `41260ab2`：第二指接管后取消尚未提交的物件拖动、画笔及框选，恢复初始组件和旧选区，不使用普通 Undo。剩余单指不能重新编辑，全部抬起后新触点才能开始。
- `8414d7dd`：双指平移和捏合时间线缩放、以双指中点锚定位置；二指轻触撤销／三指轻触重做的识别及定向命令。松手和导航期间清除画布的桌面悬浮反馈。

移动解释器和私有头位于 `Modules/Game/Canvas/src/presentation/mobile/`；公共控制器不包含这些头。经典桌面及纯鼠标方案仍走原解释器。新增通用命令字段默认保留旧行为。

连续捏合只更新局部动画倍率，不逐帧重建滚速积分或全谱面 Transform；结束才发布最终配置。平移和显式历史命令从不可变路由快照投递，不等待覆盖整个 Session 更新的注册表锁。`EditorEngine::updateTimelineZoom` 只短持配置值锁。

## 验证结果与边界

- Windows 桌面完整构建成功。
- 完整 CTest **194/194** 通过；最后补充悬浮清理后重新构建，4 项输入／导航相关测试再次通过。
- Android **arm64-v8a、x86_64** 原生构建及 APK 打包成功。打包目录均为仓库内已忽略的 `build-android`，没有改桌面部署入口。
- 实体机 **NX809J**：验证单指点按创建、绘制／物件拖动途中双指取消、剩余单指不误落笔、下一次轻点可正常编辑、长按菜单、双指平移、捏合、二指撤销。
- 平移时画面跟随 200 个物理像素；100% 捏合到 150% 后工具栏数值正确。选定音符在固定中点缩放的像素误差约 1.5px。
- 平移和捏合后保存的 `note` 数组与 72 根音符的测试基线逐项相同；二指撤销后为 71。
- **三指重做未通过实体机验收**：二指撤销并保存后，三指轻触未恢复第 72 根音符；再次聚焦画布也未恢复。CPU 三指分类用例通过。尚不能确定是系统手势接管、事件归属，还是保存后的历史状态问题，不能当作已验证功能。
- 最新悬浮清理已通过桌面相关测试和两种安卓构建；实体机先前的捏合验收包尚未包含最后这项清理，后续部署应补一次松手不残留 tooltip 的检查。

## 换机后的顺序

1. 先切到并拉取本分支，读取本文件；不要从旧 APK 的行为推断当前源码。
2. 优先排查三指重做：分别测试“撤销后直接重做”和“撤销后保存再重做”，区分手势识别、命令路由与历史栈。必要时核对系统是否发送 Cancel；不要未经确认修改用户的系统手势设置。
3. 继续原定顺序：单指点选／拖动物件／框选及多选 → 点按单键／拖动长条 → 折线续写及 Timing → 属性与辅助面板的触摸交互。上述后续步骤尚未实施，不能视为全套移动编辑已经完成。
4. 保持桌面行为和移动／桌面实现、私有头隔离；一项逻辑改动一个提交，按修改风险构建和回归测试。

## 构建与设备注意事项

已验证的 Windows 入口：`scripts/android/build-windows.ps1 -Abi arm64-v8a -Jobs 2` 和 `-Abi x86_64 -Jobs 2`。桌面使用 `cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`。本机同时运行两个模拟器且内存紧张，不要并行跑多个原生构建或开启大量 clang 任务。

实体机原有 `top.xiang233.musicmapmaker.next.dev` 应用签名与本机调试签名不同，**没有卸载或清除其数据**。本次使用独立的 `top.xiang233.musicmapmaker.next.touchtest`，显示名 `MMM Touch Test`。测试工程仅在该测试包私有目录 `files/touch-test-project`，来自内置 CanonRock 示例的副本。

独立测试包由本机忽略目录中的派生 Gradle 工程生成：复制仓库自维护的 `packaging/android` 外壳，修改 applicationId 和 label，保持 namespace／MainActivity 类名不变；JNI、SDL Java 和 assets 继续引用统一 `build-android` 输出。仓库默认脚本仍产出 `.next.dev` 包。换机若签名不同，不要通过卸载原 MMM 来解决；应沿用调试签名或另建独立测试身份，签名私钥不要提交 Git。

两个模拟器 AVD：`mmm-api36-minimal`（手机）和 `mmm-api36-tablet`（平板）。平板曾整体无响应，已单独重启且保留数据；两者均保持打开，未修改手机显示密度，也未清除设备数据。

本机完整日志、截图、APK 和注释 CSV 位于 `build-android/evidence`、`build-android/apk`，均不进 Git，换机需重新生成。APK 输出会随所选 ABI 覆盖，安装前核对 ABI。已保存测试数据，不依赖未保存的实体机编辑。

## 本阶段逐文件注释检查

按 cloc 的 comment / (comment + code) 统计，所有本阶段修改的自维护代码文件独立达到 30%。只检查本阶段修改文件，没有刷新全仓基线。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/include/canvas/Basic2DCanvasInteraction.h` | 241 | 276 | 46.62% |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2010 | 4688 | 30.01% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.cpp` | 36 | 83 | 30.25% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 30 | 62 | 32.61% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasGesture.h` | 28 | 64 | 30.43% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 184 | 421 | 30.41% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasNavigation.h` | 47 | 109 | 30.13% |
| `Modules/Game/Canvas/src/presentation/mobile/MobilePointerOwnership.h` | 16 | 37 | 30.19% |
| `Modules/Game/Canvas/tests/MobileCanvasGestureTest.cpp` | 103 | 239 | 30.12% |
| `Modules/Game/Canvas/tests/MobileCanvasNavigationTest.cpp` | 54 | 124 | 30.34% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 402 | 571 | 41.32% |
| `Modules/Game/Graphic/include/graphic/mobile/MobileTouchInput.h` | 22 | 46 | 32.35% |
| `Modules/Game/Graphic/include/graphic/sdl/AndroidNativeWindow.h` | 31 | 45 | 40.79% |
| `Modules/Game/Graphic/src/mobile/MobileTouchInput.cpp` | 38 | 88 | 30.16% |
| `Modules/Game/Graphic/src/sdl/AndroidNativeWindow.cpp` | 90 | 207 | 30.30% |
| `Modules/Game/Graphic/tests/MobileTouchInputTest.cpp` | 24 | 54 | 30.77% |
| `Modules/Game/Logic/include/logic/EditorEngine.h` | 348 | 228 | 60.42% |
| `Modules/Game/Logic/include/logic/ecs/system/ScrollCache.h` | 103 | 108 | 48.82% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 109 | 229 | 32.25% |
| `Modules/Game/Logic/include/logic/session/tool/MarqueeTool.h` | 17 | 24 | 41.46% |
| `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1635 | 3791 | 30.13% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 390 | 908 | 30.05% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1312 | 3054 | 30.05% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp` | 650 | 1512 | 30.06% |
| `Modules/Game/Logic/src/logic/session/PlaybackController.cpp` | 310 | 723 | 30.01% |
| `Modules/Game/Logic/src/logic/session/tool/GrabTool.cpp` | 771 | 1797 | 30.02% |
| `Modules/Game/Logic/src/logic/session/tool/MarqueeTool.cpp` | 127 | 293 | 30.24% |
| `Modules/Game/Logic/tests/CanvasTouchNavigationTest.cpp` | 47 | 108 | 30.32% |
| `Modules/Game/Logic/tests/GroupedDraftDragPreviewTest.cpp` | 140 | 322 | 30.30% |
