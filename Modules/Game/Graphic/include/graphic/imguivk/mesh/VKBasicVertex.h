#pragma once

#include "common/render/CanvasRenderTypes.h"

#include <array>
#include <cstddef>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic::Vertex
{

/// @brief 图形模块沿用的画布顶点位置别名。
using Position = Common::Render::CanvasPosition;

/// @brief 图形模块沿用的画布顶点颜色别名。
using Color = Common::Render::CanvasColor;

/// @brief 图形模块沿用的画布纹理坐标别名。
using TexUV = Common::Render::CanvasTexCoord;

/// @brief Vulkan 顶点输入使用的共享画布顶点别名。
using VKBasicVertex = Common::Render::CanvasVertex;

/// @brief 共享顶点必须保持既有九个 float 的紧凑 GPU 上传布局。
static_assert(sizeof(VKBasicVertex) == sizeof(float) * 9U);
/// @brief 位置属性必须从顶点首地址开始。
static_assert(offsetof(VKBasicVertex, pos) == 0U);
/// @brief 颜色属性必须紧随三个位置分量。
static_assert(offsetof(VKBasicVertex, color) == sizeof(float) * 3U);
/// @brief UV 属性必须紧随位置和颜色分量。
static_assert(offsetof(VKBasicVertex, uv) == sizeof(float) * 7U);

/// @brief 画布顶点输入绑定描述。
inline constexpr vk::VertexInputBindingDescription VKVERTEX_BIND_DESC =
    vk::VertexInputBindingDescription()
        .setBinding(0)
        .setStride(sizeof(VKBasicVertex))
        .setInputRate(vk::VertexInputRate::eVertex);

/// @brief 画布顶点输入属性描述列表。
inline constexpr std::array<vk::VertexInputAttributeDescription, 3>
    VKVERTEX_ATTR_DESC = {
        vk::VertexInputAttributeDescription()
            .setBinding(0)
            .setLocation(0)
            .setFormat(vk::Format::eR32G32B32Sfloat)
            .setOffset(offsetof(VKBasicVertex, pos)),
        vk::VertexInputAttributeDescription()
            .setBinding(0)
            .setLocation(1)
            .setFormat(vk::Format::eR32G32B32A32Sfloat)
            .setOffset(offsetof(VKBasicVertex, color)),
        vk::VertexInputAttributeDescription()
            .setBinding(0)
            .setLocation(2)
            .setFormat(vk::Format::eR32G32Sfloat)
            .setOffset(offsetof(VKBasicVertex, uv)),
    };

}  // namespace MMM::Graphic::Vertex
