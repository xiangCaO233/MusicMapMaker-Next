#pragma once

#include <cstdint>
#include <string>
#include <vulkan/vulkan.hpp>

namespace MMM::UI
{
/// @brief Brush 批处理提交给图形层的单段绘制指令。
/// @details 指令只描述索引范围与渲染状态，不拥有顶点、索引或纹理资源。
struct BrushDrawCmd {
    /// @brief 本段提交的索引数量。
    uint32_t indexCount{ 0 };
    /// @brief 本段在共享索引缓冲中的起始偏移。
    uint32_t indexOffset{ 0 };
    /// @brief 本段索引引用的基础顶点偏移。
    uint32_t vertexOffset{ 0 };
    /// @brief 绑定纹理描述符；空值表示纯色绘制。
    vk::DescriptorSet texture{};
    /// @brief 逻辑层纹理 ID，用于与渲染层描述符映射解耦。
    uint32_t customTextureId{ 0 };
    /// @brief 自定义 Shader 管线稳定名称，预留给扩展状态切换。
    std::string shaderId;
    /// @brief 当前批次裁剪矩形，单位为物理像素。
    vk::Rect2D scissor{};
};
}  // namespace MMM::UI
