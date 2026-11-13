# 移动端触屏交互 TODO

更新时间：2026-11-14。继续当前分支 `feat/android-sdl3`。本地 T08～T11 核心交互已完成；T11 已补 NX809J 真机验收并修正高 DPI、大键盘下的输入空间。**T12 辅助工具审计进行中**，相机参数／预览及 BPM 测量核心交互已通过模拟器与 NX809J 真机验收；布局元素／预设已实现并通过模拟器核心验收，真机包已准备，待手动解锁补验。

## 换机接续入口

- 先阅读仓库根目录 `AGENTS.md` 和本文。用户明确要求继续当前分支；保持经典桌面、纯鼠标方案和业务模型不变，一个职责一个提交，不夹带重构或公开发布。
- 换机后先获取远端 `feat/android-sdl3`，核对 `git status`、当前分支及上述业务提交是否存在，核对本机回归及 T12 验收记录后继续其余辅助工具；布局本批相关 6/6 通过，最终完整测试 201/203，导航失败仍在、音频项独立复跑通过；先补布局真机验收。已有本地修改时先保留，不以重置或强推覆盖。
- T02 正交结构已完成本机三设备核心回归，后续按 T12 清单继续；先核对已有能力，再设计触控入口。表中的“待完善”包含已有桌面功能的触控强化，不能当作这些业务功能完全不存在，也不能宣称所有候选问题都已在设备复现。
- 上一机器产物在 `/home/xiang/Android/mmm-smoke/main`，本次 Windows 产物在项目忽略目录 `build-android`；这些内容不会随普通 Git 拉取迁移；需要原始证据时单独复制，不能将备份、APK 或缓存加入源码提交。新机器重新检查 SDK／NDK／JDK、依赖、可用设备和签名；不得沿用本机 ADB 序号、路径或授权状态作为已连接证据。

## 触屏交互 TODO

优先级及顺序沿用本轮讨论；每项先检查实际缺口，再按独立职责实现、验收和提交。`待完善` 不是已确认缺陷清单。

### T12 辅助工具审计（2026-11-14）

T12 尚未全部完成。相机参数／预览与 BPM 测量分别实现、验收和提交；其余工具继续按下表逐项核对。

| 范围 | 已核对情况 | 接续工作 |
| --- | --- | --- |
| 相机布局参数／预览 | 小屏复现参数、预览、布局面板三窗重叠；原多分量精确输入依赖 Ctrl，观察旋转／平移／缩放依赖中键和滚轮。本批提供内嵌标量参数、独立观察页与主画布预览切换。 | NX809J 实际输入法与参数／预览核心验收已补齐；三维轴／环的多指与系统 Cancel 仍需独立适配，不能把参数替代入口视为已完成全部场景手势。 |
| BPM 测量 | 已接入移动中央页签、图表／参数分页、单指浏览、轻点定位、双指缩放、显式拍点编辑和本地历史；桌面保留双栏与原鼠标操作。 | 模拟器与 NX809J 核心交互、精调及应用保存通过；自动测量的多段建议、音频标记导出等完整组合仍待独立验收。 |
| 布局元素／预设 | 原始单指捕获、48dp 命中带、抓取偏差、窄区最近句柄及文字主体移动／外角缩放已接通；预设独立名称模态与 SAF 导入导出完成。 | 模拟器小屏与高 DPI 核心验收通过；NX809J 新隔离包已安装但未启动，待手动解锁。旧 `.0923` 窄工作台比例限制仍登记，默认 `.17` 已复核。 |
| BMS／其它辅助工具 | BMS 开关、包导出选项、重叠检查、数据源替换和倍速导出源码都有显式选择／确认入口，未据此宣称设备通过。 | 按窗口逐项验证滚动、文字输入、目标选择及确认／取消；完整文件选择／导出组合仍待验。 |

#### 布局元素与预设触控（2026-11-14，真机待验）

- 布局工具独立接收原始单指，不经过通用长按菜单或双指导航。边缘、角点及中心命中带至少 48dp，显示尺寸保留原样；按下只冻结目标与抓取偏差，越过容差后才改变配置，普通 Up 补交最后坐标。多指、Cancel、换谱／方案／键数、显示尺寸与停靠身份变化、隐藏缺帧均封锁旧代际，保留最后合法结果；同节点因配置采纳而自发改变几像素宽度可继续拖动，不再误取消。
- 文字主体移动、外角缩放，窄辅助区按最近句柄消解扩大命中带的遮挡；桌面维持原命中优先级。原吸附、邻区联动、各键数保存及布局文件格式不变。移动长表单限制内侧宽度、滚动与 48dp 控件行；预设复制／重命名使用键盘避让的草稿模态，Enter 收键盘、显式确认才提交，删除需确认。系统选择异步返回，导出关闭 writer 后再发布 SAF 文档；修复反馈 Combo 的样式栈配对，并增加真实打开列表的零 ImGui 错误回归。
- **模拟器核心验收通过**：1280×720／160dpi 完成复制取消、复制、重命名、删除确认取消、预设选择／恢复、实际 SAF 导出和再导入；导入布局与导出 JSON 全字段相等。主轨道边缘在偏离把手 16px 的起点拖动，实际宽度方向推进约 78px；静止按住不修改或弹菜单。判定线、窄批注区中心移动、文字主体移动与外角缩放通过。文字主体拖动只改位置，字号仍 `.035`；外角拖动后字号为 `.041735582`。另在 2688×1216／442dpi、默认工作台比例 `.17` 与 Gboard 下，名称字段及确认／取消完整位于键盘上方，取消后库不变。旧窄比例 `.0923` 和旧 INI 的显示限制未在本批修改全局 Dock 逻辑。
- 重启重新打开工程后，选中 `T12Saved (2)`，库与退出前相等，当前视觉配置与选中布局全部字段相等；整份谱面字节与基线一致，SHA256 为 `241b3f992765b4f042b625894a6caa5d796d53228b33dec6b135ab8702b6f6a3`。上述为自动注入的核心验收，不代替真人手感、全部组件组合或厂商系统抢占验收。
- **NX809J 尚未运行验收**：最终 ARM64 包已安装到独立 `top.xiang233.musicmapmaker.next.t12layoutprobe`，工程与配置已隔离准备，但未启动；手机自动锁屏，已请求用户手动解锁，等待补验。原应用、数据、密度、输入法和系统手势未改动。解锁后先核对当前页面与可用状态，再启动该包，验证拖动、名称输入、选择／重开与保存回读；完成后卸载本批新包。不得把安装成功或模拟器高 DPI 通过记为真机通过。
- Linux 完整构建、Android 双 ABI 构建、签名与三份原生库 Build ID 校验通过。最终相关 CTest **6/6**：`LayoutPresetsTest`、`MobileInputEditorTest`、`LayoutEditingTest`、`MobileLayoutEditingTest`、`CameraLayoutEditorTest`、`MobileCanvasGestureTest`。新回归调用正式布局控制器并使用真实 ImGui／原始触点，覆盖静止、正常尾点、合帧、Cancel、多指、外部起点、同节点 5px 配置采纳与窄区中心。首轮完整 **203/203**；最终完整 **201/203**，失败为既有 `CanvasTouchNavigationTest` 最终配置采纳断言及未修改的 `AudioManagerTimelineIntegrationTest` 共享 stretcher 断言。独立复跑音频项通过，导航仍失败；未修改、屏蔽或弱化这些测试，不声称完整回归全绿。
- 证据、脚本、APK、读回数据与注释率集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t12-layout-20261114`。模拟器原配置／INI 已逐字节恢复，13 份旧谱面哈希不变；本批工程、SAF 导出与对应工作副本／映射已清理，恢复 1080×2400／420dpi、返回桌面并保持模拟器服务在线。手机隔离包留待解锁补验，未作为清理完成。未改 `.gitignore`，产物和缓存不入仓；本批只本地提交，不推送／合并。

本批 9 个修改／新增代码与 CMake 文件逐文件均满足 30% 注释率，完整原始统计见集中目录的 `comment-rates.json`／`comment-rates.csv`：

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/CMakeLists.txt` | 148 | 345 | 30.02% |
| `Modules/Game/Canvas/include/canvas/Basic2DCanvasInteraction.h` | 248 | 282 | 46.792% |
| `Modules/Game/Canvas/include/canvas/TrackLayoutEditing.h` | 171 | 396 | 30.159% |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2104 | 4907 | 30.01% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 221 | 511 | 30.191% |
| `Modules/Game/UI/src/ui/imgui/manager/LayoutPresetControls.cpp` | 127 | 294 | 30.166% |
| `Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp` | 1995 | 4653 | 30.009% |
| `Modules/Game/Canvas/include/canvas/LayoutTouchInput.h` | 43 | 89 | 32.576% |
| `Modules/Game/Canvas/tests/MobileLayoutEditingTest.cpp` | 118 | 267 | 30.649% |

#### BPM 测量触控核心（2026-11-14）

