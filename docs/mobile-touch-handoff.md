# 移动端触屏交互阶段交接

更新时间：2026-11-12。分支 `feat/android-sdl3`，本批接续 `5a2fe815`，实现移动 Timing 触控编辑，独立提交，不推送。

## 本批行为

- Timing 绘制工具下点按创建时间点，拖动创建插值范围；起点锁存效果泳道，横向跨道不改变类型。STOP 保留普通时间点入口；静止按住不会误开选择菜单。
- 选择工具点按只选中，容差内抖动不产生移动历史；空白拖动框选，标记拖动整批移动。替换／追加／减选与主画布共用触控方案状态。
- 选择工具长按 0.45 秒显示两列 Timing 菜单：复制、粘贴、剪切、删除、撤销、重做、全选、清空、三种选择模式、参数编辑、忽略吸附、定位。原长按及其释放均不能激活按钮。
- 菜单打开时冻结时间、目标与谱面实例，粘贴使用本次落点吸附时间；插值正文可直接打开完整段落编辑器。切换谱面、播放、外部弹层和方案切换均不能复用旧目标。
- 第二指或系统取消清除未提交点、区间与移动预览，并恢复本笔之前的选择，不调用 Undo、不清空重做。剩余单指须全部抬起后才能重新编辑。
- Timing 双指平移／捏合复用主画布的直接导航协议，忽略参数泳道横移；缩放锚点计入 Timing 显示补偿，不等待配置或会话回流。
- 移动参数按钮至少 44dp，来源文案改为触摸位置；插值正文单独滚动，保存／取消固定底部。插值窗口首开和最小尺寸随内容倍率调整，说明换行；菜单按真实主题内边距和行间距约束高度。
- 移动实现和私有头放在 `Canvas/src/presentation/mobile`。公开 `TimelineCanvas.h` 只前置声明中立呈现接口；工厂由 CMake 选择。主画布与 Timing 只共用移动私有的指针回放和导航路由，不将移动布局头传入桌面公共接口。
- 经典桌面及纯鼠标仍转交原指针状态机与原菜单；桌面参数窗口默认尺寸、自动按钮高度和原有提示保留。剪贴板、批量修改、插值定义、撤销和联机继续共用原业务命令。

## 验证

- Windows 最终完整构建通过，最终完整 CTest **197/197 通过**（234.48 秒）；相关五项额外通过。此前验证曾遇到 `AudioManagerTimelineIntegrationTest` 和 `SkinPackageServiceTest` 各一次偶发失败，独立复跑均通过，后续完整运行也通过；保留失败日志，未改动这两个模块或删减测试。
- 新增 `MobileTimingInteractionTest` 使用真实 ImGui 帧、原始触点代际和生产移动解释器，覆盖五泳道轻点、抖动、双向范围、第二指中断、轻点不建历史及整批移动只提交一次。
- 手机模拟器 1280×720、160dpi、英文：500ms 时间点、500ms 起的 0.375s HS 段落、粘贴到 750ms、移动到 875ms、一次撤销／重做、删除／撤销、追加整组移动、减选后子集移动均回读通过。第二指取消保留内容和重做，五个原始物件不变。
- NX809J `9125282100E7`：使用本批 arm64 独立包完成相同核心建点、区间、粘贴、移动、撤销／重做和第二指取消回归；表单触摸滚动与固定底部确认区可用，五个原始物件不变。
- 平板模拟器 2560×1600、320dpi、中文：500ms 时间点、约 558.857ms 起的 0.342857s HS 段落、粘贴到 750ms、移动到 875ms、精确撤销／重做及第二指取消后保留重做，均保存回读通过，原五个物件不变。范围端点未落在吸附阈值内时保留实际时间，不能将其误断言为固定分拍。
- 最终 APK 已在手机、平板与 NX809J 复验：七行菜单完整可见，高密度参数窗口字段不再被固定桌面宽度裁切；正文触摸滚动与底部保存／取消分离。取消后 JSON 与打开前完全一致。
- 最后审阅补上跨谱面导航中断条件；Windows、x86_64、arm64 和独立实机包均重新构建完成并安装。最终包再次验证双指导航不修改谱面内容，三设备前台进程日志没有崩溃；原实机应用更新时间与密度均未改变。APK 包名、ABI 和 SHA256 已记录到忽略的验收目录。
- 构建、注释统计、截图、输入脚本和逐步保存回读位于已忽略的 `build-android/evidence/touch-timing-*`；辅助验收脚本位于 `build-android/tools`。`.mmm` 时间戳按毫秒保存，插值 `duration` 字段按秒保存，断言区分这两个单位。

