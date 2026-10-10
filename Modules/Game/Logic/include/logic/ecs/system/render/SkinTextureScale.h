#pragma once

#include "config/visual/NoteTexturePosition.h"
#include <cstdint>

namespace MMM::Config
{
enum class BackgroundFillMode;
}

namespace MMM::Common::Render
{
enum class TextureID : std::uint32_t;
}

namespace MMM::Logic::System
{
/// @brief 将画布纹理身份转换为皮肤声明的独立视觉倍率。
/// @param texture 静态皮肤纹理或已分配的序列帧 ID。
/// @return 正有限倍率；未配置、纯色、项目背景及字形均返回 1。
/// @note 映射留在逻辑层，配置模块不反向依赖画布纹理枚举。
/// @warning 逐图元热路径只查询已加载的表，不读取文件或拼接资源键。
[[nodiscard]] float skinTextureScale(Common::Render::TextureID texture);
/// @brief 取得 Note 固定对齐所需的纵向比例，中心为零，底边为负半高。
/// @note 判定区和固定尺寸打击帧采用底边规则，连接体由端点位置决定。
/// @warning 逐图元热路径仅判断枚举，不访问文件、Lua 或分配资源。
[[nodiscard]] float noteTextureVerticalOffset(
    Common::Render::TextureID texture, Config::NoteTexturePosition position);
/// @brief 根据最终点贴图尺寸计算视觉中心相对时间锚点的位移。
/// @param texture 实际点纹理，不包含按跨度绘制的连接体。
/// @param width 已包含皮肤倍率的布局宽度。
/// @param height 已包含皮肤倍率的布局高度。
/// @param aspect 原贴图宽高比，不包含布局横纵缩放。
/// @param fillMode 当前点贴图的填充方式。
/// @param position 当前中心或底边位置。
/// @return 填充之后的纵向中心偏移，供连接点与拾取共同使用。
/// @warning 热路径仅计算标量，不加载资源、分配内存或同步线程。
[[nodiscard]] float noteTextureCenterShiftY(
    Common::Render::TextureID texture, float width, float height, float aspect,
    Config::BackgroundFillMode fillMode, Config::NoteTexturePosition position);
}  // namespace MMM::Logic::System
