#pragma once

#include <glm/glm.hpp>

namespace MMM::Logic
{

/**
 * @brief 位置与变换组件
 */
struct TransformComponent {
    /// @brief 坐标 (x, y)
    glm::vec2 m_pos{ 0.0f, 0.0f };

    /// @brief 尺寸 (w, h)
    glm::vec2 m_size{ 0.0f, 0.0f };
};

}  // namespace MMM::Logic