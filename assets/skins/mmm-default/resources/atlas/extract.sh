#!/usr/bin/env bash
# @brief 从皮肤图集恢复独立音符贴图。
# 原图画布和每个裁切区域都保存在 regions.json，避免依赖自动 trim。
set -euo pipefail

# 所有路径以脚本目录为基准，允许在仓库根目录复现。
atlas_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# 同一轮导出同时重建成品图集，避免图集预览与独立贴图不同步。
atlas_parts=()

# @brief 按固定坐标裁切；可选尺寸参数统一单键和判定区的内容与画布。
# 相邻图块不参与导出，保持抗锯齿与连接体边缘的原始像素。
extract_component() {
    # 仅单键与判定区需要统一留白，连接体不得在延伸方向添加空白。
    local transform=()
    if [[ $# -eq 5 ]]; then
        transform=(-resize "$4!" -background none -gravity center -extent "$5")
    fi
    magick "$atlas_dir/source-atlas.png" -crop "$2" +repage "${transform[@]}" \
        -depth 8 "PNG32:$atlas_dir/$1"
    # 拼合只复制像素，透明区域不会叠加改色。
    atlas_parts+=("$atlas_dir/$1" -geometry "$3" -compose Copy -composite)
}

# 每个组件独立保留形状比例，左右箭头采用对称画布。
# 连接体沿长度方向不裁切，确保端点仍能延伸到连接中心。
extract_component "../image/note/note.png" "380x184+66+36" "+0+0" "380x184" "400x194"
extract_component "../image/note/node.png" "108x108+586+74" "+512+0"
extract_component "../image/note/holdend.png" "234x92+1163+82" "+1024+0"
extract_component "../image/note/holdbodyvertical.png" "52x40+102+256" "+0+256"
extract_component "../image/note/holdbodyhorizontal.png" "40x54+512+357" "+512+256"
extract_component "../image/note/arrowleft.png" "172x198+1066+285" "+1024+256"
extract_component "../image/note/arrowright.png" "172x198+42+541" "+0+512"

# 判定区与单键共同存档，默认判定区铺满画布，单键保留约 95% 占比。
extract_component "../image/panel/judgearea.png" "415x214+560+521" "+512+512" "400x194" "400x194"

# 成品区域坐标与 regions.json 一致，格位空白不属于导出矩形。
magick -size 1536x768 xc:none "${atlas_parts[@]}" \
    -depth 8 "PNG32:$atlas_dir/note-atlas.png"