## 已完成基础及边界

- `5a2fe815`：折线新绘制、四类来源续写、事务取消及一次撤销／重做，移动绘制意图分为单键／长条与折线。
- `23e815fb`：点按单键、固定轨道双向长条，简单笔画不隐式续写、删除或合并已有物件。
- `df76335b`、`df7fd397`：替换／追加／减选、分多次框选、轻点不建空移动历史及保留重做。
- NX809J 的第三指会触发系统 Cancel；不能将其当作重做。该设备使用长按菜单重做，支持完整多指事件流的设备保留二／三指分类。
- 高级插值属性仍复用既有共享表单，未整体改写函数编辑器；历史中文字段和图表属性属于后续属性／辅助面板适配范围。本批不宣传触屏中不存在的 Shift、Esc、滚轮或 Hover 入口。
- 前轮 Linux 的 `CanvasTouchNavigationTest` 最终缩放采纳空闲门控问题未在本批修复；Windows 通过不代表跨平台已经解决。
- ADB 注入验证触点事件、布局及数据回读，不代替真人手指的手感评估。短 `adb input swipe` 可能缺少最终 Move，验收使用明确终点 Move → Up。

## 构建与设备约定

在项目根目录执行 Windows `cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`；安卓 `scripts/android/build-windows.ps1 -Abi x86_64 -Jobs 2` 或 `-Abi arm64-v8a -Jobs 2`。同一台 32GB 主机运行两个模拟器时，原生构建逐项进行，不将完整 CTest／重音频测试与原生编译并发。

手机 `emulator-5554`、平板 `emulator-5556` 测试后继续保持开启。测试工程是各自私有的 `files/acceptance-project`，清理旧测试停靠缓存前已保留副本。NX809J 使用 `.next.drawprobe`（MMM Touch Regression），原 `.next.dev` 与 `.next.touchtest` 不覆盖、不卸载，也不更改实体机密度设置。手机当前为 1216×2688、442dpi 覆盖值，本批沿用。

APK 收纳于 `build-android/apk`。默认文件会随 ABI 覆盖，验收保留 `touch-timing-x86_64.apk`、`touch-timing-arm64.apk` 和 `touch-timing-physical-probe.apk` 三份；安装前核对 ABI、身份与签名。

## 本批逐文件注释统计

口径：`cloc --by-file`，`comment / (comment + code)`，仅统计本批修改或新增代码文件。中英文 Lua 翻译表为纯资源数据，不计入程序注释率。未重扫其他模块基线。

| 路径 | 注释 | 代码 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/TimelineCanvas_Popups.cpp` | 1093 | 2548 | 30.02% |
| `Modules/Game/Canvas/src/TimelineCanvas_Interaction.cpp` | 623 | 1411 | 30.63% |
| `Modules/Game/Canvas/src/TimelineCanvas.cpp` | 576 | 1335 | 30.14% |
| `Modules/Game/Logic/src/logic/session/PlaybackController.cpp` | 314 | 729 | 30.11% |
| `Modules/Game/Canvas/src/TimelineCanvas_Interpolation.cpp` | 282 | 657 | 30.03% |
| `Modules/Game/UI/src/ui/interaction/EditorInteraction.cpp` | 242 | 497 | 32.75% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 152 | 353 | 30.10% |
| `Modules/Game/Canvas/include/canvas/TimelineCanvas.h` | 322 | 348 | 48.06% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimelineInteractionPresentation.cpp` | 156 | 347 | 31.01% |
| `Modules/Game/Canvas/CMakeLists.txt` | 133 | 297 | 30.93% |
| `Modules/Game/Canvas/src/TimelineCanvas_ContextMenu.cpp` | 111 | 253 | 30.49% |
| `Modules/Game/Canvas/tests/MobileTimingInteractionTest.cpp` | 118 | 250 | 32.07% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.cpp` | 60 | 138 | 30.30% |
| `Modules/Game/Canvas/src/presentation/mobile/MobilePointerInputScope.h` | 50 | 84 | 37.31% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileNavigationRouting.h` | 31 | 70 | 30.69% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.h` | 40 | 62 | 39.22% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasFactory.cpp` | 9 | 17 | 34.62% |
| `Modules/Game/Canvas/src/presentation/desktop/DesktopCanvasFactory.cpp` | 8 | 15 | 34.78% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimelineInteractionPresentation.h` | 4 | 8 | 33.33% |
