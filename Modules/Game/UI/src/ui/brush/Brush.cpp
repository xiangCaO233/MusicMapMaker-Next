#include "ui/brush/Brush.h"
#include <cmath>

/// @file Brush.cpp
/// @brief Brush 绘制状态、矩形与圆形 CPU 几何生成及纹理批次合并实现。
/// @details 所有绘制函数只追加顶点、绝对索引和 BrushDrawCmd；GPU 上传与命令
/// 录制由可渲染视图在后续阶段完成。

namespace MMM::UI
{

/// @brief 清理上一帧的顶点、索引和绘制指令。
/// @warning 渲染热路径：每帧开始调用，保留容器容量以避免重复分配。
void Brush::clear()
{
    // 三个数组共同描述同一帧，必须同步清空以维持偏移一致。
    m_vertices.clear();
    m_indices.clear();
    m_cmds.clear();
}

/// @brief 设置后续几何使用的 RGBA 浮点颜色。
/// @param color 各通道通常位于 0 到 1 范围的颜色。
/// @warning 绘制热路径：只更新当前状态，不产生几何或批次。
void Brush::setColor(const glm::vec4& color)
{
    m_currentColor = color;
}

/// @brief 分通道设置后续几何使用的 RGBA 浮点颜色。
/// @param r 红色通道。
/// @param g 绿色通道。
/// @param b 蓝色通道。
/// @param a 透明度通道。
void Brush::setColor(float r, float g, float b, float a)
{
    m_currentColor = glm::vec4(r, g, b, a);
}

/// @brief 从 0xRRGGBBAA 打包值设置后续几何颜色。
/// @param colorHex 按红、绿、蓝、透明度排列的八位通道值。
void Brush::setColor(uint32_t colorHex)
{
    m_currentColor = unpackColor(colorHex);
}

/// @brief 设置后续几何绑定的 Vulkan 纹理描述符。
/// @param texture 描述符集合；空句柄表示使用默认纯色纹理。
/// @note 状态变化本身不创建空批次，只有追加索引时才写入命令。
void Brush::setTexture(vk::DescriptorSet texture)
{
    // updateDrawCmd 在下一次几何提交时比较状态，因此此处只更新当前纹理。
    if ( m_currentTexture != texture ) {
        m_currentTexture = texture;
    }
}

/// @brief 将新追加的索引范围合并到兼容批次或建立新批次。
/// @param indicesCount 本次几何刚追加的索引数量。
/// @warning 绘制热路径：每个图元调用，只检查最后一条命令，不遍历历史批次。
void Brush::updateDrawCmd(uint32_t indicesCount)
{
    if ( m_cmds.empty() ) {
        // 首个图元从共享索引缓冲起点建立第一条命令。
        BrushDrawCmd cmd;
        cmd.indexOffset  = 0;
        cmd.vertexOffset = 0;
        cmd.indexCount   = indicesCount;
        cmd.texture      = m_currentTexture;
        m_cmds.push_back(cmd);
        return;
    }

    // 只有最后批次可能与当前连续索引范围合并。
    auto& lastCmd = m_cmds.back();
    // 相同纹理代表渲染状态兼容，直接扩展连续索引数量。
    if ( lastCmd.texture == m_currentTexture ) {
        lastCmd.indexCount += indicesCount;
    } else {
        // 纹理切换要求新命令，避免一次 drawIndexed 中途重新绑定描述符。
        BrushDrawCmd cmd;
        // 新批次紧接上一批次的连续索引范围。
        cmd.indexOffset = lastCmd.indexOffset + lastCmd.indexCount;
        // 索引值相对整个顶点数组，因此 Vulkan 基础顶点始终为零。
        cmd.vertexOffset = 0;
        cmd.indexCount   = indicesCount;
        cmd.texture      = m_currentTexture;
        m_cmds.push_back(cmd);
    }
}

/// @brief 以当前颜色和纹理追加一个轴对齐实心矩形。
/// @param x 左上角横坐标。
/// @param y 左上角纵坐标。
/// @param w 矩形宽度。
/// @param h 矩形高度。
/// @warning 绘制热路径：固定追加四个顶点和六个索引。
void Brush::drawRect(float x, float y, float w, float h)
{
    // 新索引必须指向本次追加前的顶点数组末尾。
    uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());

    // 四角按左上、右上、左下、右下顺序写入，与索引绕序一致。
    // 坐标用于 Y 轴已翻转的正交投影，UV 则覆盖完整纹理区域。
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

