#include "ui/brush/Brush.h"
#include <cmath>
#include <glm/glm.hpp>


namespace MMM::UI
{

/// @brief 清理顶点缓存和绘制指令
void Brush::clear()
{
    m_vertices.clear();
    m_indices.clear();
    m_cmds.clear();
}

void Brush::setColor(const glm::vec4& color)
{
    m_currentColor = color;
}

void Brush::setColor(float r, float g, float b, float a)
{
    m_currentColor = glm::vec4(r, g, b, a);
}

void Brush::setColor(uint32_t colorHex)
{
    m_currentColor = unpackColor(colorHex);
}

void Brush::setTexture(vk::DescriptorSet texture)
{
    // 如果纹理发生变化，强制打断当前的 Batch
    // updateDrawCmd 会在有新的顶点写入时处理，这里只需设置状态即可。
    // 如果后续没有绘制任何东西，这个状态改变不会产生空的 DrawCmd
    if ( m_currentTexture != texture ) {
        m_currentTexture = texture;
    }
}

void Brush::updateDrawCmd(uint32_t indicesCount)
{
    if ( m_cmds.empty() ) {
        BrushDrawCmd cmd;
        cmd.indexOffset  = 0;
        cmd.vertexOffset = 0;
        cmd.indexCount   = indicesCount;
        cmd.texture      = m_currentTexture;
        m_cmds.push_back(cmd);
        return;
    }

    auto& lastCmd = m_cmds.back();
    // 如果状态（这里主要是纹理）相同，就合并
    if ( lastCmd.texture == m_currentTexture ) {
        lastCmd.indexCount += indicesCount;
    } else {
        // 状态不同，开启新的批次
        BrushDrawCmd cmd;
        // 新批次的索引偏移是旧批次的偏移 + 旧批次的数量
        cmd.indexOffset = lastCmd.indexOffset + lastCmd.indexCount;
        // 顶点偏移这里保持为 0，因为我们的 indices 是绝对索引
        // (相对于整个顶点数组)
        cmd.vertexOffset = 0;
        cmd.indexCount   = indicesCount;
        cmd.texture      = m_currentTexture;
        m_cmds.push_back(cmd);
    }
}


/// @brief 绘制一个实心矩形
void Brush::drawRect(float x, float y, float w, float h)
{
    // 获取当前顶点数，用于计算索引偏移
    uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());

    // 1. 添加 4 个顶点 (注意：坐标根据投影矩阵来)
    // 假设投影矩阵是 Ortho(0, width, 0, height) 且 Y 轴已翻转
    m_vertices.push_back({ { x, y, 0.0f },
                           { m_currentColor.r,
                             m_currentColor.g,
                             m_currentColor.b,
                             m_currentColor.a },
                           { 0.0f, 0.0f } });  // 左上
    m_vertices.push_back({ { x + w, y, 0.0f },
                           { m_currentColor.r,
                             m_currentColor.g,
                             m_currentColor.b,
                             m_currentColor.a },
                           { 1.0f, 0.0f } });  // 右上
    m_vertices.push_back({ { x, y + h, 0.0f },
                           { m_currentColor.r,
                             m_currentColor.g,
                             m_currentColor.b,
                             m_currentColor.a },
                           { 0.0f, 1.0f } });  // 左下
    m_vertices.push_back({ { x + w, y + h, 0.0f },
                           { m_currentColor.r,
                             m_currentColor.g,
                             m_currentColor.b,
                             m_currentColor.a },
                           { 1.0f, 1.0f } });  // 右下

    // 2. 添加 6 个索引 (两个三角形拼成矩形)
    // 三角形 1: 左上, 右上, 左下
    m_indices.push_back(baseIndex + 0);
    m_indices.push_back(baseIndex + 1);
    m_indices.push_back(baseIndex + 2);
    // 三角形 2: 右上, 右下, 左下
    m_indices.push_back(baseIndex + 1);
    m_indices.push_back(baseIndex + 3);
    m_indices.push_back(baseIndex + 2);

    // 3. 更新绘制指令
    updateDrawCmd(6);
}

void Brush::drawRect(glm::vec2 pos, glm::vec2 size)
{
    drawRect(pos.x, pos.y, size.x, size.y);
}

// 辅助函数：将 uint32_t 颜色 (0xRRGGBBAA) 转换为 glm::vec4
glm::vec4 Brush::unpackColor(uint32_t color)
{
    return glm::vec4(((color >> 24) & 0xFF) / 255.0f,
                     ((color >> 16) & 0xFF) / 255.0f,
                     ((color >> 8) & 0xFF) / 255.0f,
                     (color & 0xFF) / 255.0f);
}


void Brush::drawRect(glm::vec2 pos, glm::vec2 size, glm::vec4 color)
{
    glm::vec4 oldColor = m_currentColor;
    setColor(color);
    drawRect(pos.x, pos.y, size.x, size.y);
    setColor(oldColor);
}

void Brush::drawRect(glm::vec2 pos, glm::vec2 size, uint32_t color)
{
    glm::vec4 oldColor = m_currentColor;
    setColor(color);
    drawRect(pos.x, pos.y, size.x, size.y);
    setColor(oldColor);
}

void Brush::drawCircle(float cx, float cy, float radius, int segments)
{
    if ( segments < 3 ) segments = 3;

    uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());

    // 1. 圆心顶点
    m_vertices.push_back(
        { { cx, cy, 0.0f },
          { m_currentColor.r,
            m_currentColor.g,
            m_currentColor.b,
            m_currentColor.a },
          { 0.5f, 0.5f } });  // 圆心的 UV 也可以计算，这里简单给 0.5

    // 2. 边缘顶点
    float angleStep = (2.0f * 3.1415926535f) / segments;
    for ( int i = 0; i < segments; ++i ) {
        float angle = i * angleStep;
        float x     = cx + radius * std::cos(angle);
        float y     = cy + radius * std::sin(angle);

        // 简易的 UV 映射
        float u = 0.5f + 0.5f * std::cos(angle);
        float v = 0.5f + 0.5f * std::sin(angle);

        m_vertices.push_back({ { x, y, 0.0f },
                               { m_currentColor.r,
                                 m_currentColor.g,
                                 m_currentColor.b,
                                 m_currentColor.a },
                               { u, v } });
    }

    // 3. 构建索引 (三角形扇)
    for ( int i = 0; i < segments; ++i ) {
        m_indices.push_back(baseIndex);          // 中心
        m_indices.push_back(baseIndex + 1 + i);  // 当前边缘点
        // 下一个边缘点 (闭合时回到第一个)
        uint32_t nextIdx = (i == segments - 1) ? 1 : (i + 2);
        m_indices.push_back(baseIndex + nextIdx);
    }

    // 4. 更新指令
    updateDrawCmd(segments * 3);
}

}  // namespace MMM::UI
