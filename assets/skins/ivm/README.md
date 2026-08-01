# IVM 皮肤

IVM 是一套仿经典 Windows 音游制谱工具的内置皮肤。皮肤使用
Liberation Sans 作为与 Arial 指标兼容的 ASCII 字体，并复用默认皮肤的
CJK 与图标字体。随皮肤分发的 Liberation Sans 使用
[`SIL Open Font License 1.1`](resources/font/OFL-1.1.txt)。

物件纹理均为白色 Alpha 遮罩，运行时由 `skin.lua` 的颜色进行纯色着色：

- `note.png`：无描边矩形；
- `node.png`：直径为 24 px 的实心圆形节点；
- `holdbodyvertical.png`、`holdbodyhorizontal.png`：宽度或厚度均为
  24 px 的实心 Body；
- `holdend.png`：宽度与竖向 Body 相同的实心上半圆；
- `arrowleft.png`、`arrowright.png`：折线宽度为 24 px 的尖括号箭头；
  两个方向由同一纹理镜像生成，因此张开角度完全一致，连接处宽度也与
  Body 一致；纹理内置一段穿过目标轨中心的水平 Body，用于和渲染器绘制
  到目标轨中心的连接段重叠，避免箭头接缝。
- `panel/judgearea.png`：与 `note.png` 同为 256×128、没有透明边距的
  纯色判定区，显示尺寸与物件完全一致。
- `note/effect/note`、`note/effect/flick`：独立的 32×256 纵向粉色渐变
  轨道光效序列。`effects.hit_effect.layout = "track_fill"` 会将每帧拉伸到
  对应单轨的完整可见区域，并绘制在拍线及物件下方；其他皮肤省略该配置或
  使用 `"fixed"` 时，仍在判定线打击点按物件缩放并置顶绘制原有固定尺寸
  纹理组。

IVM 默认拍头线使用完全不透明的纯红色，其余分拍线槽位使用同一组完全
不透明灰色。皮肤推荐主题的亮色和暗色分支均固定为内置 `IVM`，因此在
软件主题设置为“自动”时不会随系统亮暗外观切换。
