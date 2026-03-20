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
void Brush::drawRect(glm::vec2 min, glm::vec2 max, uint32_t color,
                     float thickness)
{
}

}  // namespace MMM::Graphic::UI
