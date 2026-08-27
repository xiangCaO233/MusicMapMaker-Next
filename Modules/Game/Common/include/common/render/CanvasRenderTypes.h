#pragma once

#include <cstdint>

namespace MMM::Common::Render
{

/// @brief 可由逻辑线程生成并由图形模块直接上传的画布顶点位置。
struct CanvasPosition {
    float x{};
    float y{};
    float z{ 0.0F };
};

/// @brief 画布顶点使用的线性 RGBA 颜色。
struct CanvasColor {
    float r{ 1.0F };
    float g{ 1.0F };
    float b{ 1.0F };
    float a{ 1.0F };
};

/// @brief 画布顶点使用的纹理坐标。
struct CanvasTexCoord {
    float u{};
    float v{};
};

/// @brief 跨逻辑与图形模块共享的固定布局画布顶点。
/// @warning 渲染热路径数据：字段顺序属于 GPU 顶点输入契约，修改后必须同步验证
/// Vulkan 顶点属性描述，禁止加入动态所有权成员。
struct CanvasVertex {
    CanvasPosition pos{};
    CanvasColor    color{};
    CanvasTexCoord uv{};
};

/// @brief 与图形 API 无关的画布裁剪矩形。
struct CanvasScissor {
    std::int32_t  x{};
    std::int32_t  y{};
    std::uint32_t width{};
    std::uint32_t height{};

    friend constexpr bool operator==(const CanvasScissor&,
                                     const CanvasScissor&) = default;
};

/// @brief 逻辑线程发布给画布渲染器的无图形 API 绘制指令。
/// @warning 渲染热路径数据：每个批次都会读取，禁止加入字符串、智能指针或
/// Vulkan 句柄；纹理必须通过稳定整数 ID 在消费端解析。
struct CanvasDrawCmd {
    std::uint32_t indexCount{};
    std::uint32_t indexOffset{};
    std::uint32_t vertexOffset{};
    std::uint32_t customTextureId{};
    CanvasScissor scissor{};
};

}  // namespace MMM::Common::Render
