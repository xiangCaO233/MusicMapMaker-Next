#!/usr/bin/env bash
# @brief 从同一张模型原始图集重建 RM 的全部静态音符组件。
# 原始输出永久保留，归一化图集只是可复现的导入中间产物。
# 全部组件共用颜色和倍率；实体按键与发光连接件采用各自的遮挡语义。
set -euo pipefail

# 脚本从自身所在目录定位资源，调用时应位于项目根目录。
# 所有读写路径均转换为绝对路径，不依赖当前 shell 的资源相对路径。
atlas_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
note_dir="$atlas_dir/../image/note"

# 模型原图为 1774 × 887，整张等比归一化到 2048 × 1024。
# 黑色底用于保存模型原始输出，不能直接当作不透明游戏贴图导入。
# 全图暗部反预乘保证在黑底上合成时保留亮度，极暗背景噪声归零。
# 左上两格为实体按键，内部保持不透明，遮住延伸到中心的连接体。
# 其余发光组件透明度取最大色彩分量，避免暗晕遮断下层连接体。
magick "$atlas_dir/rm-atlas-source.png" -resize 2048x1024! \
    -alpha set -channel A \
    -fx 'max(r,max(g,b))<0.01?0:((i<1024&&j<512)?min(1,4*max(r,max(g,b))):max(r,max(g,b)))' \
    -channel RGB -fx 'a>0?u/a:0' +channel -depth 8 \
    "PNG32:$atlas_dir/rm-atlas-rgba.png"

# @brief 按归一化图集坐标裁切，再统一缩小为 50%。
# 参数一为输出文件名，参数二为宽高和左上角坐标。
# 留白参与软件的相对尺寸计算，不对裁出的组件分别 trim 或自动缩放。
extract_component() {
    magick "$atlas_dir/rm-atlas-rgba.png" -crop "$2" +repage \
        -resize 50% -depth 8 "PNG32:$note_dir/$1.png"
}

# 同尺寸的两种按键同时保留玻璃倒角和完整外围光晕。
extract_component note '512x224+0+138'
extract_component holdhead '512x224+512+138'
# 圆节点与矩形尾端分别居中，裁切范围涵盖全图一致的辉光边距。
extract_component node '364x364+1098+68'
extract_component holdend '256x160+1664+170'
# 连接体只取长条中段，排除模型绘制的端帽，纵向拉伸时无额外接缝。
extract_component holdbodyvertical '128x256+192+612'
extract_component holdbodyhorizontal '256x128+640+680'
# 左右三角头分别来自同一图集，均无短杆；软件连接体延伸到其内部。
extract_component arrowleft '296x364+1132+562'
extract_component arrowright '296x364+1644+562'