    // 第一个三角形连接左上、右上和左下。
    m_indices.push_back(baseIndex + 0);
    m_indices.push_back(baseIndex + 1);
    m_indices.push_back(baseIndex + 2);
    // 第二个三角形连接右上、右下和左下，补全矩形。
    m_indices.push_back(baseIndex + 1);
    m_indices.push_back(baseIndex + 3);
    m_indices.push_back(baseIndex + 2);

    // 六个连续索引按当前纹理状态并入绘制批次。
    updateDrawCmd(6);
}

/// @brief 使用向量参数追加轴对齐矩形。
/// @param pos 左上角坐标。
/// @param size 矩形宽高。
void Brush::drawRect(glm::vec2 pos, glm::vec2 size)
{
    drawRect(pos.x, pos.y, size.x, size.y);
}

/// @brief 将 0xRRGGBBAA 打包颜色转换为浮点向量。
/// @param color 四个八位通道按高位到低位排列的颜色。
/// @return 各通道归一化到 0 到 1 的 RGBA 向量。
glm::vec4 Brush::unpackColor(uint32_t color)
{
    return glm::vec4(((color >> 24) & 0xFF) / 255.0f,
                     ((color >> 16) & 0xFF) / 255.0f,
                     ((color >> 8) & 0xFF) / 255.0f,
                     (color & 0xFF) / 255.0f);
}

/// @brief 使用临时浮点颜色追加矩形，并恢复调用前颜色状态。
/// @param pos 左上角坐标。
/// @param size 矩形宽高。
/// @param color 仅本图元使用的 RGBA 颜色。
void Brush::drawRect(glm::vec2 pos, glm::vec2 size, glm::vec4 color)
{
    // Brush 是有状态对象，临时重载不能影响调用方后续图元。
    glm::vec4 oldColor = m_currentColor;
    setColor(color);
    drawRect(pos.x, pos.y, size.x, size.y);
    setColor(oldColor);
}

/// @brief 使用临时打包颜色追加矩形，并恢复调用前颜色状态。
/// @param pos 左上角坐标。
/// @param size 矩形宽高。
/// @param color 仅本图元使用的 0xRRGGBBAA 颜色。
void Brush::drawRect(glm::vec2 pos, glm::vec2 size, uint32_t color)
{
    // 颜色保存与恢复使便捷重载不会改变 Brush 持久绘制状态。
    glm::vec4 oldColor = m_currentColor;
    setColor(color);
    drawRect(pos.x, pos.y, size.x, size.y);
    setColor(oldColor);
}

/// @brief 以三角形扇追加近似实心圆。
/// @param cx 圆心横坐标。
/// @param cy 圆心纵坐标。
/// @param radius 圆半径。
/// @param segments 圆周分段数，小于三时提升为三。
/// @warning 绘制热路径：追加数量与 segments 成正比，调用方应限制分段数。
void Brush::drawCircle(float cx, float cy, float radius, int segments)
{
    // 三角形扇至少需要三个边缘点才能构成封闭面积。
    if ( segments < 3 ) segments = 3;

    // 中心和边缘索引都相对追加前的顶点末尾计算。
    uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());

    // 圆心 UV 位于纹理中心，颜色使用当前 Brush 状态。
    m_vertices.push_back({ { cx, cy, 0.0f },
                           { m_currentColor.r,
                             m_currentColor.g,
                             m_currentColor.b,
                             m_currentColor.a },
                           { 0.5f, 0.5f } });

    // 圆周等角分段，最后一个点不重复起点。
    float angleStep = (2.0f * 3.1415926535f) / segments;
    for ( int i = 0; i < segments; ++i ) {
        float angle = i * angleStep;
        float x     = cx + radius * std::cos(angle);
        float y     = cy + radius * std::sin(angle);

        // 将单位圆坐标从负一到一映射到纹理零到一范围。
        float u = 0.5f + 0.5f * std::cos(angle);
        float v = 0.5f + 0.5f * std::sin(angle);

        m_vertices.push_back({ { x, y, 0.0f },
                               { m_currentColor.r,
                                 m_currentColor.g,
                                 m_currentColor.b,
                                 m_currentColor.a },
                               { u, v } });
    }

    // 每段生成中心、当前边缘、下一边缘组成的三角形。
    for ( int i = 0; i < segments; ++i ) {
        m_indices.push_back(baseIndex);          // 圆心。
        m_indices.push_back(baseIndex + 1 + i);  // 当前边缘点。
        // 最后一段回到首个边缘点，闭合三角形扇。
        uint32_t nextIdx = (i == segments - 1) ? 1 : (i + 2);
        m_indices.push_back(baseIndex + nextIdx);
    }

    // 每个分段贡献三个索引，并按当前纹理状态合并批次。
    updateDrawCmd(segments * 3);
}

}  // namespace MMM::UI