- 工具停靠在移动中央页签；修正默认桌面窗口位置使移动页签脱离节点、被主画布遮挡的问题。图表与长参数表分开，一次显示完整波形或频谱，切换保留共享视野。常用按钮、参数行与应用确认框至少 48dp，常驻说明替代触摸 Hover 提示；桌面布局与业务算法保持原路径。
- 浏览模式单指平移、正常轻点定位、双指按实际可见时间窗锚定缩放；首尾裁切处也会真实改变图像跨度。显式“调整拍点”模式复用原拍点算法，命中带至少 48dp，保留抓取偏差，越过移动阈值才编辑；一笔拖动只产生一次测量历史。新增可见首拍、撤销与重做按钮；浏览不消耗历史或覆盖重做。
- 原始触点按图表、代际和稳定指针认领；快速 Down／Up 按真实起点裁决遮挡，不依赖延迟的合成鼠标位置。取消、第三指、换轨、分页、几何改变、隐藏和方案切换拒绝旧输入；只保留最后已显示的合法编辑，不使用 Cancel 坐标补一次修改。播放定位仍经原同步／独立试听路由与视觉偏移换算。
- **设备核心验收通过**：模拟器 **1280×720／160dpi**、NX809J **2688×1216／442dpi**，使用实际 CanonRock 音频。波形／频谱、快速轻点定位、浏览／捏合、拍线拖动、一次撤销恢复 120 BPM 与重做均通过。精调草稿取消后重开源值不变；两机原输入法下精调的输入与操作按钮均可见。
- 两机明确应用 **128.5 BPM、首拍 250ms**，先取消目标确认再重开应用、保存；回读 JSON 的全部非 Timing 数据与基线一致。保留原流速会按 BPM 补偿 SV，八个采样时间的有效 `BPM × SV` 与原谱相等，未把合法补偿误判为 SV 原值应不变。最终 APK 覆盖安装并重启、重新打开工程后，两机文件哈希一致且内容保持上述结果。播放游标与视野实际推进；隔离配置为静音，不据此声称真人听音验收。
- Linux 完整构建、相关 CTest **6/6**、最终完整 CTest **202/202** 通过。真实 ImGui 回归覆盖宽命中无按下跳变、原拍点反算及单笔历史，生产 Dock 回归覆盖完整工具首次／后续帧。临时恢复旧移动窗口定位后回归按预期失败，再恢复修复通过。早前轮次出现过已登记的 `CanvasTouchNavigationTest` 间歇失败，本批未修改或弱化该测试，不能据最终通过声称已经修好。
- Android 双 ABI 构建、签名、全部 APK 原生库 Build ID 与安装包 SHA 核对通过。设备手势由 ADB 注入；Cancel／第三指等边界由回归覆盖，真人手感、真实系统取消及鼠标热插拔未新增验收。
- 证据、脚本、APK、配置备份和逐文件注释率集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t12-bpm-20261114`。模拟器原配置／INI 已逐字节恢复、13 份旧谱面哈希不变，移除本批工程及注入 jar，恢复 **1080×2400／420dpi** 并保持模拟器服务在线；临时安装存储阈值已恢复。真机仅新增 `.next.t12bpmprobe`，验收后卸载并移除本批 jar，原应用、442dpi、百度输入法及系统手势未改动，返回桌面。不改 `.gitignore`，不提交产物或缓存；本批仅本地提交，不推送／合并。

本批 6 个修改／新增 C++ 文件均满足逐文件 30% 注释率，原始统计为上述目录的 `comment-rates.json`／`comment-rates.csv`：

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/UI/include/ui/imgui/menu/actions/tools/BpmMeasurementToolView.h` | 394 | 313 | 55.73% |
| `Modules/Game/UI/src/ui/imgui/menu/actions/tools/BpmMeasurementToolView.cpp` | 1683 | 3924 | 30.02% |
| `Modules/Game/UI/src/ui/workspace/MobileDockLayout.cpp` | 216 | 490 | 30.59% |
| `Modules/Game/UI/tests/BpmMeasurementHistoryTest.cpp` | 207 | 468 | 30.67% |
| `Modules/Game/UI/tests/MobileDockLayoutTest.cpp` | 244 | 563 | 30.23% |
| `Modules/Game/UI/include/ui/imgui/menu/actions/tools/BpmPlotTouch.h` | 91 | 195 | 31.82% |

#### 相机参数／预览首批（2026-11-14）

- 移动相机参数并入既有布局面板，不再创建两层桌面浮窗；位置／旋转逐轴显示，长度、运动补偿、厚度补偿保留原单位与范围。标量与按钮至少 48dp，复用 T11 长按精调的草稿、有限值校验和明确应用／取消；常驻说明取代 Ctrl 输入提示。保留平移轴／旋转环入口，桌面仍使用原窗口和操作路径。
- 观察页提供俯仰、方位、距离和中心 XYZ，只改变辅助观察；实际预览在同一画布区域显示当前纹理，不缓存描述符或创建 GPU 所有者，也不额外重复填充整张纹理。参数修改在侧栏真实变化边沿捕获当前布局预设并调用原配置通知入口，不等待会话更新或同步磁盘写入。
- 小屏 **1280×720／160dpi**：实际软键盘下位置 X `0 → 0.25` 明确应用并落盘，下一次输入 `9` 后取消不覆盖；旋转 Y `0 → 15°` 只改变目标分量；实际预览与操作视图切换正常。观察俯仰 `25 → 40°` 后场景即时更新，持久化的整个 VisualConfig 不变。切回选择工具后实际相机姿态生效；保存后整谱与原基线完全一致，进程重启后配置仍保留。
- 大 DPI 模拟器 **2688×1216／442dpi**：使用默认工作台宽度比例 `.17`，在布局页滚动至位置字段，长按后输入与两枚操作按钮完整位于 Gboard 上方；X `0.25 → 0.375` 落盘，其它位置分量与旋转保持不变。旧窄宽度比例 `.0923` 的显示限制作为后续布局项登记，没有通过本批修改全局 Dock 逻辑。
- Linux 完整构建通过，相关测试 **5/5**（相机、精调、触控滚动、操作方案、布局预设）；最终完整 CTest **201/202**，仍为此前登记的 `CanvasTouchNavigationTest` 最终配置采纳断言失败，本批不改或弱化其测试。真实 ImGui 回归补充移动浮窗生命周期、48dp 按钮、目标复位保留长度与补偿、观察复位不修改目标和预览模式无残留事务。Android 双 ABI 构建、签名和全部 APK 原生库 Build ID 核对通过。
- **NX809J 真机补验通过**：**2688×1216／442dpi**、原百度输入法。精调的活动数值与应用／取消按钮位于键盘上方；输入 `0.25` 后 Enter 只收键盘、取消后重开仍为源值；X 明确应用为 `0.375`，同时保留其它位置分量。旋转 Y 应用为 `15°`，其余 VisualConfig 保持不变；观察俯仰 `25 → 40°` 只改变辅助场景，整个持久化 VisualConfig 不变。操作视图／实际预览切换、退出至选择工具、保存后整谱与基线完全一致、进程重启并重开工程后的姿态均通过。安装 APK SHA 与已验 ARM64 产物一致；真人手感、真实系统三指／Cancel 和其它输入法仍未覆盖。
- 产物、缓存、完整配置／谱面证据及 `comment-rates.csv` 集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t12-20261114` 和既有 `main` 工作树。模拟器仅更新 `.next.t01probe`，原配置与 INI 已逐字节恢复、13 份旧谱面哈希不变，移除隔离工程、恢复 **1080×2400／420dpi**，模拟器服务保持在线。真机只新增 `.next.t12probe`，验收后已卸载，原应用、输入法及 442dpi 未改动，手机返回桌面；本次未改源码，仅补充验收记录。不改 `.gitignore`，不提交 APK 或缓存；本批仅本地提交，不推送／合并。

本批修改代码逐文件注释率如下，完整原始统计保存在上述目录：

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/include/canvas/CameraLayoutEditor.h` | 39 | 56 | 41.05% |
| `Modules/Game/Canvas/src/Basic2DCanvas.cpp` | 1142 | 2656 | 30.07% |
| `Modules/Game/Canvas/src/CameraLayoutEditor.cpp` | 287 | 666 | 30.12% |
| `Modules/Game/Canvas/tests/CameraLayoutEditorTest.cpp` | 409 | 953 | 30.03% |

### T11 数值与文本属性输入（2026-11-13～14）

