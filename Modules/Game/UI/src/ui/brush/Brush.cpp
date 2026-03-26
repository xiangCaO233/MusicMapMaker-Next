#include "ui/brush/Brush.h"
#include <glm/glm.hpp>


namespace MMM::Graphic::UI
{

/// @brief 清理顶点缓存
void Brush::clear()
{
    m_vertices.clear();
    m_indices.clear();
}

/// @brief 绘制一个矩形
void Brush::drawRect(glm::vec2 pos, glm::vec2 size, glm::vec4 color)
{
    // 获取当前顶点数，用于计算索引偏移
    uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());

    // 1. 添加 4 个顶点 (注意：坐标根据你的投影矩阵来)
    // 假设你的投影矩阵是 Ortho(0, width, 0, height) 且 Y 轴已翻转
    m_vertices.push_back({ { pos.x, pos.y, 0.0f },
                           { color.r, color.g, color.b, color.a },
                           { 0.0f, 0.0f } });  // 左上
    m_vertices.push_back({ { pos.x + size.x, pos.y, 0.0f },
                           { color.r, color.g, color.b, color.a },
                           { 1.0f, 0.0f } });  // 右上
    m_vertices.push_back({ { pos.x, pos.y + size.y, 0.0f },
                           { color.r, color.g, color.b, color.a },
                           { 0.0f, 1.0f } });  // 左下
    m_vertices.push_back({ { pos.x + size.x, pos.y + size.y, 0.0f },
                           { color.r, color.g, color.b, color.a },
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
}

// 辅助函数：将 uint32_t 颜色 (0xRRGGBBAA) 转换为 glm::vec4
glm::vec4 unpackColor(uint32_t color)
{
    return glm::vec4(((color >> 24) & 0xFF) / 255.0f,
                     ((color >> 16) & 0xFF) / 255.0f,
                     ((color >> 8) & 0xFF) / 255.0f,
                     (color & 0xFF) / 255.0f);
}

void Brush::drawRect(glm::vec2 pos, glm::vec2 size, uint32_t color)
{  // 便利重载
    drawRect(pos, size, unpackColor(color));
}

}  // namespace MMM::Graphic::UI
