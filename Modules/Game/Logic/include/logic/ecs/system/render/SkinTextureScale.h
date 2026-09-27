#pragma once

#include <cstdint>

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
}  // namespace MMM::Logic::System