- 触控方案为公共单值 Slider／Drag 增加静止长按精调，支持 `float`、`double`、`int32`；草稿与源值分离，显式应用／取消，提供步长切换与加减。严格检查全文、有限性、范围和整数语义；源值变化、目标隐藏、方案／上下文切换拒绝旧草稿。不支持的标量类型、多分量控件及显式 `NoInput` 控件保留原入口；没有精度降级地接管 64 位整数。应用沿用原 Changed 与编辑完成边沿，只报告一次真实存储值变化，兼容只在释放时落盘的既有设置。
- Java 被动观察本窗口 IME Insets 与动画，几何通过独立快照发布，JNI 从 `libmain` 的 Android 启动模块导出。文本模态使用单独输入安全区，不把临时键盘尺寸固化成普通辅助窗布局；主画布不随 IME 缩小。正文与父模态共用含留白的尺寸上限，固定正文身份、局部滚动和底部操作行，键盘缩小时把活动字段滚回可见区。系统收起键盘只结束文本焦点、保留草稿，防止后端立即重弹。
- 已接入普通 Timing 参数窗、画布批注和批注表独立编辑窗；文件重命名／新建目录的字段按模态余宽排布。移动批注 Enter 只换行，文件名和精调 Enter 不提交，保存／应用仍需明确按钮；桌面批注保留原生弹窗、尺寸与 Enter 语义。
- 小屏模拟器 **1280×720／160dpi**：实际软键盘下精调字段和操作行可见；界面倍率 `1 → 1.05` 落盘并恢复。文件名输入／Enter／取消不重命名，明确确认后目录变为 `T11Renamed`，内部 `Readme.txt` SHA 不变。批注两行正文经过换行、返回收起、取消及保存验证，最终保存回读为 `T11_saved\nsecond_line`，原身份、作者、时间和其它整谱字段不变。Timing 长表单的活动 SV 输入和三按钮完整可见，`2 → 2.125` 保存回读正确，一次撤销后整谱精确恢复。
- 平板尺寸 **2560×1600／320dpi**：Timing 输入和底部操作行位于键盘上方；输入 `3.375` 后取消并保存，整谱与取消前完全一致。以上使用隔离工程和 ADB 输入，不代表真人手感或所有输入法／系统返回手势组合。
- NX809J **2688×1216／442dpi**（11 月 14 日补验）：大字号和百度横屏键盘暴露标题／留白挤占正文、复合数值框导航位置错误两处问题。键盘可见时省去原生标题，保留触摸按钮下限，并紧跟实际输入项按 ItemRect 滚动正文，禁止滚动父模态。真实 SV 字段及三按钮完整可见，`2 → 2.125` 保存正确，一次撤销后整谱精确恢复；批注 Enter 换行、返回收键盘不保存、取消和明确保存通过，保存正文为 `Phone_first\nPhone_second`，其余整谱字段不变；数值精调 Enter／取消不写入，明确应用后界面倍率 `1 → 1.05` 落盘。文件名确认语义沿用本批模拟器验收，未在手机重复测试。
- Linux 完整构建通过，最终相关回归 **6/6**。11 月 13 日完整套件曾 **202/202**；14 日补验最终为 **201/202**，既有 `CanvasTouchNavigationTest` 在最终配置采纳断言失败，单独复跑仍失败，未修改或弱化该测试／导航逻辑。本批新增真实 ImGui 回归覆盖草稿、重复打开自动全选、单次应用、取消、源冲突、焦点、键盘收起及实际字段／按钮裁剪，并加入真机尺寸、2.7625 倍字号和 792 像素键盘的回归。Android **x86_64／arm64-v8a** 构建、签名、JNI 导出和 APK 原生库 Build ID 核对通过；最新版小屏模拟器复测活动 SV 字段、操作行和取消后保存，整谱与基线完全一致。
- **待验边界**：API 26～29 的可视框回退只完成构建，未在对应系统运行。其它辅助工具的专用长表单及修饰键入口按 T12 逐项审计，不能把公共避让接入视为所有窗口均已设备验收。
- 证据、脚本、APK、注释率 CSV 集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t11-20261113`，构建与缓存仍在同一 `main` 工作树。模拟器只更新 `.next.t01probe`，原配置／INI 已逐字节恢复，13 份旧谱面哈希不变；移除本批工程，恢复 **1080×2400／420dpi** 并保持模拟器在线。手机只安装新增的 `.next.t11probe`，验收后已卸载；原应用、数据、输入法和 442dpi 未改动。模拟器临时存储阈值已恢复原值；不改 `.gitignore`，不提交产物／缓存。本批仅本地提交，不推送或合并。

T11 核心批次 19 个新增／修改代码与 CMake 文件均达到逐文件 30% 注释率；真机补验修改的 7 个 C++ 文件已重新逐文件检查，均达标，CSV 为 `phone-comment-rates-final.csv`。翻译与 Manifest 纯配置不计入代码；下表已刷新补验涉及的条目。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2036 | 4747 | 30.02% |
| `Modules/Game/Canvas/src/TimelineCanvas_Popups.cpp` | 1162 | 2705 | 30.05% |
| `Modules/Game/UI/src/ui/utils/UIWidgetUtils.cpp` | 685 | 1597 | 30.02% |
| `Modules/Game/UI/src/ui/imgui/manager/FileManagerView_Hierarchy.cpp` | 639 | 1485 | 30.08% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkspacePresentation.cpp` | 349 | 802 | 30.32% |
| `Modules/Game/UI/CMakeLists.txt` | 221 | 509 | 30.27% |
| `Modules/Game/UI/tests/MobileInputEditorTest.cpp` | 136 | 316 | 30.09% |
| `Modules/Game/UI/src/ui/workspace/MobileInputEditor.cpp` | 132 | 292 | 31.13% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileAnnotationTablePresentation.cpp` | 115 | 267 | 30.10% |
| `Modules/Game/UI/include/ui/utils/UIWidgetUtils.h` | 317 | 172 | 64.83% |
| `Modules/Main/src/AndroidBootstrap.cpp` | 65 | 147 | 30.66% |
| `Modules/Game/UI/src/ui/workspace/WorkspacePresentation.cpp` | 62 | 141 | 30.54% |
| `Modules/Game/Graphic/CMakeLists.txt` | 53 | 122 | 30.29% |
| `packaging/android/app/src/main/java/top/xiang233/musicmapmaker/MainActivity.java` | 49 | 96 | 33.79% |
| `packaging/android/app/src/main/java/top/xiang233/musicmapmaker/SoftKeyboard.java` | 34 | 73 | 31.78% |
| `Modules/Game/UI/include/ui/workspace/IWorkspacePresentation.h` | 59 | 67 | 46.83% |
| `Modules/Game/Graphic/src/mobile/MobileKeyboard.cpp` | 16 | 26 | 38.1% |
| `Modules/Game/UI/include/ui/workspace/MobileInputEditor.h` | 17 | 21 | 44.74% |
| `Modules/Game/Graphic/include/graphic/mobile/MobileKeyboard.h` | 5 | 6 | 45.45% |

### T10 音频图表与资源面板（2026-11-13）

- 触控方案下，波形／频谱在实际绘图区轻点定位，单指横拖浏览，双指平移／锚点缩放；纵向起手留给页面滚动。声道共用首触点归属，半窗仍为 0.1～10 秒。正常 Up 保留尾坐标；Cancel、第三指、跨声道配对、窗口重排、隐藏、切谱、弹层和切换方案结束最后有效导航，余指不补点击。不引入撤销／重做、主画布缩放或物件编辑；经典方案保留鼠标路径。
- 资源轻点和长按共用从画布提取的值状态分类器：稳定路径／资源 ID 锁定目标，滑动、取消及多指不误激活。音频资源长按复用原类型／移除菜单，新增“用于绘制”，再到主画布目标轨放置，替代跨窗口拖放；文件条目长按复用新建、复制、粘贴、重命名和删除确认，列表空白长按提供根目录菜单。没有复制另一套文件或采样业务命令。
- 真机验收发现抽屉内重命名模态打开后，外壳停止提交其父面板，使模态下一帧消失。现保留父面板绘制，仅省略外壳遮罩／握柄，背景输入继续由 ImGui 模态阻挡。NX809J 已完成目录重命名、复制／粘贴及文件内容 SHA 回读；“用于绘制”确实新增绑定 `canonrock.mp3` 的 BGM 样本并保存。波形与频谱按既有“同步效果器”入口生成真实双声道分析数据，不用空图占位冒充渲染成功。
- Linux 完整构建及完整 CTest **201/201** 通过；此前登记的 `CanvasTouchNavigationTest` 本轮最终通过，未修改该测试来规避历史间歇失败。新增真实 ImGui／原始触点／正式播放控制器回归覆盖定位偏移、合帧尾点、方向仲裁、多指、取消、重排／弹层／换谱和资源轻点／长按单次交付。Android 双 ABI 构建、签名和 APK 原生库 Build ID 核对通过。16 个本批修改的 C++／CMake 文件逐文件注释率均达到 30%，明细 CSV 与构建日志集中保存。
- x86_64 模拟器小屏 **1280×720／160dpi**、平板 **2560×1600／320dpi** 及 NX809J 完成实际波形／频谱定位、横拖、按住期间缩放与保存回读；注入 Cancel 后保留最后有效浏览。手机及两种模拟器布局导航后的整份谱面 JSON 与各自放置音频后的保存基线一致。手机正常退出后工作区定位约为 **1.7996 秒**，不以旧工作区快照代替实时 Seek 结果。文件空白根菜单另在平板验证。注入不代替真人手感、厂商系统抢占或全部滑块／EQ 参数组合验收。
- 证据和产物位于 `/home/xiang/Android/mmm-smoke/main/evidence/t10-20261113`。已移除本轮新建的手机 `.next.t10probe` 和注入工具，真实应用保持原样；模拟器恢复原配置／ini 字节、13 份原谱面哈希、1080×2400／420dpi，并保持在线。安装时因低空间临时调整的模拟器存储保留阈值已恢复为原先未设置状态。
- **T11 接续边界**：名称输入目前仍可能被软键盘遮住提交按钮，可先按返回隐藏键盘后确认；返回只收键盘，不代表确认。下一批处理活动字段／确认按钮避让、明确确认取消及数值精调。本批不改变 EQ、频谱重算或文件格式语义，不推送／合并分支。

### T09 预览区导航（2026-11-13）

- 触控方案下，内容区轻点按实际显示帧反投影定位；单指拖动浏览，双指平移／捏合缩放。密度栏继续按全谱时间定位，顶部为曲尾、底部为开头。预览范围限制沿用 1～50，不改变主画布倍率、横移、物件、选区或编辑历史；经典方案保留原鼠标路径。
- 首触点决定窗口及内容／密度栏归属，跨窗第二指不接管；双指任一指抬起即结束，剩余指不回放点击。系统 Cancel、三指、几何变化、弹层、隐藏、切谱及切换方案结束最后有效浏览，未成立的轻点不补发。预览不使用主画布的二指撤销／三指重做。
- 定位沿用真实 ScrollCache 和正式 Seek，支持跨变速段；轻点不读取滞后的 Hover 时间。排队命令绑定谱面实例，切谱后拒绝旧目标。正常 Up 单独保留最终坐标，Down／Up 合帧不丢拖动尾点。连续缩放只传标量覆盖，绘制、可见范围和反投影同步使用新倍率；释放后短配置锁发布最终范围，直到逻辑采纳才撤去覆盖，不逐帧广播整份配置或等待会话长锁。
- Linux 完整构建及相关 CTest **7/7** 通过，最终完整套件 **199/200**；只有此前已登记的 `CanvasTouchNavigationTest` 失败，未修改或绕过。真实 ImGui／原始触点／播放控制器回归覆盖跨 SV 轻点、合帧尾点、多指死区与锚点、取消、外部起点、隐藏／弹层／切谱、经典方案回退和最终配置交接；实际会话渲染快照验证配置尚未采纳时预览几何已更新，主画布比例仍为一。
- Android **x86_64／arm64-v8a** 均构建、签名校验通过；两份隔离 APK 内原生库与集中构建库按 ELF Build ID 核对一致。小屏 **1280×720／160dpi**、平板 **2560×1600／320dpi** 完成轻点、单指浏览、按住期间的双指缩放、密度栏及保存回读；平板另注入 `ACTION_CANCEL`，释放后图像保留最后有效浏览。两种布局导航后整份谱面 JSON 与各自首次保存基线一致，主画布倍率／横移仍为 1／0。正常退出读取工作区定位时间，小屏密度栏点按为约 17.453 秒，平板拖动为约 34.340 秒。
- NX809J **2688×1216／沿用 442dpi** 已通过相同隔离包验收：点按预览中 5 秒音符后对齐主画布判定线，单指上拖 120 像素跟手；双指按住时实际预览倍率更新至约 3.254，释放后不回跳。内容区／密度栏分别注入 `ACTION_CANCEL`，取消前后两块画布像素完全相同；密度栏约中点定位，正常退出回读为 **35.017677 秒**，弹层外点按未穿透改变最终位置。显式保存后整份谱面 JSON 与基线一致，主画布倍率／横移为 1／0，隔离 UID 无崩溃记录。本批使用 7 物件／3 Timing 的无声几何测试工程，未验收音频播放，也未以触点注入替代真人手感、系统手势冲突或热插拔测试。
- 证据、脚本、APK、配置备份及注释率 CSV 集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t09-20261113`。模拟器仅更新独立 `.next.t01probe`，原配置／INI 已逐字节恢复，13 份旧谱面哈希不变，尺寸／密度恢复 **1080×2400／420dpi**，模拟器继续在线；本批几何工程与注入 jar 已从模拟器移除。真机仅用新增 `.next.t09probe`，验收后该包与注入 jar 已清理，手机留在桌面，真实应用／旧验收包及 442dpi 均未改动。不改 `.gitignore`，不提交产物／缓存。本批只保留本地提交，不推送或合并。

