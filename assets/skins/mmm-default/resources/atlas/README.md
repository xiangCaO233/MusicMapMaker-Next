# 音符图集

`source-atlas.png` 按原始画布存档七个静态音符组件及判定区；`note-atlas.png` 收录最终导出的组件。透明格位留白仅用于分隔组件，不会导出到独立贴图中。

`regions.json` 记录原图区域、裁切范围和成品区域。运行本目录 `extract.sh` 可从原图集逐像素恢复独立 PNG，保持原有外观；判定区仅在默认皮肤中缩放到与单键一致的内容尺寸。22 张打击动画帧另存为 `effects-source-atlas.png` 和 `effects-atlas.png`；`effects-regions.json` / `.tsv` 记录所有帧坐标，`extract-effects.sh` 重建帧序列。

默认皮肤裁掉大片完全透明边距，左右箭头、节点及横向连接体最多保留两像素用于维持画布中心。单键使用 400×194 画布、380×184 内容，宽度占比 95%、高度占比约 94.85%；判定区与单键打击特效均为 400×194，内容铺满画布，不留外圈空白。默认宽高缩放均为 0.95。

## 验证（2026-09-10）

原图集与修改前八张 PNG 逐像素一致；成品图集与裁切输出逐像素一致。构建成功，SkinThemeBindingTest、VisualConfigTest、DefaultConfigAssetSyncTest、ExtremeSvHoldRenderTest 均通过。图片由既有 Git LFS 规则追踪。

本目录 `extract.sh` 注释 12 行、代码 23 行，注释率 34.29%。 `Modules/Config/include/config/VisualConfig.h` 注释 84 行、代码 96 行，注释率 46.67%。已有配置中的自定义缩放保持原值，恢复物件默认设置后使用 1.0。

滑键特效 16 帧统一裁切到 682×674，保留原画布中心与所有非透明像素，避免逐帧紧裁引起中心跳动；播放顺序、帧率和透明度不变。

特效原图集保留 16 位通道精度。`extract-effects.sh` 注释 10 行、代码 11 行，注释率 47.62%。五项相关 CTest（含 HitEffectStereoTest）通过。
