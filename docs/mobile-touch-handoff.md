# 移动端触屏交互阶段交接

更新时间：2026-11-11。当前分支 `feat/android-sdl3`，接续基线 `d2f63983`。本轮范围为保留长按菜单撤销／重做，完善单指点选、物件拖动和空白框选；本轮改动随本次提交记录，尚未推送。

## 当前交互

- 触控选择工具：点按物件选中，拖动物件及其可命中子部位沿用原有移动／调整语义；从空白处拖动才框选。空白轻点清空选择，框选后可拖动已选集合。
- 原点回放锁定实体、注册表、部位和子索引，不能在移到另一物件后重新决定拖动目标。
- 十逻辑像素容差区分轻点抖动与拖动。快速移动和抬手同帧到达时，平台释放标志经中立桥补足通用控制器的最终 Update → End；框选不能退化为空白轻点清空。
- 选择工具连续轻点继续选择，不触发经典桌面双击 seek；经典桌面和纯鼠标仍走桌面解释器，绘制工具语义不在本轮扩展。
- 长按候选期间没有编辑副作用，菜单继续提供撤销／重做。第二指或系统取消仍通过既有事务取消恢复旧状态，不用普通 Undo 撤回未提交动作。

## 三指与悬浮反馈结论

NX809J 当前系统在第三指落下后即取消应用整段输入。独立、不含 SDL 的 Activity 已用真实手指确认：三指轻触在第三指落下后 6ms 收到 Cancel，下滑在 2ms 后收到，之后均无 Move／Up；用户确认下滑正常截屏。因此无法从应用收到的事件可靠区分两者。用户现已选择保留长按菜单撤销／重做并继续单指适配；没有关闭系统截屏或其他手势，也不能把 Cancel 当作重做。

支持完整事件流的设备仍可使用分支已有的二指轻触撤销／三指轻触重做分类。前一轮已在手机确认菜单重做的“撤销后直接重做”和“撤销→保存→重做”路径。单指释放、平移结束与捏合结束后不残留画布 Tooltip，选中高亮保留；导航前后完整音符数组相同。

## 验证与证据

- Linux 完整构建成功；本轮新增 `MobileCanvasSelectionTest`，直接链接生产移动解释器、真实 ImGui 帧和原始触点跟踪器，观察正式命令。
- CPU 回归覆盖容差内抖动点选、连续点选、草稿折线节点身份锁存、持续拖动释放终点、同一帧内移动并释放的快速物件拖动／框选、空白轻点和经典桌面双击跳转；相关 9 项测试通过。
- 完整 CTest 为 193/194。唯一失败仍为基线已有的 `CanvasTouchNavigationTest`：最终缩放配置采纳可能被空闲节流跳过；指针覆盖未撤、积分基准为 1。该独立问题未夹带修复，不能声称全绿。前轮定位补丁在集中证据树 `touch-handoff-20261111/zoom-idle-followup.patch`。
- Android arm64-v8a 与 x86_64 构建成功。NX809J 在独立五物件工程副本上经 ADB 触摸注入验证：点选高亮；单件横移只改一根音符的轨道；空白框选三件后成组移动，保存回读确认三件均偏移 250ms、轨道及长条时长不变，其余两件未变。
- 最终包补验：常规 ADB 40ms 拖动与框选通过；从已选物件开始移动后，保存回读仍确认三件统一偏移 250ms。
- 菜单撤销恢复完整基线；保存后菜单重做恢复完整成组位移。空白轻点清空高亮和框，未增加谱面修改。
- 本轮证据集中在 `/home/xiang/Android/mmm-smoke/main/evidence/selection-20261111`，包含保存回读、截图、构建日志、测试日志和逐文件注释 CSV；前三指诊断证据在并列的 `touch-handoff-20261111`。测试原始文件会因首次保存补齐 collaboration_id／extra 并排序，后续比较使用首次保存后的完整音符数组。

SDL 边界：`adb input swipe ... 1` 可能只发 Down／Up，不包含 Move；SDL 会用最后已知触点位置构造 Up，不能据此把远处 Up 坐标当作编辑终点。底层终点补偿试验已撤回。CPU 回归明确把终点位置事件送入队列再释放，不用这种不完整注入代替真实拖动。自定义 app_process 单 Move 注入曾出现跨两轨，而常规 ADB 40ms／240ms 输入按预期跨一轨；该差异尚未用真实手指对照，不能宣称所有高速输入精度已验收。

## 代码与下一阶段

移动私有策略在 `Modules/Game/Canvas/src/presentation/mobile/`。通用控制器不包含移动私有头；中立桥新增的完成拖动参数默认关闭，桌面调用保持原样。原始触点跟踪器和第三方依赖保持原样，新增判定只保存画布局部标量。

剩余次序：触控追加／减选的具体交互方案（尚未设计）→ 点按单键／拖动长条的专门绘制适配 → 折线续写及 Timing → 属性与辅助面板。不要把本轮基础选择验收当作全套移动编辑完成。

继续开发时先核对分支与远端，不从旧 APK 推断当前源码；一项逻辑改动保持独立。不要混入已知缩放节流修复，除非用户扩大范围。

## Linux 构建与设备约束

构建、缓存、签名、APK 与证据统一在 `/home/xiang/Android/mmm-smoke/main`，未修改 `.gitignore`。

```bash
cmake --build /home/xiang/Documents/coding/c_cpp/MusicMapMaker-Next/build --parallel 12
ctest --test-dir /home/xiang/Documents/coding/c_cpp/MusicMapMaker-Next/build --output-on-failure
MMM_ANDROID_WORK_DIR=/home/xiang/Android/mmm-smoke/main MMM_ANDROID_ABIS='arm64-v8a x86_64' MMM_ANDROID_JOBS=12 bash /home/xiang/Documents/coding/c_cpp/MusicMapMaker-Next/scripts/android/build.sh
```

当前手机序列号 `9125282100E7`，APK 为 `.next.dev`，调试签名已核对一致，可保留数据覆盖安装。原 Windows `.next.touchtest` 包和工程不写入、不卸载。手机测试前备份 `.next.dev` 的 user_config.json 与 imgui.ini，结束后逐字节恢复，不清空用户数据。本轮两份原配置已逐字节恢复，手机私有测试副本和临时注入 dex 已清理，完整测试副本归档到集中证据目录。Waydroid 本轮未连接，不能沿用旧轮次模拟器验收当作本轮结果。

历史 Windows 构建入口仍为 `scripts/android/build-windows.ps1`，当时产物位于本机忽略的 build-android；Linux 不复用其缓存或签名假设。安装前检查目标 ABI 与签名，不以卸载用户应用解决签名差异。

## 本批逐文件注释检查

按 cloc --by-file 的 comment / (comment + code) 统计，仅检查本批修改代码，未刷新全仓基线。各文件均独立达到 30%；文档不参与统计。

| 路径 | 注释行 | 代码行 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 187 | 432 | 30.21% |
| `Modules/Game/Canvas/CMakeLists.txt` | 119 | 276 | 30.13% |
| `Modules/Game/Canvas/tests/MobileCanvasSelectionTest.cpp` | 99 | 226 | 30.46% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.cpp` | 39 | 85 | 31.45% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasGesture.h` | 33 | 75 | 30.56% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 32 | 63 | 33.68% |