本批 26 个新增／修改代码及 CMake 文件均满足逐文件 30% 注释率；`cloc --by-file` 结果如下（完整原始统计在上述集中目录）。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/CMakeLists.txt` | 146 | 340 | 30.04% |
| `Modules/Game/Canvas/include/canvas/PreviewCanvas.h` | 89 | 106 | 45.64% |
| `Modules/Game/Canvas/src/PreviewCanvas.cpp` | 360 | 829 | 30.28% |
| `Modules/Game/Canvas/src/presentation/desktop/DesktopCanvasFactory.cpp` | 12 | 26 | 31.58% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasFactory.cpp` | 13 | 28 | 31.71% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 423 | 585 | 41.96% |
| `Modules/Game/Graphic/include/graphic/mobile/MobileTouchInput.h` | 23 | 47 | 32.86% |
| `Modules/Game/Graphic/src/mobile/MobileTouchInput.cpp` | 40 | 92 | 30.30% |
| `Modules/Game/Graphic/tests/MobileTouchInputTest.cpp` | 27 | 61 | 30.68% |
| `Modules/Game/Logic/CMakeLists.txt` | 139 | 321 | 30.22% |
| `Modules/Game/Logic/include/logic/EditorEngine.h` | 350 | 229 | 60.45% |
| `Modules/Game/Logic/include/logic/ecs/system/NoteRenderSystem.h` | 122 | 244 | 33.33% |
| `Modules/Game/Logic/include/logic/session/PlaybackController.h` | 56 | 54 | 50.91% |
| `Modules/Game/Logic/include/logic/session/context/SessionContext.h` | 118 | 236 | 33.33% |
| `Modules/Game/Logic/src/logic/EditorEngine.cpp` | 1641 | 3794 | 30.19% |
| `Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem.cpp` | 609 | 1415 | 30.09% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 393 | 916 | 30.02% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Commands.cpp` | 1319 | 3064 | 30.09% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp` | 656 | 1513 | 30.24% |
| `Modules/Game/Logic/src/logic/session/PlaybackController.cpp` | 319 | 738 | 30.18% |
| `Modules/Game/Canvas/src/presentation/PreviewInteractionPresentation.h` | 16 | 33 | 32.65% |
| `Modules/Game/Canvas/src/presentation/mobile/MobilePreviewGesture.h` | 76 | 174 | 30.40% |
| `Modules/Game/Canvas/src/presentation/mobile/MobilePreviewInteractionPresentation.cpp` | 61 | 142 | 30.05% |
| `Modules/Game/Canvas/tests/MobilePreviewInteractionTest.cpp` | 181 | 421 | 30.07% |
| `Modules/Game/Logic/src/logic/session/PlaybackController_Preview.cpp` | 29 | 67 | 30.21% |
| `Modules/Game/Logic/src/logic/session/PreviewNavigationProjection.h` | 11 | 25 | 30.56% |

### T08 批注与数据表格（2026-11-13）

- 批注气泡／卡片点按或静止长按可打开常驻 Markdown 阅读菜单，保留同时间戳条目的前后切换、编辑和新增。触屏不再依赖 Hover／右键，也不把按下即视为编辑；桌面原入口保留。
- 批注表采用可滚动摘要列表与独立正文阅读区，点按选择、长按编辑，显式跳转／编辑／删除／撤销／重做。编辑使用稳定 ID 的正文副本，确认时检查谱面实例、目标存在、写入许可及可见的源正文冲突；只更新正文，保留原作者、时间、关联物件和目标领域。没有新增批注批量删除语义；原导出功能继续共用。
- Timing 表触屏点按按替换／追加／减选模式更新集合，滑动只浏览，不沿用鼠标范围拖选；时间和数值单元格只读，静止长按或“编辑参数”进入原参数窗。复制／剪切／删除／粘贴及历史按钮在高级折叠区外，沿用正式命令与一次历史。普通点弹窗与插值窗均独立于主时间线的显示状态。
- 触屏窗口使用外壳发布的可用区域，避开常驻右侧工具栏；等待快照的占位窗也使用相同几何。创建工具默认折叠、常用按钮按空间同行显示，避免高 DPI 下耗尽数据区；Timing 触屏列使用独立布局 ID 与字体单位首选宽度，不继承桌面过窄列宽。摘要文本仅在数据／语言／时间格式变化时重建，不在常态帧重复格式化全表。
- Linux 完整构建通过，最终相关 CTest **6/6**；完整套件 **198/199**，只有已登记的 `CanvasTouchNavigationTest` 失败，未修改该测试或声称修复。一次并行构建期间的 `AudioManagerTimelineIntegrationTest` 计时失败在无原生编译的完整复跑中通过。Android **x86_64／arm64-v8a** 均构建、签名校验通过；APK 中原生库与集中构建库按 ELF Build ID 匹配，AGP 去除调试信息后的字节哈希不要求相等。
- 小屏模拟器 **1280×720／160dpi**、平板尺寸 **2560×1600／320dpi** 与 NX809J **2688×1216／沿用 442dpi** 已验证批注列表与明确操作。小屏、真机完成关联物件批注正文编辑、空作者批注删除、一次撤销／重做及整谱保存回读；平板完成批注删除／历史。Timing 小屏、平板完成两行多选删除与精确历史；真机从隐藏时间线的独立表格长按打开参数窗，修改流速并一次撤销，保存回读精确恢复。最终布局复核与清理结果记录在集中证据目录。
- CPU 测试使用正式 ImGui、全局滚动仲裁、真实会话和命令历史，覆盖多指／Cancel、滑动不误选、空作者／物件关联、同时间戳多条、草稿取消、源正文变化拒绝覆盖、播放禁写、方案切换、主时间线隐藏后的实际模态及占位窗首次尺寸。设备操作使用 ADB 触点注入，不代表真人手感或所有系统手势场景；协作跨线程竞态仍由既有业务命令处理，不把 UI 缓存检查当成原子冲突合并。
- 软键盘出现时可先按系统返回隐藏键盘，再按显式保存／应用；返回不自动提交。自动避让活动输入与确认按钮属于 **T11**，本批未宣称完成。批注导出格式／SAF 与 Timing 剪贴板的全部设备组合未在本批重验。
- 产物、缓存、截图、完整谱面、备份与逐文件注释率 CSV 集中在 `/home/xiang/Android/mmm-smoke/main/evidence/t08-20261113` 及同一 `main` 工作树。模拟器仅更新独立 `.next.t01probe`，真机使用新增 `.next.t08probe`（MMM T08 Regression），不覆盖 `.next.dev` 或旧验收包；测试工程与配置均隔离，不提交 APK／缓存，不改 `.gitignore`。模拟器恢复原配置／INI（逐字节核对）及 **1080×2400／420dpi**，原有 13 份谱面字节不变，模拟器继续在线；真机新建的 `.next.t08probe` 验收后已移除，原应用与 442dpi 设置保持原样。本批保留本地提交，不自动推送或合并主线。

