#pragma once

#include <cstdint>

namespace MMM::Common::Render
{

/// @brief 可由逻辑线程生成并由图形模块直接上传的画布顶点位置。
/// @note 坐标单位和投影含义由具体画布决定，该 DTO 只固定字段布局。
struct CanvasPosition {
    float x{};
    float y{};
    float z{ 0.0F };
};

/// @brief 画布顶点使用的线性 RGBA 颜色。
/// @note 分量不在结构内钳制，生产者负责提供渲染管线可接受的范围。
struct CanvasColor {
    float r{ 1.0F };
    float g{ 1.0F };
    float b{ 1.0F };
    float a{ 1.0F };
};

/// @brief 画布顶点使用的纹理坐标。
/// @note 坐标是否归一化由纹理采样契约决定。
struct CanvasTexCoord {
    float u{};
    float v{};
};

/// @brief 跨逻辑与图形模块共享的固定布局画布顶点。
/// @warning 渲染热路径数据：字段顺序属于 GPU 顶点输入契约，修改后必须同步验证
/// Vulkan 顶点属性描述，禁止加入动态所有权成员。
struct CanvasVertex {
    /// @brief 顶点在画布坐标系中的位置。
    CanvasPosition pos{};
    /// @brief 与纹理采样结果相乘的顶点颜色。
    CanvasColor color{};
    /// @brief 顶点对应的纹理坐标。
    CanvasTexCoord uv{};
};

/// @brief 与图形 API 无关的画布裁剪矩形。
/// @note 原点使用有符号坐标，尺寸使用无符号值，便于映射 Vulkan scissor。
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
    /// @brief 本批次需要读取的索引数量。
    std::uint32_t indexCount{};
    /// @brief 本批次在共享索引缓冲区中的起始偏移。
    std::uint32_t indexOffset{};
    /// @brief 索引解释时叠加的顶点缓冲区基址。
    std::uint32_t vertexOffset{};
    /// @brief 由消费端解析的稳定自定义纹理 ID；零值表示默认纹理。
    std::uint32_t customTextureId{};
    /// @brief 限制本批次片元输出的画布裁剪区域。
    CanvasScissor scissor{};
    /// @brief 按源 Alpha 加权的加法混合；不同模式禁止合并批次。
    bool additiveBlend{ false };
};

}  // namespace MMM::Common::Render
