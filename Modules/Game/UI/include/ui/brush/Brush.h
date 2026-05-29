#pragma once

#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "ui/brush/BrushDrawCmd.h"
#include <glm/glm.hpp>
#include <vector>

namespace MMM::UI
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

    /// @brief 清理顶点缓存和绘制指令
    /// @warning 热路径：每帧 RenderContext 构建时执行；只清空内存缓存，禁止释放
    /// GPU 资源。
    void clear();

    // ==========================================
    // 状态机 API (QPainter 风格)
    // ==========================================

    /// @brief 设置绘制颜色
    void setColor(const glm::vec4& color);
    void setColor(float r, float g, float b, float a = 1.0f);
    void setColor(uint32_t colorHex);  // 0xRRGGBBAA

    /// @brief 设置绑定的纹理 (传递 VK_NULL_HANDLE 代表纯色)
    void setTexture(vk::DescriptorSet texture);

    // ==========================================
    // 绘制 API
    // ==========================================

    /// @brief 绘制实心矩形
    /// @warning 热路径：快照生成和 UI
    /// 绘制期间高频调用；只追加顶点/索引，不得访问文件系统或执行排序。
    void drawRect(float x, float y, float w, float h);
    /// @brief 绘制实心矩形
    /// @warning 热路径：委托到坐标版本；不得引入额外分配。
    void drawRect(glm::vec2 pos, glm::vec2 size);

    // 兼容旧版便利方法 (带颜色，内部会修改状态机并恢复)
    /// @brief 兼容旧版便利方法，绘制指定颜色矩形。
    /// @warning
    /// 热路径：内部会修改并恢复状态机；不得用于引入纹理或资源生命周期变更。
    void drawRect(glm::vec2 pos, glm::vec2 size, glm::vec4 color);
    /// @brief 兼容旧版便利方法，绘制指定 16 进制颜色矩形。
    /// @warning 热路径：内部会修改并恢复状态机；不得用于引入文件系统访问。
    void drawRect(glm::vec2 pos, glm::vec2 size, uint32_t color);

    /// @brief 绘制实心圆形
    /// @warning 热路径：按 segments 追加几何；调用方必须限制
    /// segments，避免每帧过量顶点生成。
    void drawCircle(float cx, float cy, float radius, int segments = 36);

    // --- 获取数据供 Vulkan 使用 ---
    inline const std::vector<Graphic::Vertex::VKBasicVertex>&
    getVertices() const
    {
        return m_vertices;
    }
    inline const std::vector<uint32_t>& getIndices() const { return m_indices; }
    inline const std::vector<BrushDrawCmd>& getCmds() const { return m_cmds; }

private:
    /// @brief 添加或更新当前的 DrawCmd
    /// @warning 热路径：每次追加几何时执行；只合并相邻状态一致的 DrawCmd。
    void updateDrawCmd(uint32_t indicesCount);

    /// @brief 辅助函数：解包 16 进制颜色
    static glm::vec4 unpackColor(uint32_t color);

    // ==========================================
    // 内部状态
    // ==========================================
    glm::vec4         m_currentColor{ 1.0f };              // 默认白色
    vk::DescriptorSet m_currentTexture{ VK_NULL_HANDLE };  // 默认无纹理

    // ==========================================
    // 缓存数据
    // ==========================================
    std::vector<Graphic::Vertex::VKBasicVertex> m_vertices;
    std::vector<uint32_t>                       m_indices;
    std::vector<BrushDrawCmd>                   m_cmds;
};
}  // namespace MMM::UI
