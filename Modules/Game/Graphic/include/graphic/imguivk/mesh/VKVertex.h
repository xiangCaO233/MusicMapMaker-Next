#pragma once

#include "vulkan/vulkan.hpp"
#include <array>
#include <cstddef>

namespace MMM
{
namespace Graphic
{

/**
 * @brief 3D 空间坐标结构体
 */
struct Position {
    float x;
    float y;
    float z{ 0.f };
};

/**
 * @brief RGBA 颜色结构体
 */
struct Color {
    float r;
    float g;
    float b;
    float a;
};

/**
 * @brief Vulkan 顶点数据结构体
 *
 * 包含位置和颜色信息，对应 Shader 中的输入属性。
 */
struct VKVertex final {
    Position pos;
    Color    color;
};


/**
 * @brief 顶点输入绑定描述 (Vertex Binding Description)
 *
 * 定义了数据在内存中的排列方式（Stride）和读取频率（InputRate）。
 */
inline constexpr vk::VertexInputBindingDescription VKVERTEX_BIND_DESC =
    vk::VertexInputBindingDescription()
        // 对应的Shader 中的绑定点 bind = 0(默认)
        .setBinding(0)
        // 每个顶点的步长（字节数）
        .setStride(sizeof(VKVertex))
        // 按顶点步进
        .setInputRate(vk::VertexInputRate::eVertex);

/**
 * @brief 顶点输入属性描述列表 (Vertex Attribute Descriptions)
 *
 * 定义了 Shader 中每个 location 对应的数据格式和偏移量。
 * 包含 Position (loc=0) 和 Color (loc=1)。
 */
inline constexpr std::array<vk::VertexInputAttributeDescription, 2>
    VKVERTEX_ATTR_DESC = {
        // 属性 0: Position (location = 0)
        vk::VertexInputAttributeDescription()
            // 对应的Shader 中的绑定点 bind = 0(默认)
            .setBinding(0)
            // Shader 中的 layout(location = 0)
            .setLocation(0)
            // 3个 float
            .setFormat(vk::Format::eR32G32B32Sfloat)
            // 偏移量
            .setOffset(offsetof(VKVertex, pos)),

        // 属性 1: Color (location = 1)
        vk::VertexInputAttributeDescription()
            .setBinding(0)
            // Shader 中的 layout(location = 1)
            .setLocation(1)
            // 4个 float
            .setFormat(vk::Format::eR32G32B32A32Sfloat)
            // 偏移量
            .setOffset(offsetof(VKVertex, color)),
    };  // namespace Graphic

}  // namespace Graphic

}  // namespace MMM
