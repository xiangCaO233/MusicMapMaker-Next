#!/usr/bin/env bash
# @brief 从原始特效图集重建帧序列和成品图集。
# 帧序号和坐标固定保存，不依赖文件系统枚举次序。
set -euo pipefail

# 所有路径由脚本目录定位，允许在仓库根目录重建。
atlas_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
parts=()
# TSV 四列分别为输出路径、裁切区域、成品尺寸和图集位置。
# 坐标同时以 JSON 保存，方便人工检查原始画布和裁切关系。
while IFS=$'\t' read -r output crop dimensions position; do
    # 同组帧采用固定裁切，避免逐帧 trim 引入动画中心抖动。
    # 默认单键光效统一到判定区尺寸，滑键保留原中心和全部非透明像素。
    magick "$atlas_dir/effects-source-atlas.png" -crop "$crop" +repage \
        -resize "$dimensions!" -depth 16 "PNG64:$atlas_dir/$output"
    # 保留 16 位渐变与原始 Alpha，不在拼图时再次混合或着色。
    parts+=("$atlas_dir/$output" -geometry "$position" -compose Copy -composite)
done < "$atlas_dir/effects-regions.tsv"

# 成品图集仅用于存档，运行时仍由原序列帧路径加载。
# 图块之间的空白不会进入独立帧，也不改变帧率或播放顺序。
magick -size 3248x3160 xc:none "${parts[@]}" \
    -depth 16 "PNG64:$atlas_dir/effects-atlas.png"
