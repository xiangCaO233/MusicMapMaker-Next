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
extract_component "../image/note/note.png" "256x128+0+0" "+0+0"
extract_component "../image/note/node.png" "24x24+512+0" "+512+0"
extract_component "../image/note/holdend.png" "24x12+1024+0" "+1024+0"
extract_component "../image/note/holdbodyvertical.png" "24x128+0+256" "+0+256"
extract_component "../image/note/holdbodyhorizontal.png" "256x24+512+256" "+512+256"
extract_component "../image/note/arrowleft.png" "128x96+1024+256" "+1024+256"
extract_component "../image/note/arrowright.png" "128x96+0+512" "+0+512"

# 判定区与单键共同存档，默认皮肤两者保持相同内容占比和中心。
extract_component "../image/panel/judgearea.png" "256x128+512+512" "+512+512"

# 成品区域坐标与 regions.json 一致，格位空白不属于导出矩形。
magick -size 1536x768 xc:none "${atlas_parts[@]}" \
    -depth 8 "PNG32:$atlas_dir/note-atlas.png"
