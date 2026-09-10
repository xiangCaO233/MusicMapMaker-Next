# RM(old)

旧版 RM 游玩素材迁移皮肤，入口目录为 `rm-old`，显示名称为 `RM(old)`，与新版 RM 独立分发。

## 来源

素材包：`MalodyV折线皮肤-新版道具RM旧面板 By-Dcyc.msp`。
原包标题：RM旧面板船长；版本：Stable 0512。
原作者：dreamcat、Acc@9961、Sam_0324、TEN-DcyC。
SHA-256：`1354b180dc588db0ce758f9e0426eb848024148e5227208b69e16bee9b679438`。

映射依据是 `rmslideEXF.lua` 和 `info.asm` 的实际游玩引用，未执行原包脚本。

| 组件 | 原包参考 |
| --- | --- |
| 蓝色单键、绿色长条头 | `5note3.png`、`5notel3.png` |
| 圆节点、尾端、连接体 | `notefbody0.png`、`tail.png`、`notebody.png` |
| 无短杆双向箭头 | `tailsl0.png`、`tailsr0.png` |
| 单轨判定区 | `rym-key5k.png` 中的旧版面板 |
| 轨道底板 | `trackbg` 引用的 `1712474142416.png`，采样中央无边线区域 |
| 单键 / 滑键打击动画 | `rym-hit-1..9.png` / `rym-hitl-1..16.png` |

原包没有音频，打击音、界面音效和字体复用默认皮肤。动画使用 30 FPS 和 Alpha 加权加法叠加，保持原帧序。

## 图集及复现

蓝绿 Note 以原包配色与高光为参考，在同一图集中编辑为正俯视面板；保留旧版 RM 的凹槽、四角切面和中央玻璃亮面，但不显示按键底部侧面。判定区紧裁原包五轨面板的中央一块并校正透视。六个发光连接组件来自同一张模型图集，箭头不带短杆。`resources/atlas/source-atlas.png` 保存模型原输出，`atlas-rgba.png` 保存导入图集，`source-reference.png` 保存原包组件参考。

`regions.json` 记录原素材及最终图集的裁切坐标。`components-atlas.png` 汇总九个运行时组件，逐像素对应独立贴图。运行时按键从 `topdown-source-atlas-beveled.png` 的蓝绿双键裁切为 281×123；先前的扁平比较稿保存在 `topdown-source-atlas.png`，便于无损切换。原包素材仍独立存档；判定区裁为 310×130，不增加外部留白；外侧右框与轨道分隔线同色同相对宽度，底框贯通相邻面板；发光连接件统一从模型图集缩小为 50%。

底板保留原素材中央的蓝灰至深棕渐变及透明度，Alpha 乘以 0.55，避免遮住谱面背景。纵向镜像拼接成 256×1024 的无断层平铺纹理，右侧用原判定面板灰棕反光像素提亮构成不透明的 4 像素轨道分隔线。

25 帧动画同时存档为 `effects-source-atlas.png` 和 `effects-atlas.png`，保留 16 位精度；`effects-regions.json` 和 `.tsv` 记录帧位置。轨道原图存档为 `track-source.png`；蓝绿按键及判定区原图分别存档为 `5note3.png`、`5notel3.png`、`rym-key5k.png`。

在仓库根目录运行以下脚本，可从皮肤内存档恢复全部贴图：

```sh
bash "$PWD/assets/skins/rm-old/resources/atlas/extract.sh"
bash "$PWD/assets/skins/rm-old/resources/atlas/extract-effects.sh"
```

## 验证

构建成功。独立测试目录 `build/test_output/rm_old_check` 内 SkinThemeBindingTest、DefaultConfigAssetSyncTest 均通过，覆盖显示名、独立头部尺寸、资源存在、9/16 帧动画、加法模式以及图集增量分发。主构建 BUILD_TESTING=OFF 保持不变。

贴图检查确认连接体不带端帽、箭头不带短杆、按键采用正俯视轮廓。此次未改用户皮肤选择，也未提交。

本批 cloc（注释/代码）：`skin.lua` 58/129（31.02%），`extract.sh` 21/49（30.00%），`extract-effects.sh` 7/15（31.82%），同步 CMake 25/58（30.12%）。既有皮肤测试 70/489（12.52%）、同步测试 42/118（26.25%）低于 30%，未扩展为整文件注释重写。

正俯视编辑使用内置图片模型，提示要点：垂直俯视、零透视与外露底部侧面，保留凹槽、四角切面、中央玻璃亮面及蓝绿配色。最终采用的源图及裁切尺寸对应 `topdown-source-atlas-beveled.png`，扁平比较稿继续保留，运行时结果合并存档为 `components-atlas.png`。
