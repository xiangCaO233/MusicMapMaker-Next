# 打击特效混合

皮肤可以在 `effects.hit_effect.blend` 中按资源键选择 `alpha` 或 `additive`。未声明或未知值回退为 `alpha`。

```lua
hit_effect = {
    layout = "fixed",
    blend = { ["note.effect.flick"] = "additive" },
}
```

默认皮肤仅爆炸特效启用加法，矩形反馈保持覆盖；RM 的两组打击光启用加法；IVM 未声明，保留整轨渐变覆盖。

加法管线使用非预乘 RGBA 输入，RGB 为 `src.rgb * src.a + dst.rgb`，保留目标 Alpha。暗色光晕不会削弱背景，零 Alpha 像素不产生贡献。RM 图集导出的 RGB 已反预乘，不在着色器中重复乘透明度。不能将未经转换的预乘贴图直接当作该模式的输入。

逻辑命令携带混合标记；Batcher 即使在同图集内也按模式切批。主画布和预览绑定布局兼容的预建管线，结束后恢复普通管线。原模糊发光合成管线保持原配置，未将所有普通物件全局改为加法。

Vulkan 混合因子定义：[Khronos VkBlendFactor](https://docs.vulkan.org/refpages/latest/refpages/source/VkBlendFactor.html)。

## 验证

构建及 SkinThemeBindingTest、HitEffectStereoTest、ExtremeSvHoldRenderTest、DefaultConfigAssetSyncTest 通过。新增用例覆盖皮肤模式选择与同图集覆盖→加法→覆盖的批次边界。使用独立 MMM_CONFIG_ROOT 启动实际软件检查管线创建，日志与截图位于 build/test_output/hit_blend_visual；该启动检查不等同于 GPU 像素回归测试。用户随后在实际画面中确认叠加效果正确。

## 本批注释率

以下为 cloc 逐文件统计。既有大文件仍有低于 30% 的项，本次未扩展为整文件注释重写。

| 文件 | 注释 | 代码 | 比例 |
| --- | ---: | ---: | ---: |
| `Modules/Game/Canvas/src/Basic2DCanvas_Rendering.cpp` | 44 | 809 | 5.16% |
| `Modules/Game/Canvas/src/PreviewCanvas.cpp` | 73 | 741 | 8.97% |
| `Modules/Config/src/skin/SkinLoader.cpp` | 93 | 638 | 12.72% |
| `Modules/Game/Graphic/src/imguivk/VKOffScreenRendererRess.cpp` | 106 | 632 | 14.36% |
| `Modules/Config/tests/SkinThemeBindingTest.cpp` | 68 | 480 | 12.41% |
| `Modules/Game/Logic/src/logic/ecs/system/HitFXSystem.cpp` | 169 | 390 | 30.23% |
| `Modules/Game/Graphic/src/imguivk/VKOffScreenRenderer.cpp` | 29 | 377 | 7.14% |
| `Modules/Game/Logic/include/logic/ecs/system/render/Batcher.h` | 147 | 340 | 30.18% |
| `Modules/Game/Logic/tests/HitEffectStereoTest.cpp` | 136 | 338 | 28.69% |
| `Modules/Game/Graphic/include/graphic/imguivk/VKOffScreenRenderer.h` | 111 | 230 | 32.55% |
| `assets/skins/mmm-default/skin.lua` | 42 | 229 | 15.50% |
| `Modules/Game/Graphic/src/imguivk/VKRenderPipeline.cpp` | 58 | 173 | 25.11% |
| `Modules/Config/include/config/skin/SkinConfig.h` | 88 | 143 | 38.10% |
| `assets/skins/rm/skin.lua` | 57 | 129 | 30.65% |
| `Modules/Game/Common/include/common/render/CanvasRenderTypes.h` | 11 | 41 | 21.15% |
| `Modules/Game/Graphic/include/graphic/imguivk/VKRenderPipeline.h` | 25 | 41 | 37.88% |
