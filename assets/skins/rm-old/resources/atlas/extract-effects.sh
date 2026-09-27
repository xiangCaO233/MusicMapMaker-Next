#!/usr/bin/env bash
# @brief 从游玩原帧图集导出打击动画，保持 9 / 16 帧原始次序。
# 原图集保留 16 位数据，运行时通过独立 PNG 路径加载。
set -euo pipefail
# 只使用当前皮肤内的原图，不依赖下载包所在位置。
atlas_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
parts=()
frame_index=0
# 最大 RGB 分量构造透明度并反预乘，与 Alpha 加权加法混合配套。
# 黑色像素贡献为零，不会在背景上产生黑色方框。
while IFS=$'\t' read -r output crop; do
    magick "$atlas_dir/effects-source-atlas.png" -crop "$crop" +repage \
        -alpha set -channel A -fx 'max(r,max(g,b))' \
        -channel RGB -fx 'a>0?u/a:0' +channel -resize 512x512! \
        -depth 16 "PNG64:$atlas_dir/$output"
    # 成品帧等尺寸排列，空白格位不会导入独立纹理。
    parts+=("$atlas_dir/$output" -geometry "+$((frame_index%5*512))+$((frame_index/5*512))" -compose Copy -composite)
    frame_index=$((frame_index+1))
done < "$atlas_dir/effects-regions.tsv"
# 原始图集和去黑底成品图集同时留档，保证动画导入可复现。
magick -size 2560x2560 xc:none "${parts[@]}" \
    -depth 16 "PNG64:$atlas_dir/effects-atlas.png"