本批逐文件 `cloc --by-file` 结果如下；翻译纯数据不计入代码。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/CMakeLists.txt` | 141 | 327 | 30.13% |
| `Modules/Game/Canvas/include/canvas/AnnotationTableData.h` | 40 | 61 | 39.60% |
| `Modules/Game/Canvas/include/canvas/AnnotationTableWindow.h` | 38 | 47 | 44.71% |
| `Modules/Game/Canvas/src/AnnotationTableData.cpp` | 72 | 166 | 30.25% |
| `Modules/Game/Canvas/src/AnnotationTableWindow.cpp` | 263 | 597 | 30.58% |
| `Modules/Game/Canvas/src/Basic2DCanvas_AnnotationMenu.cpp` | 63 | 145 | 30.29% |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2024 | 4718 | 30.02% |
| `Modules/Game/Canvas/src/TimelineCanvas.cpp` | 579 | 1337 | 30.22% |
| `Modules/Game/Canvas/src/TimelineCanvas_Popups.cpp` | 1159 | 2694 | 30.08% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 43 | 69 | 38.39% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.h` | 77 | 102 | 43.02% |
| `Modules/Game/Canvas/src/presentation/desktop/DesktopCanvasFactory.cpp` | 9 | 20 | 31.03% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasFactory.cpp` | 10 | 22 | 31.25% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 219 | 505 | 30.25% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimelineInteractionPresentation.cpp` | 245 | 571 | 30.02% |
| `Modules/Game/Canvas/tests/MouseAnnotationMenuTest.cpp` | 168 | 389 | 30.16% |
| `Modules/Game/Canvas/tests/TimingTableMouseSelectionTest.cpp` | 218 | 496 | 30.53% |
| `Modules/Game/UI/include/ui/workspace/IWorkspacePresentation.h` | 54 | 65 | 45.38% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkspacePresentation.cpp` | 341 | 785 | 30.28% |
| `Modules/Game/UI/src/ui/workspace/WorkspacePresentation.cpp` | 57 | 132 | 30.16% |
| `Modules/Game/Canvas/src/presentation/AnnotationTablePresentation.h` | 12 | 24 | 33.33% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileAnnotationTablePresentation.cpp` | 112 | 260 | 30.11% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileAuxiliaryWindow.h` | 10 | 23 | 30.30% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileReadPress.h` | 32 | 74 | 30.19% |
| `Modules/Game/Canvas/tests/MobileAnnotationTableTest.cpp` | 162 | 378 | 30.00% |

### T07 本机收尾与接续（2026-11-13）

- 贝塞尔控制点采用扩大热区、保持抓取偏移的单指拖动；第二指、系统取消或图表隐藏恢复本笔原值。自由手绘沿用原 129 点与拟合算法，正常抬手才使旧拟合失效，取消保留旧拟合；外层保存段落才提交原有业务命令。
- 输出图支持单指横向平移、双指平移／带死区缩放，以及明确的缩小／全段／放大按钮；图内操作不滚动父页面，图外继续滚动。表单标签与输入在移动端分行，符号说明点按常驻，中英文新增 112 对字段／帮助翻译；未声称翻译全部底层解析错误。
- 移动解释器和私有状态头位于 `presentation/mobile`，桌面通过中立呈现接口保留原输入和布局，不改变插值模型、业务命令或拟合算法。
- Windows 完整构建、相关 CTest **7/7** 和最终完整 CTest **199/199** 通过，Android **x86_64** 最终 APK 构建通过。真实 ImGui CPU 回归覆盖两种缩放、控制点抓取／往返、手绘完整覆盖、取消与既有拟合保留、页面滚动仲裁及预览导航。格式化、差异检查及翻译参数契约检查通过。
- 手机 **1280×720／160dpi（英文）**、平板 **2560×1600／320dpi（中文）** 均完成控制点拖动、第二指与注入 `ACTION_CANCEL`、完整手绘拟合、输出图按钮／单指平移／双指缩放／全段恢复，以及保存回读和一次精确撤销／重做。逐份存档核对无关对象与字段不变，外层保存前磁盘谱面不变。测试结束恢复原谱面、配置和工作区，两台模拟器保持开启。本项未构建 ARM、未操作实体手机，注入不代替真人手感。
- 证据、脚本、APK 位于忽略目录 `build-android`，前缀 `t07-*`，不随 Git 迁移。下一项 **T08 批注与数据表格**：先核对已有长按入口，再补查看／编辑／删除、行选择与批量操作，不提前扩展 T09 预览窗口导航。该机器收尾时仅本地提交；本机现已从远端拉取到 `26a210f8`，T07 已随分支同步。

