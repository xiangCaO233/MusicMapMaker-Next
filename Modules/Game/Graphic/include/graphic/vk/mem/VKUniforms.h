#pragma once

#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{

/**
 * @brief Vulkan 测试用时间戳Uniform变量
 *
 * 仅包含时间信息，对应 Shader 中的Uniform属性。
 */
struct VKTestTimeUniform final {
    float time;
};

/**
 * @brief Uniform布局绑定描述 (Uniform Layout Binding Description)
 *
 * 定义了Uniform数据在内存中的排列方式
 */
inline constexpr vk::DescriptorSetLayoutBinding TEST_TIMEUNIFORM_BIND_DESC =
    vk::DescriptorSetLayoutBinding()
        // 对应的Shader 中的绑定点 bind = 0(默认)
        .setBinding(0)
        // 数据类型 - Uniform
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        // 阶段设置 - 顶点着色阶段和片段着色阶段都用
        .setStageFlags(vk::ShaderStageFlagBits::eVertex |
                       vk::ShaderStageFlagBits::eFragment)
        // 描述UBO的变量数量(对应shader中的uniform变量数组) - 就一个
        .setDescriptorCount(1);

}  // namespace Graphic

}  // namespace MMM
