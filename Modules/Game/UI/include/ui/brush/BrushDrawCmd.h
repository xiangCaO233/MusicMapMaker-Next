#pragma once

#include <cstdint>
#include <string>
#include <vulkan/vulkan.hpp>

namespace MMM::UI
{
// 绘制指令 (用于处理状态切换，如更换纹理、更换 Shader)
struct BrushDrawCmd {
    uint32_t          indexCount;    // 索引数量
    uint32_t          indexOffset;   // 索引偏移
    uint32_t          vertexOffset;  // 顶点偏移
    vk::DescriptorSet texture;       // 绑定的纹理 (如果为空则画纯色)
    uint32_t          customTextureId{
        0
    };  // 自定义纹理ID，用于逻辑层与渲染层解耦的纹理映射
    std::string shaderId;  // 绑定的自定义 Shader 管线名称 (留作扩展)
};
}  // namespace MMM::UI