本批逐文件注释率如下，CSV 为 `build-android/evidence/t07-comments-final.csv`；翻译纯数据不计入代码。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/TimelineCanvas_Interpolation.cpp` | 297 | 690 | 30.09% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimelineInteractionPresentation.cpp` | 235 | 547 | 30.05% |
| `Modules/Game/Canvas/src/TimelineCanvas_FunctionEditor.cpp` | 216 | 503 | 30.04% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimingCurveEditor.h` | 123 | 282 | 30.37% |
| `Modules/Game/Canvas/tests/MobileTimingCurveInteractionTest.cpp` | 113 | 262 | 30.13% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.cpp` | 80 | 185 | 30.19% |
| `Modules/Game/Canvas/src/TimingInterpolationControls.cpp` | 66 | 154 | 30.00% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.h` | 71 | 94 | 43.03% |
| `Modules/Game/Canvas/include/canvas/TimingInterpolationControls.h` | 13 | 23 | 36.11% |
| `Modules/Game/Canvas/CMakeLists.txt` | 135 | 306 | 30.61% |

| 编号 | 优先级 | 状态 | 交互范围 | 下一步与完成标准 |
| --- | --- | --- | --- | --- |
| T01 | P1 | 已实现，核心验收通过 | 已有 Note／Hold 的局部调整 | 选中后提供适合手指的首尾把手，区分整体移动与端点修改；验证向前／向后调整、吸附、边界、取消和一次撤销。 |
| T02 | P1 | 正交约束已修正，本机三设备核心回归通过 | 折线节点与 Flick 修改 | 类型交替、接缝、父锚点、域边界与精确历史已回归；历史补验限制见本机记录。 |
| T03 | P1 | 已实现，三设备核心验收通过 | 采样锚点与偏移 | 左侧移动锚点，右侧调整触发偏移；零／正／负偏移、跨零、取消与精确历史已验证，显示策略修订见下文。 |
| T04 | P1 | 已实现，手机／平板模拟器验收通过 | 橡皮擦 | 绘制工具选择物件擦除；点按或连续扫过整物件，释放后一次提交，多指／系统取消保留之前历史。 |
| T05 | P1 | 已实现，手机／平板模拟器验收通过 | 调色笔刷与选区填色 | 点按／拖涂和配色橡皮抬手一次提交；独立触屏调色页显式填色／清色，修改调色盘不自动修改选区。 |
| T06 | P2 | 已实现，手机／平板模拟器验收通过 | Timing 段落局部修改 | 点选首标记后左右把手分别调整首尾，固定另一端；保留原标记整体移动，复用原插值、吸附和单次撤销命令。 |
| T07 | P2 | 已实现，手机／平板模拟器验收通过 | 高级曲线与预览图 | 扩大控制点热区、单指手绘与取消；输出图单指平移、双指缩放及明确缩小／全段／放大按钮；与页面滚动隔离。 |
| T08 | P2 | 已实现，核心验收通过 | 批注与数据表格 | 批注点按常驻阅读、长按编辑及明确删除／历史；Timing 点选、多选、批量操作与独立参数窗。范围及设备边界见本批记录。 |
| T09 | P2 | 已实现，三设备核心验收通过 | 预览区导航 | 轻点、单指浏览、双指缩放、密度定位及取消已回归；设备注入边界见本批记录。 |
| T10 | P2 | 核心完成 | 音频图表与资源面板 | 保留已有 EQ／行高／按钮布局成果；核对波形／频谱导航、资源长按操作与拖放替代入口，区分滑块精调和页面滚动。 |
| T11 | P2 | 核心已实现，模拟器／真机通过 | 数值与文本属性输入 | 支持便捷精调、明确确认／取消；手机软键盘不能遮住活动字段与提交按钮，焦点切换和返回不能误保存。 |
| T12 | P3 | 进行中：相机及 BPM 两机通过，布局模拟器通过／真机待验 | 其余辅助工具 | 逐项检查布局、BMS、测速等窗口中的修饰键、右键、双击和 Hover 唯一入口；先登记可复现缺口，再按功能独立适配。 |

### T01 当前实现与验收

- 已选独立 Note 使用本体细描边，Hold 首尾使用贴边短胶囊，移除圆圈与外伸连线。首把手保持尾时刻、尾把手保持起点，本体继续整体移动。原半径 22 逻辑像素的触摸热区完整保留，并扩展到可见胶囊附近。把手仅属于触控方案，不改变经典鼠标入口。
- 首尾通过公共拖动事务处理吸附、边界、取消和一次撤销／重做；修正草稿端点被统一跨域路径误当整体平移的问题。折线局部节点见下方 T02。
- 核心基线 Linux 完整构建、相关 CTest 8/8 和完整套件 196/196 通过；外观修订的完整构建和相关 CTest 5/5 通过。`CanvasTouchNavigationTest` 在核心验收中曾失败、复跑通过，不将其视为已修复。Android 双 ABI 构建通过。NX809J、小屏 1280×720／160dpi 与平板 2560×1600／320dpi 均验证首尾把手和保存回读；实体额外验证 Note／Hold 整体移动与一次撤销重做，三种尺寸均完成单指阳性对照和双指接管取消。
- 模拟器宿主退出后改用软件图形后端完成回归，原尺寸和密度已恢复并保持开启。真人手感、系统 `ACTION_CANCEL`、场景切换及完整双向边界尚未独立设备验收；边界与事务由 CPU 回归覆盖，不将注入结果当作真人体验。这些补验项继续保留，后续按表推进。
- 胶囊外观修订已在 NX809J 与模拟器验收：Note 细描边、Hold 胶囊及旧热区均保留，首尾拖动保存回读通过。模拟器 GUI 宿主不稳定，改用无窗口软件后端；存储不足时仅对独立验收包执行保留数据重装，谱面和配置哈希一致。
- 本轮产物、缓存及证据集中在 `/home/xiang/Android/mmm-smoke/main`；签名不同的已有 `.next.drawprobe` 保留，验收改用独立 `.next.t01probe`，不覆盖真实应用。

### T02 当前实现与验收

以下为上一机器的修正记录；本次 Windows 与实体机补验见后面的“本机接回”小节。

- **撤回上一轮“核心验收通过”结论。** 原测试夹具含普通 Note 子项和连续 Hold，不符合 `mmm/note/Polyline.h` 的合法折线契约；自由移动这些点会产生斜线。节点数量变化、文件能保存和历史回放成功均不能替代结构合法性验证。旧现场与工程已备份在 `/home/xiang/Android/mmm-smoke/main/evidence/t02-legality-20261113`。
- 合法原生折线只能由 Hold／Flick 交替组成：父起点等于首段；Hold 尾时刻与后 Flick 时刻一致；Flick 终轨与后 Hold 起轨一致；每段保持在所属玩家或草稿域。移除自由 Note 锚点和等分插点入口；新增转折继续使用原有绘制／续写状态机，不能用同类段或零跨度段伪造节点。
- 节点触控目标继续使用细描边和扩大热区，尾端使用贴边胶囊。Hold 节点按桌面规则横向调整前一 Flick 和后缀轨道；Flick 节点纵向调整前一 Hold 和后缀时间。触控起笔先验证合法结构及稳定身份；旧畸形数据保持可见但不自动改写为另一种几何。节点删除沿用橡皮擦断开路径，首节点整条删除使用原有通用入口。
- 回归改用合法五段路径，逐步检查预览、提交、取消、撤销／重做及删除余段的完整连接；额外拒绝普通 Note、连续同类段、时间缺口、轨道断口和父锚点错位。桌面 DrawTool 的真实输入序列生成 Hold→Flick→Hold 基线；原生 MMM 身份往返测试同样使用合法交替段，不再把畸形结构保留当作正确行为。
- 已在静音隔离桌面 UI 实际绘制并保存 `DesktopDrawnBaseline.mmm`，再横拖节点保存 `DesktopNodeDrag.mmm`；独立脚本均通过结构校验，截图与存档在上述集中目录。手机必须复用这份真实桌面基线，每次修改保存后都校验类型交替、接缝、父锚点与域边界，不能仅凭画面改变判定通过。小屏 1280×720 与平板 2560×1600 分阶段完成模拟器复验：Hold 节点横移、Flick 节点纵移、撤销重做及双指接管均保存回读；每份折线经独立几何脚本校验。删除明确断为两条独立 Hold，精确核对数量、类型与各自几何，撤销恢复三段、重做再断开。最新包从原尾端续写仍保持单根，三段变为合法 Hold／Flick／Hold／Flick，新 Flick 跨度非零；没有以新建独立物件冒充续写。
- 同轮发现桌面更新提示与启动同意框争抢模态并循环播放提示音。更新弹层改为排队消费一次打开事件，取消或外部关闭后不因持久终态自动重开；手动再次检查仍能打开。新增真实 ImGui 回归覆盖排队与重新检查，所有桌面诊断使用无物理输出的磁盘音频驱动，避免再次打扰用户。
- Linux 完整构建通过，相关 CTest **8/8**；完整套件 **196/197**，仍只有交接已登记的 `CanvasTouchNavigationTest` 间歇失败，本轮未修改该测试或导航行为。Android 双 ABI 构建通过。19 个本轮修改的代码文件逐文件注释率均达到 30%，CSV 记录每个路径、注释行数、代码行数及比例。
- 模拟器空间不足时，仅移除已完整备份且逐文件 SHA 与 APK 一致的 81 个默认皮肤缓存文件，安装后资源自动重提取；工程存档哈希保留。配置在备份后仍有正常 UI 改动，未声称安装前后字节完全一致。最终模拟器恢复 1080×2400／420dpi 并保持在线，前台显示合法路径。详见集中目录 `acceptance-summary.json` 和 `final-canvas-selected.png`。
- 产物、缓存、完整谱面备份、静音录音和逐文件注释率报告继续集中存放。测试只更新 `.next.t01probe`，不覆盖真实 `.next.dev`。用户明确要求本轮仅使用模拟器，**实体手机修复版未安装、未验**；真人手感和真实系统 `ACTION_CANCEL` 仍需单独验收。

### 本机接回后的 T01／T02 回归（2026-11-13）

- `809cda5d` 完整 Windows 构建成功，完整 CTest **198/198**；Android x86_64／arm64-v8a 打包成功。Linux 既有 `CanvasTouchNavigationTest` 间歇失败仍保留登记，Windows 通过不代表已修复 Linux 行为。
- 小屏模拟器 **1280×720／160dpi**、平板 **2560×1600／320dpi** 与 NX809J **2688×1216／沿用 442dpi** 均回归独立 Hold 首尾调整、折线 Hold 节点横移和 Flick 节点纵移；逐次保存回读核对五段交替类型、接缝、父锚点、轨道域与旁边长条不变。撤销／重做精确恢复整个存档，双指取消不清空上一笔重做。
- 本机没有上一机器的 `DesktopDrawnBaseline.mmm` 原始产物；没有将手填自由节点当作替代。当前基线由实际触控绘制生产路径生成合法 Hold／Flick／Hold／Flick／Hold，再独立校验几何；生产桌面 DrawTool 输入回归也通过。隔离桌面 UI 的额外手绘补验停在隐私授权页，未代用户选择，不能声称完成了该项桌面实操。
- NX809J 使用新包 `top.xiang233.musicmapmaker.next.t03probe`（MMM Touch T03），未覆盖已有真实应用和旧验收包。三指存在系统接管限制，重做使用长按菜单。注入结果不等同于真人手感；此前登记的场景切换、完整双向边界等设备补验仍保留。
- 本机日志、APK、截图及逐次存档在项目忽略目录 `build-android/evidence`、`build-android/apk`；回归记录为 `t03-return-*`。这些产物不随 Git 迁移。原机器 `/home/xiang/Android/mmm-smoke/main` 下的记录继续仅作为历史证据。

### T03 当前实现与验收

- 采样交互提交为 `843c9e84`。已选自动采样的锚点与实际触发点分别使用左／右贴边胶囊和扩大触摸热区，零偏移时也可分别抓取。左侧沿用原锚点移动事务，保留有符号偏移；右侧只改 `offset_ms`，不改锚点、资源、音量、轨道或其他对象。
- 呈现仅消费已有快照并发布原 `SampleAnchor`／`SampleOffset` 命令；不扩大桌面拾取框，不改值语义、业务算法或公共头依赖。按住说明复用现有中英文翻译。详细 Hover 检视由平台呈现接口决定：触屏使用本地短说明，经典桌面和纯鼠标保留原详情，不散布平台宏或通过清空命中身份抑制提示。
- Windows 完整构建、相关回归和最终完整 CTest **198/198** 通过；最后短说明底色样式补充后，完整构建与相关回归 **4/4** 通过。Android x86_64／arm64-v8a 均重新打包成功。早期整套分别遇到 `VisualResourceRenameTest` 工程创建失败及既有 `SkinPackageServiceTest` 根布局导入失败；独立复跑和最终完整套件均通过，未修改这些模块，不将偶发失败记为已修复。Linux 原有导航测试登记继续保留。
- 手机、平板模拟器与 NX809J 均完成锚点移动、正／负偏移、跨零调整、点按不建历史、一次撤销／重做、双指接管及注入 `ACTION_CANCEL`；逐次保存回读检查整个谱面及采样资源、音量、轨道不变，最终撤回到测试基线。双指接管允许继续导航，边缘拖动也可触发既有自动平移；脚本重新核对投影后再验下一手势，不把视野变化误判为谱面改动。真人手感和未覆盖的场景切换仍需另验。
- 实体机核心验收使用 `.next.t03probe`，结束后测试包已退出；用户已拿走手机，之后不再执行实体机操作。最后的 Hover 显示隔离只修改呈现策略，不改核心输入；该修订及主题底色补充已完成手机／平板模拟器复验，未重新安装实体机。中英文按住说明均通过截图复核，原桌面详情框不再遮挡画布。
- 当前本机日志及核心三设备证据为 `build-android/evidence/t03-sample-*`；呈现隔离证据为 `t03-feedback-*`，最终底色和截图证据为 `t03-style-*`。测试工程、APK 和输入脚本均在忽略目录，不提交为源码资源。下一项从 **T04 橡皮擦** 开始。

本批修改代码的逐文件 `cloc --by-file` 结果如下；CSV 在 `build-android/evidence/t03-comments-final.csv`，没有合并同名头／实现文件的比例。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Interaction.cpp` | 2024 | 4716 | 30.03% |
| `Modules/Game/Logic/tests/SampleRenderSystemTest.cpp` | 406 | 946 | 30.03% |
| `Modules/Game/Canvas/tests/MobileCanvasSelectionTest.cpp` | 277 | 644 | 30.08% |
| `Modules/Game/Logic/src/logic/ecs/system/render/SampleRenderSystem.cpp` | 262 | 611 | 30.01% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 183 | 424 | 30.15% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileNoteHandles.h` | 69 | 159 | 30.26% |
| `Modules/Game/Canvas/src/presentation/CanvasInteractionPresentation.h` | 41 | 68 | 37.61% |

### 双指平移的缩放防抖（2026-11-13）

- 修正导航开始后直接把所有指距变化映射到缩放的问题。主画布与 Timing 共用的移动端私有导航器增加独立缩放死区：取 15 逻辑像素（按内容缩放）与初始指距 8% 的较大值。死区内保持起始倍率，仅超出部分连续缩放；同一手势仍可从平移转为明确捏合，回到初始指距区间精确恢复起始倍率。桌面输入路径不变，不增加等待、锁或分配。
- Windows 完整构建及完整 CTest **198/198** 通过，最终相关回归 **4/4**；Android x86_64／arm64-v8a 均打包成功。CPU 回归覆盖两种密度、非方形视口、上下往返、两指交错更新、死区边界连续性、同一手势放大／缩小、释放倍率及不误撤销。Linux 原导航测试的历史登记继续保留。
- 小屏 **1280×720／160dpi** 与平板 **2560×1600／320dpi** 在主画布和 Timing 分别注入带反向微小开合的上下平移；退出保存回读后倍率仍精确为 **0.5／1.0**，视野时间均确实移动。再分别在主画布明确张开、Timing 明确合拢，最终倍率为 **0.5567996／1.1136004**，与预期 **0.5568／1.1136** 一致；各步保存的整个谱面均与基线相同。模拟器测试配置已恢复并保持开启。本轮按用户先前要求不再操作实体机，注入验收也不代替真人手感。
- 证据、脚本和 APK 均在忽略目录 `build-android`，本轮前缀为 `pinch-pan-*`。逐文件注释率 CSV 为 `build-android/evidence/pinch-pan-comments-final.csv`：`MobileCanvasNavigation.h` **51 注释／116 代码，30.54%**；`MobileCanvasNavigationTest.cpp` **79 注释／182 代码，30.27%**。下一项仍为 **T04 橡皮擦**。

### T04 触屏物件擦除（2026-11-13）

- 入口为绘制工具下的“物件擦除／Erase Objects”，与原有配色橡皮分开。点按擦一个完整物件，拖动连续扫过并描边预览，正常抬手才提交一个删除批次；往返经过相同目标只记录一次。折线整条删除，局部断开继续使用已有节点菜单，不改变原折线结构规则。
- 第二指接管、系统取消、切换模式／谱面、播放或外部弹层接管均丢弃本地候选，不通过 Undo 假装取消，因此上一笔撤销／重做与未擦到的选区不受影响。空批次不回退当前选择或悬浮，逻辑消费时复核谱面实例、带代数实体、对象领域与编辑权限；混合音符／采样复用现有复合动作，一次历史完整恢复。
- 移动解释器及候选、命中与预览私有头保持在移动目录；公共头仅新增中立的显式删除值载荷。桌面默认删除与右键橡皮路径不变。连续帧只读已有可见命中快照，起笔一次预留容量，不扫描 ECS、不加锁或等待；笔画中协作新增目标突破容量时安全取消整笔，不静默部分删除。抬手后同步撤去普通悬停反馈。

- Windows 完整构建和完整 CTest **198/198** 通过；收尾的抬手悬浮反馈修订重新构建并通过相关回归 **5/5**。Android **x86_64** 最终 APK 打包成功；本项未构建 ARM、未操作实体手机，不将模拟器注入视为真人手感验收。CPU 回归另覆盖快速释放、往返去重、同号跨领域目标、长按、待定轻点与活动笔画切谱、播放拒绝、空请求及桌面回退；既有 Linux 导航测试登记保持不变。
- 小屏 **1280×720／160dpi** 与平板 **2560×1600／320dpi** 完成中英文入口、点按擦除、空白无操作、第二指接管、注入 `ACTION_CANCEL`、混合扫过独立 Hold／合法五段折线／音频采样及一次撤销／重做。逐次保存回读整个存档，元数据及无关字段保持原样；按住时截图确认物件仍在且有待删轮廓，正常抬手后才统一删除。取消分别验证保留已应用上一笔和上一笔重做，最终通过撤销恢复原测试谱面。
- 本机 APK、触点脚本、截图、日志及完整存档在忽略目录 `build-android`，前缀 `t04-*`，不随源码提交迁移。测试完成后恢复模拟器原配置／工作区并保持两个模拟器开启。下一项为 **T05 调色笔刷与选区填色**。

本批代码逐文件注释率如下，CSV 为 `build-android/evidence/t04-comments-final.csv`；翻译字典为纯数据，不计入代码统计。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Logic/src/logic/session/ActionController_Editing.cpp` | 1422 | 3317 | 30.01% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession.cpp` | 391 | 909 | 30.08% |
| `Modules/Game/Canvas/tests/MobileCanvasDrawingTest.cpp` | 325 | 758 | 30.01% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 415 | 576 | 41.88% |
| `Modules/Game/UI/src/ui/interaction/EditorInteraction.cpp` | 246 | 500 | 32.98% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 204 | 473 | 30.13% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkbench.cpp` | 155 | 361 | 30.04% |
| `Modules/Game/UI/include/ui/interaction/EditorInteraction.h` | 153 | 181 | 45.81% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileObjectEraser.h` | 73 | 167 | 30.42% |
| `Modules/Game/Common/include/common/LogicCommandMutationClassification.h` | 54 | 125 | 30.17% |
| `Modules/Game/Common/include/common/ObjectDeletion.h` | 9 | 17 | 34.62% |

### 已完成，不要重复实现

| 范围 | 已有能力 | 基线 |
| --- | --- | --- |
| 画布导航与历史 | 主画布双指导航、可用设备的多指历史轻触；NX809J 可通过菜单重做 | 当前分支；设备限制见下文 |
| 选择 | 点选、替换／追加／减选、多次框选；轻点不建空移动历史，取消保留重做 | `df76335b`、`df7fd397` |
| 单键／长条绘制 | 点按单键、固定轨道双向拖动长条 | `23e815fb` |
| 折线 | 新绘制、四类来源续写、事务取消及撤销／重做 | `5a2fe815` |
| 主画布菜单 | 长按复制／粘贴／剪切／删除／历史操作等，已有重叠层选择入口 | 当前分支移动呈现实现 |
| Timing | 点按建点、拖动建段、选择移动、长按菜单、吸附粘贴、多指导航与取消 | `caa2e82e`；验收基线见下文 |

### 每项验收与实现约束

- 移动布局、触点解释器及其私有头保持平台隔离。公开头只依赖中立接口，CMake／工厂选择实现，禁止在桌面代码各处散布 Android 宏；模型、剪贴板、撤销和联机继续共用业务命令。
- 不改热路径性能契约：本地反馈逐次立即更新，不增加固定时长等待、逐帧分配／文件访问或跨会话长锁。手指释放立即提交最终状态；第二指、系统 Cancel、切换谱面、播放及外部弹层接管不能提交旧事务。
- 主题圆角、间距和颜色使用设置；触摸命中区与可见图形可以不同，不能靠放大所有控件挤占画布。文字和说明走翻译，不将 Hover 当作唯一说明入口。
- 每项覆盖小屏手机与平板模拟器；实体机可用时也必须验收。新机器实体机不可用时，明确记录未完成，不能把模拟器结果写成实机通过；测试后按用户要求保留模拟器开启。
- 验证完整构建和相关 CTest；范围不明确时运行完整套件。核心手势覆盖点按／抖动／双向拖动／越界／多指取消／场景切换／一次撤销重做，并保存回读核对对象、时间、类型与历史边界。
- 格式化修改的 C++／CMake，按 `cloc --by-file` 检查每个修改代码文件至少 30% 注释率；实际实现附近解释状态转换与边界，不能堆无意义注释。只改文档无需重跑构建。
- 已知 Linux `CanvasTouchNavigationTest` 的最终缩放采纳问题单独登记，Windows 197/197 不能替代 Linux 验证；不要在无授权的交互批次顺便扩大修复范围。


## 历史验收基线与换机注意事项

- 此前 Timing 业务基线 `caa2e82e`（前序 `5a2fe815`；不代表当前 T02 修订验收）：Windows 完整构建成功，CTest **197/197 通过**；x86_64 与 arm64 Android 打包成功。手机模拟器、平板模拟器及 NX809J 独立验收包均完成 Timing 核心编辑回归，保存回读核对创建、粘贴、移动、撤销／重做与多指取消。
- 此前音频集成测试和皮肤包测试各出现一次偶发失败，独立复跑及最终完整套件均通过；未删测试或改动这两个模块。新机器仍需实际运行，不能沿用本机结果宣称新环境通过。
- NX809J 第三指可能触发系统 Cancel，不能把它误判为重做；该设备使用长按菜单重做。ADB 注入通过不替代真人手指手感验收。
- 历史验收使用手机模拟器 1280×720／160dpi、平板模拟器 2560×1600／320dpi；最新 T02 复验后已恢复 1080×2400／420dpi。上述为验收结束时状态，新机器检查实际尺寸及密度。实体机沿用原有密度，不为截图方便修改全局设置。
- 早期验收使用独立 `.next.drawprobe`；本轮改用 `top.xiang233.musicmapmaker.next.t01probe`。不得覆盖、卸载原 `.next.dev`、`.next.touchtest`、旧 `.next.drawprobe` 及其用户数据；换机先核对包名和签名。本机已重新获准使用实体机，测试使用另建的 `.next.t03probe`；后续按实际授权与设备状态复核。用户已更新要求：之后由当前智能体直接使用 `android-automation` skill 操作手机，不再将手机步骤交给另一智能体；操作前仍须核对设备与当前前台状态。
- 本机 32GB 内存同时运行两个模拟器时，Windows 和 Android 原生构建顺序进行、使用 2 个编译任务；完整 CTest／重音频测试不与原生编译并发。新机器根据实际资源调整，不在编译过程中修改源码。
- Windows 入口：`cmake --build build --parallel 2`、`ctest --test-dir build --output-on-failure`；Android 入口：`scripts/android/build-windows.ps1 -Abi x86_64 -Jobs 2` 或 `-Abi arm64-v8a -Jobs 2`，均在项目根执行。
- Linux 构建从项目根执行：`MMM_ANDROID_WORK_DIR=/home/xiang/Android/mmm-smoke/main MMM_ANDROID_ABIS="arm64-v8a x86_64" bash scripts/android/build.sh`；换机可选择另一处集中工作目录，显式设置 `MMM_ANDROID_WORK_DIR`，不要散落到源码目录。Windows 入口使用其专用工作目录。
- 默认 APK 输出会被后续构建覆盖，安装前核对包名、ABI、签名及摘要。本轮标准 APK 为 `/home/xiang/Android/mmm-smoke/main/apk/MusicMapMaker-Next-android-debug.apk`，SHA256 为 `79799886d38137246aadeb05c6dd169eb4a8c04594af708edb28ee76db87743f`；独立 x86_64 验收包摘要与签名见 `evidence/t02-legality-20261113/acceptance-summary.json`，不能将标准包当成已安装的验收包。
- 数据回读注意：`.mmm` Timing 时间戳单位是毫秒，插值 `duration` 单位是秒；吸附阈值外保留实际时间，不能强行断言所有触点都落在固定分拍。

### T05 触屏调色与选区填色（2026-11-13）

- 工作台“笔刷／橡皮”分别提供点按和连续拖涂、清除整物件配色。沿途目标描边预览，正常抬手形成一个批次；往返去重，折线按根物件处理，采样始终忽略。起笔锁定颜色与谱面身份；空白、同色重复、第二指、系统取消、播放、切谱及弹层接管不制造历史，不借助 Undo 取消本笔，保留原来的重做。
- “配色”进入独立移动布局：具名槽位、图形色相／明暗／透明度，以及“应用到选中／清除选区配色”明确按钮。取色拖动只调整画笔；点外收起不向画布补涂。方案、导入导出和分拍线管理通过原高级入口保留，本项不宣称重做其全部布局。选区填色沿用原有类型权限，普通物件不额外要求专业模式；颜色工具仍遵守原专业模式权限。
- 移动手势与候选私有头位于移动目录，调色布局另设实现文件；公共载荷只有颜色、身份和目标值。桌面原有单目标染色与自动应用入口保留。起笔预留本笔容量，连续帧不扫描 ECS、不加锁／等待或分配；发布空闲颜色笔的固定大小颜色表，保持几何画笔非活动。修复下拉列表的释放保护误关父调色窗，并正确配对反馈 Combo 的样式栈。
- Windows 最终完整构建及 CTest **198/198** 通过，相关回归 **6/6**。新增真实会话命令到快照发布的连接验证，另覆盖帧合并、去重、根／子身份、同号音频、无变化保留重做、普通选区权限与桌面阳性对照。过程中 `SkinPackageServiceTest` 根目录导入与 `AudioManagerTimelineIntegrationTest` 共享拉伸器计时曾失败，均在原环境独立复跑通过；未修改其模块，也未据此声称修复间歇问题。最终日志为 `t05-host-verified.log`、`t05-related-verified.log`、`t05-suite-verified.log`。
- Android **x86_64** 最终包构建成功。手机 **1280×720／160dpi（英文）**、平板 **2560×1600／320dpi（中文）** 均通过槽位切换、三种取色拖动、点外防穿透、点涂／往返扫涂、同色无操作、多指与 `ACTION_CANCEL`、配色橡皮和明确选区填色／清色。每步保存回读完整谱面，未命中音符、采样及其他字段保持原值；按住截图确认仅描边预览，整笔和选区操作均一次撤销／重做精确恢复。最终用撤销恢复原谱面，重启前恢复原配置／工作区，两台模拟器保持开启。本项未构建 ARM、未操作实体机，触点注入不代替真人手感。
- APK、输入脚本、截图和完整存档位于忽略目录 `build-android`，本批证据前缀 `t05-*`；不会随源码提交迁移。下一项为 **T06 Timing 段落局部修改**。

本批代码逐文件注释率如下，CSV 为 `build-android/evidence/t05-comments-final.csv`；翻译字典为纯数据，不计入代码统计。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/UI/src/ui/imgui/manager/ToolbarView.cpp` | 1990 | 4631 | 30.06% |
| `Modules/Game/Logic/src/logic/session/ActionController_Editing.cpp` | 1444 | 3367 | 30.01% |
| `Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp` | 656 | 1518 | 30.17% |
| `Modules/Game/Canvas/tests/MobileCanvasDrawingTest.cpp` | 447 | 1037 | 30.12% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkspacePresentation.cpp` | 337 | 780 | 30.17% |
| `Modules/Game/Common/include/common/LogicCommands.h` | 418 | 580 | 41.88% |
| `Modules/Game/UI/CMakeLists.txt` | 218 | 497 | 30.49% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileCanvasInteractionPresentation.cpp` | 217 | 496 | 30.43% |
| `Modules/Game/UI/src/ui/workspace/MobileWorkbench.cpp` | 157 | 364 | 30.13% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileColorStroke.h` | 78 | 180 | 30.23% |
| `Modules/Game/UI/include/ui/imgui/manager/ToolbarView.h` | 201 | 176 | 53.32% |
| `Modules/Game/UI/src/ui/workspace/MobilePaletteControls.cpp` | 50 | 105 | 32.26% |
| `Modules/Game/Common/include/common/NotePaletteStroke.h` | 7 | 14 | 33.33% |

### T06 Timing 段落首尾调整（2026-11-13）

- 选择工具先点选插值段落的首标记，左侧细把手调整起点、右侧细把手调整末端，始终固定另一端。把手横向错开，短段也可区分首尾；原标记中央继续整体移动，普通时间点不显示段落把手。没有新增正文区域选取规则或修改普通点业务。
- 起笔锁定完整插值定义，拖动立即显示本地范围边框，原曲线保留为未提交参考；抬手沿用原 Timing 吸附配置，再提交一条原生更新命令。起终值、曲线、控制点、输出密度和拍位设置不被重置；时长至少 1 ms，跨越另一端时夹取而不互换端点。无移动、返回原点及同效果范围冲突不产生空历史；多指、系统 Cancel、切谱／实例、播放、工具或已发布源定义变化丢弃本笔，不借 Undo 取消。
- 移动事务及私有头位于 `presentation/mobile`，公共呈现接口只有中立投影、提交和附加绘制入口。桌面默认附加层为空，原鼠标移动路径不变。低频起笔复制两份定义以锁定共享不可变缓存；连续帧不复制所有权、不重建曲线或拍点容器、不扫描 ECS、不增加锁／等待。原吸附助手的临时容器仅在释放时使用。
- Windows 最终完整构建、相关回归 **7/7**、最终完整 CTest **198/198** 通过。真实触点输入覆盖首尾双向调整、快速 Up、最小时长、无操作、同效果冲突、多指／系统取消、协作改写及场景失效；命令经原动作栈验证一次撤销／重做。另有经典桌面与触屏标记整体移动阳性对照。首次完整套件的 `SkinPackageServiceTest` 覆盖导入用例失败，独立复跑及最终完整套件通过；未修改皮肤模块，不宣称修复间歇问题。既有 Linux 导航测试登记继续保留。
- Android **x86_64** 最终包构建成功；手机 **1280×720／160dpi（英文）**、平板 **2560×1600／320dpi（中文）** 均验证首尾双向拖动、轻点／长按把手无操作、返回原点、第二指和 `ACTION_CANCEL`、同效果冲突拒绝、原标记整体移动及普通点移动。按住截图和落盘文件确认释放前只有本地预览；逐步保存回读完整谱面，目标以外字段不变，一次撤销／重做精确恢复。沿用两台各自吸附配置：手机强制拍线吸附，平板阈值外保留实际时间。最后撤销恢复测试基线，恢复原谱面／配置／工作区并保持模拟器开启。本项未构建 ARM、未操作实体机，触点注入不代替真人手感。
- 本批日志、APK、脚本、截图和完整存档在忽略目录 `build-android`，前缀 `t06-*`；最终日志为 `t06-host-final.log`、`t06-related.log`、`t06-full-final.log`、`t06-native-final.log`。这些证据不随源码迁移。下一项为 **T07 高级曲线与预览图**，需另行适配曲线控制点及图表／页面滚动仲裁。

本批代码逐文件注释率如下，CSV 为 `build-android/evidence/t06-comments-final.csv`；翻译字典为纯数据，不计入代码统计。

| 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/TimelineCanvas_Interpolation.cpp` | 284 | 658 | 30.15% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimelineInteractionPresentation.cpp` | 193 | 449 | 30.06% |
| `Modules/Game/Canvas/tests/MobileTimingInteractionTest.cpp` | 182 | 423 | 30.08% |
| `Modules/Game/Canvas/src/presentation/mobile/MobileTimingRange.h` | 98 | 212 | 31.61% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.cpp` | 70 | 163 | 30.04% |
| `Modules/Game/Canvas/src/presentation/TimelineInteractionPresentation.h` | 48 | 68 | 41.38% |
