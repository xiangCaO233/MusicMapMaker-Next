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
///
/// 单个顶点缓冲绑定按 VKBasicVertex 的完整跨度前进，输入频率为逐顶点；实例化
/// 数据若后续引入，必须使用独立 binding，不能改变此跨逻辑模块共享的布局。
inline constexpr vk::VertexInputBindingDescription VKVERTEX_BIND_DESC =
    vk::VertexInputBindingDescription()
        .setBinding(0)
        .setStride(sizeof(VKBasicVertex))
        .setInputRate(vk::VertexInputRate::eVertex);

/// @brief 画布顶点输入属性描述列表。
///
/// location 0、1、2 分别映射位置、颜色和 UV，格式与 CanvasRenderTypes 中九个
/// float 的物理排列严格对应。着色器输入 location 或分量宽度变化时必须同步更新
/// 共享顶点契约和上述 static_assert。
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
