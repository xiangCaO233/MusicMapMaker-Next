#!/usr/bin/env bash
# @brief 从同一张模型图集导出旧版 RM 组件。
# 原始模型输出和游玩素材参考永久存档，不分别重新生成组件。
set -euo pipefail
# 固定绝对资源根，避免依赖调用者的当前目录。
atlas_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# 整张图集归一化后统一去黑底，发光部件使用最大分量透明度。
# 模型图集仅用于发光连接组件；按键与判定面板在下方从原素材导入。
# 内区位于圆角倒边内侧，外围仍使用原图亮度恢复透明边缘。
magick "$atlas_dir/source-atlas.png" -resize 1536x1536! \
    -alpha set -channel A \
    -fx '((i>1070&&i<1487&&j>1194&&j<1335)?1:(max(r,max(g,b))<0.01?0:((j<512&&i<1024)||(i>1024&&j>1024)?min(1,4*max(r,max(g,b))):max(r,max(g,b)))))' \
    -channel RGB -fx 'a>0?u/a:0' +channel -depth 8 "PNG32:$atlas_dir/atlas-rgba.png"
# @brief 所有组件统一按 50% 导出，保留公共光晕尺度。
# 固定裁切坐标同时写入 regions.json，复现时不会重复 trim。
extract() {
    magick "$atlas_dir/atlas-rgba.png" -crop "$2" +repage -resize 50% \
        -depth 8 "PNG32:$atlas_dir/../image/$1.png"
}
# 连接体只取中段，不带端帽；三角头不带短杆。
# 判定区采用紧画布，蓝绿按键保留少量对称外边距。


extract "note/node" "288x288+1132+123"
extract "note/holdend" "200x128+168+688"
extract "note/holdbodyvertical" "128x192+704+656"
extract "note/holdbodyhorizontal" "192x128+1184+688"
extract "note/arrowleft" "272x288+92+1112"
extract "note/arrowright" "272x288+656+1112"

# 原轨道中央采样避开外框，保留原透明度并降低遮挡，允许谱面背景透出。
# 纵向镜像拼接令纹理上下边颜色一致，消除重复平铺时的横向跳变。
# 单轨右边缘取原判定面板的灰棕反光像素，提亮为不透明的 4 像素分隔线，缩放后仍保持可辨识。
magick "$atlas_dir/track-source.png" -crop 32x860+1060+0 +repage \
    -resize 256x1024! -channel A -evaluate Multiply 0.55 +channel \
    \( +clone -flip \) -append \
    \( "$atlas_dir/rym-key5k.png" -crop 1x1+820+6 +repage -alpha off -modulate 160 -resize 4x2048! \) \
    -gravity East -compose Over -composite -resize 256x1024! -depth 8 \
    "PNG32:$atlas_dir/../image/panel/track.png"

# 正俯视按键保留旧版 RM 的凹槽、四角切面和中央玻璃亮面。
# 原图已经带有透明通道，固定裁切仅剔除画布留白，不再重算内部深色区域。
# 两张图共用裁切尺寸与目标尺寸，确保蓝绿按键的结构和视觉重量一致。
extract_topdown_note() {
    magick "$atlas_dir/topdown-source-atlas.png" -crop "$1" +repage \
        -alpha set \
        -resize 281x123! -depth 8 "PNG32:$atlas_dir/../image/note/$2.png"
}
extract_topdown_note "881x360+146+182" "note"
extract_topdown_note "881x360+1145+182" "holdhead"
# 判定面板中央块仍有上窄下宽，紧裁后校正两侧，避免残留透视。
# 右框沿用轨道分隔线的颜色和相对宽度，底框横向贯通相邻面板。
# 内部圆角保留原图；外框到达画布边界，避免透明角落切断连接。
magick "$atlas_dir/rym-key5k.png" -crop 310x130+668+2 +repage \
    -virtual-pixel transparent -define distort:viewport=310x130+0+0 \
    -distort Perspective '10,0 0,0 299,0 309,0 0,110 0,110 309,110 309,110' \
    +repage \
    \( "$atlas_dir/rym-key5k.png" -crop 1x1+820+6 +repage -alpha off -modulate 160 -resize 5x130! \) \
    -gravity East -compose Over -composite \
    \( "$atlas_dir/rym-key5k.png" -crop 1x1+820+6 +repage -alpha off -modulate 160 -resize 310x3! \) \
    -gravity South -composite -depth 8 "PNG32:$atlas_dir/../image/panel/judgearea.png"
# 最终组件另存统一图集，按键、面板和发光连接件均与运行时贴图逐像素一致。
# 每格使用固定中心坐标；裁切清单区分原图坐标与最终图集坐标。
magick -size 1536x1536 xc:none -compose Copy \
    "$atlas_dir/../image/note/note.png" -geometry +116+195 -composite \
    "$atlas_dir/../image/note/holdhead.png" -geometry +628+195 -composite \
    "$atlas_dir/../image/note/node.png" -geometry +1208+184 -composite \
    "$atlas_dir/../image/note/holdend.png" -geometry +206+736 -composite \
    "$atlas_dir/../image/note/holdbodyvertical.png" -geometry +736+720 -composite \
    "$atlas_dir/../image/note/holdbodyhorizontal.png" -geometry +1232+736 -composite \
    "$atlas_dir/../image/note/arrowleft.png" -geometry +188+1208 -composite \
    "$atlas_dir/../image/note/arrowright.png" -geometry +700+1208 -composite \
    "$atlas_dir/../image/panel/judgearea.png" -geometry +1125+1215 -composite \
    -depth 8 "PNG32:$atlas_dir/components-atlas.png"
