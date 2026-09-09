# RM

节奏大师游玩资源迁移皮肤，显示名称为 **RM**。

重新启动本次构建的软件，在皮肤设置中选择 RM。若项目已指定自定义配色方案，选择跟随皮肤配色可恢复蓝色单键与绿色长条的完整颜色。

## 素材来源与处理

素材源：`EX_Rhythm_Master_VI(1).msp`。
原包元数据：`EX Rhythm Master VI 250314` / `Stable 250922`。
原包作者：dreamcat、Acc@9961、Sam_0324。
SHA-256：`af3337bbcae69da0e01ec2a0ee089600434f18cfc01809e5a3c2eae01379542d`。

映射依据为外层 `rmslideEXF.lua` 的实际游玩资源引用；没有使用或执行内嵌的 `rm_editor_20240812.zip`。

| RM 资源 | 原包游玩素材 | 处理 |
| --- | --- | --- |
| `note.png` / `holdhead.png` | `5note3.png` / `5notel3.png` | 同一张模型图集中的蓝色与绿色玻璃按键，均为 256 × 112 |
| `node.png` / `holdend.png` | `notefbody0.png` / `tail.png` | 同图集中的圆节点与矩形尾端 |
| `holdbodyvertical.png` / `holdbodyhorizontal.png` | `notebody.png` | 同图集中的纵横光带，仅裁取中段以排除端帽 |
| `arrowleft.png` / `arrowright.png` | `tailsl.png` / `tailsr.png` | 同图集中的无短杆三角头，由软件连接体延伸至内部 |
| `panel/track.png` | `trackbg.png` | 提取内部无边框区域，适配软件平行轨道 |
| `panel/judgearea.png` | `rym-key5k.png` | 提取中央单键并校正透视，适配任意轨道数 |
| `effect/note/1..17.png` | `rym-hit-1..17.png` | 保留原帧序，转换黑底为透明 |
| `effect/flick/1..18.png` | `rym-hitl-1..18.png` | 保留原帧序，转换黑底为透明 |
| `audio/note.ogg` / `audio/flick.ogg` | `click.ogg` / `flick.ogg` | 原文件直接复用 |

全部八个静态音符组件在一张图片中统一生成，以相同倍率裁切导出，不分别调整颜色、辉光或缩放。
原始模型图集存档为 `resources/atlas/rm-atlas-source.png`；去黑底图集为 `rm-atlas-rgba.png`；`regions.json` 记录原图哈希、裁切坐标和导出尺寸。
在仓库根目录执行 `bash "$PWD/assets/skins/rm/resources/atlas/extract.sh"` 可重建全部八张贴图。
发光连接组件使用最大 RGB 分量作为透明度；蓝绿实体按键内部保持不透明以遮挡下层连接体。所有组件均反预乘 RGB，保留光晕亮度；连接体的长度和端点位置保持原有中心连接规则。
特效以最大 RGB 分量作为透明度，并对 RGB 反预乘，适配软件 Alpha 混合；不是简单把黑色背景删除后重复乘暗光晕。
公共字体、软件光标、界面音效和着色器复用同级 `mmm-default`，不执行原包游戏脚本。

## 软件适配

新增可选资源键 `assets.note.holdhead`，普通长条、单滑键和折线起点均可使用独立的绿色头部；主画布和预览采用相同映射。
未声明该资源的旧皮肤继续使用 `note.note`，已有纹理 ID 保持不变。
构建时随其他内置皮肤同步到配置目录，不修改用户选中的皮肤或项目配色。
所有 PNG 和 OGG 使用仓库已有 Git LFS 规则。

## 验证（2026-09-09）

- `cmake --build build` 成功，PNG 透明边缘和特效已做贴图预览检查。
- `SkinThemeBindingTest` 通过：实际加载 RM，检查独立头部、白色乘色、资源路径及 17/18 帧序列。
- `ExtremeSvHoldRenderTest` 通过：独立头部 UV 选择及旧图集回退均生成正确头部。
- `DefaultConfigAssetSyncTest` 通过：RM 入口、嵌套资源和增量同步正确，用户文件保留。
- 完整 CTest 为 **132/133**；`MarkdownRendererTest` 触发未上传纹理的 ImGui 断言，复跑仍失败；本批未修改该测试或 Markdown 渲染代码。
- `git diff --check` 通过，最终生成贴图与本机已安装资源逐字节一致。
- 使用隔离 `MMM_CONFIG_ROOT` 和临时示例谱面进行实际 Vulkan 画面验收，确认蓝色单键、绿色长条／滑键／折线头、节点、双向箭头及预览区一致；截图保存在 `build/test_output/rm_visual/RM-connected.png`。
- 图集透明度修正后，实际画面确认箭头、节点和尾端的连接无暗色断口，三项相关 CTest 均通过。
- 实际验收同时补齐了可选头部 UV 向逻辑线程的发布；修正后上述三项相关 CTest 再次通过。

本批修改文件按 `cloc --by-file` 统计。低于 30% 的既有文件在表中明确标记；此次只补充适配位置的说明，未扩展为整文件注释重写。

| 文件（仓库相对路径） | 注释行 | 代码行 | 注释率 | 达到 30% |
| --- | ---: | ---: | ---: | --- |
| `Modules/Config/tests/SkinThemeBindingTest.cpp` | 67 | 470 | 12.48% | 否 |
| `Modules/Game/Canvas/src/Basic2DCanvas_Rendering.cpp` | 42 | 803 | 4.97% | 否 |
| `Modules/Game/Canvas/src/PreviewCanvas.cpp` | 71 | 735 | 8.81% | 否 |
| `Modules/Game/Common/include/common/render/RenderSnapshot.h` | 192 | 499 | 27.79% | 否 |
| `Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem_Notes_Polyline.cpp` | 252 | 587 | 30.04% | 是 |
| `Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem_Notes_Types.cpp` | 77 | 176 | 30.43% | 是 |
| `Modules/Game/Logic/tests/ExtremeSvHoldRenderTest.cpp` | 73 | 170 | 30.04% | 是 |
| `assets/skins/rm/skin.lua` | 56 | 126 | 30.77% | 是 |
| `cmake/SyncDefaultConfigAssets.cmake` | 23 | 54 | 29.87% | 否 |
| `cmake/tests/DefaultConfigAssetSyncTest.cmake` | 39 | 104 | 27.27% | 否 |

图集导出脚本 `resources/atlas/extract.sh`：注释 17 行、代码 21 行，注释率 44.74%。
