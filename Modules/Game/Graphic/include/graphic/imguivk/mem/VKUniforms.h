#pragma once

#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{

/**
 * @brief 2D笔刷管线 描述符集绑定布局
 *
 * 对应 Shader: layout(binding = 0) uniform sampler2D texSampler;
 * 固定绑定在 set 0 的 binding 0，由片元阶段读取一张 combined image sampler。
 * 该常量同时供共享布局和独立管线布局使用，二者必须保持完全一致。
 */
inline constexpr vk::DescriptorSetLayoutBinding BRUSH_TEXTURE_BIND_DESC =
    vk::DescriptorSetLayoutBinding()
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setStageFlags(vk::ShaderStageFlagBits::eFragment)
        .setDescriptorCount(1);

}  // namespace Graphic

}  // namespace MMM
