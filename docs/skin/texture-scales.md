# 皮肤纹理独立缩放

在 `skin.lua` 返回表的顶层添加 `texture_scales`，用 `assets` 中的完整点分键指定倍率：

```lua
texture_scales = {
    ["note.effect.note"] = 1.6,
    ["note.effect.flick"] = 1.6,
    ["note.effect.hold"] = 1.6,
    -- 其他纹理也可独立配置：
    ["note.note"] = 0.9,
    ["cursor"] = 1.2,
},
```

RM 与 RM-old 已将三种打击特效设为 `1.6`；要调整效果，修改对应数值即可。省略的键使用 `1`，大于 `1` 放大、小于 `1` 缩小。只接受正有限数值；零、负数、非数值及超出可表示范围的数值按缺省处理。

序列键作用于该动画的全部帧。即使 Hold 与 Flick 引用同一组图片，二者倍率仍彼此独立。未声明独立 Hold 序列的旧皮肤继续使用单键序列及其倍率。

## 与布局设置的关系

这项配置不写回“布局设置 → 物件缩放”。普通物件和固定尺寸打击特效的最终尺寸为：

`原有绘制尺寸（已应用布局横向/纵向物件缩放）× 对应纹理倍率`

例如布局为横向 `0.8`、纵向 `1.2`，特效纹理倍率为 `1.6`，最终两轴倍率分别为 `1.28` 和 `1.92`。仅设置 `note.effect.*` 不会改变 Note 本体。

采用 `effects.hit_effect.layout = "track_fill"` 的特效先按整轨大小布局，再乘纹理倍率；它不使用固定特效的物件宽高基准。

## 支持范围与锚点

- 音符组件：`note.note`、`note.holdhead`、`note.node`、`note.holdend`、`note.arrowleft`、`note.arrowright`。
- 连接体：`note.holdbodyvertical`、`note.holdbodyhorizontal`。
- 轨道与判定区：`panel.track.background`、`panel.track.judgearea`。
- 动画：`note.effect.note`、`note.effect.flick`、`note.effect.hold`，以及皮肤声明的其他序列帧纹理。
- UI 图像：`logo`、`cursor`、`cursortrail`、`cursor_smoke`。

点状纹理以原中心缩放，保持 UV、颜色、帧率及混合模式不变。连接体仅缩放厚度，保留两端中心和实际时间/轨道跨度，避免长条和折线脱离端点。Note 的拾取和引导尺寸同步适配；Logo 按钮占位、点击热区和鼠标热点不变。

倍率改变绘制几何，不改变纹理文件、图集分辨率、谱面数据、判定或音效。放大后的纹理仍受所在画布/窗口的裁剪边界限制。项目封面、项目背景和字体不是皮肤 `assets` 纹理，不受此表影响。

修改实际加载的皮肤文件后，切换到其他皮肤再切回，或重启软件，重新加载配置。仅编辑源码目录不会自动改变已经运行的程序所加载的皮肤；本地构建会同步内置皮肤资源。

Linux 默认加载 `~/.config/mmm/assets/skins/<皮肤名>/skin.lua`。可在这里调数值并切换皮肤即时试效果；但下一次构建会用源码中的内置皮肤覆盖该文件。满意后把数值同步回 `assets/skins/<皮肤名>/skin.lua`，或保存在单独的自定义皮肤目录。
