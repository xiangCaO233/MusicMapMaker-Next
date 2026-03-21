#pragma once

#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{

/**
 * @brief 2D笔刷管线 描述符集绑定布局
 * 对应 Shader: layout(binding = 0) uniform sampler2D texSampler;
 */
inline constexpr vk::DescriptorSetLayoutBinding BRUSH_TEXTURE_BIND_DESC =
    vk::DescriptorSetLayoutBinding()
        .setBinding(0)
        .setDescriptorType(
            vk::DescriptorType::eCombinedImageSampler)  // 纹理采样器
        .setStageFlags(
            vk::ShaderStageFlagBits::eFragment)  // 只在片段着色器使用
        .setDescriptorCount(1);

}  // namespace Graphic

}  // namespace MMM
