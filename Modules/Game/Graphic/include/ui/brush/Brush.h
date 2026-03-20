#pragma once

#include "BrushDrawCmd.h"
#include "graphic/imguivk/mesh/VKVertex.h"
#include <glm/fwd.hpp>

namespace MMM::Graphic::UI
{

class Brush
{
public:
    Brush()                        = default;
    Brush(Brush&&)                 = default;
    Brush(const Brush&)            = default;
    Brush& operator=(Brush&&)      = default;
    Brush& operator=(const Brush&) = default;
    ~Brush()                       = default;

    /// @brief 清理顶点缓存
    void clear();

    /// @brief 绘制一个矩形
    void drawRect(glm::vec2 min, glm::vec2 max, uint32_t color,
                  float thickness = 1.0f);

    // --- 获取数据供 Vulkan 使用 ---
    inline const std::vector<VKVertex>& getVertices() const
    {
        return m_vertices;
    }
    inline const std::vector<uint32_t>& getIndices() const { return m_indices; }
    inline const std::vector<DrawCmd>&  getCmds() const { return m_cmds; }

private:
    /// @brief 顶点缓存
    std::vector<VKVertex> m_vertices;

    /// @brief 顶点索引缓存
    std::vector<uint32_t> m_indices;

    /// @brief 绘制指令缓存
    std::vector<DrawCmd> m_cmds;
};
}  // namespace MMM::Graphic::UI
