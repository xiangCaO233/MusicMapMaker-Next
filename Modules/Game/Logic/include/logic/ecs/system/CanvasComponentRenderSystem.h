#pragma once

#include "config/visual/CanvasComponentConfig.h"
#include <array>
#include <cstdint>
#include <span>

namespace MMM::Logic
{
struct RenderSnapshot;
struct TimelineComponent;
}  // namespace MMM::Logic

namespace MMM::Logic::System
{

class ScrollCache;

/// @brief 主画布可选组件渲染所需的只读帧上下文。
struct CanvasComponentRenderContext {
    /// @brief 当前判定线时间，单位秒。
    double currentTime{ 0.0 };
    /// @brief 主画布宽度。
    float viewportWidth{ 0.0f };
    /// @brief 主画布高度。
    float viewportHeight{ 0.0f };
    /// @brief 判定线纵向像素坐标。
    float judgmentLineY{ 0.0f };
    /// @brief 轨道布局可见区域上边界。
    float visibleTop{ 0.0f };
    /// @brief 轨道布局可见区域下边界。
    float visibleBottom{ 0.0f };
    /// @brief 当前主画布纵向渲染缩放。
    float renderScaleY{ 1.0f };
    /// @brief 已排序且已缓存的 BPM 事件。
    std::span<const TimelineComponent* const> bpmEvents;
    /// @brief 当前会话的滚动坐标缓存。
    const ScrollCache* scrollCache{ nullptr };
};

/// @brief 将启用的可选组件生成到主画布最终 Vulkan 覆盖层。
///
/// 该系统是画布组件的统一渲染入口。新增组件时只需扩展组件类型、配置和本系统
/// 的类型分派。ASCII 字体在画布资源重载阶段独立栅格化并并入现有纹理图集，
/// 热渲染阶段只生成字形四边形，不依赖 ImGui。
class CanvasComponentRenderSystem final
{
public:
    /// @brief 生成全部已启用组件的最终覆盖层字形几何。
    /// @param snapshot 目标渲染快照。
    /// @param context 当前主画布的时间、视口与节拍坐标上下文。
    /// @param config 画布组件布局配置。
    /// @warning 热路径：每个主画布快照生成末尾执行；只允许遍历固定组件表、
    /// 已缓存 BPM 事件和可见拍号，禁止字体加载、文件系统访问、完整 ECS
    /// 遍历、阻塞操作或共享所有权复制。
    static void render(RenderSnapshot*                            snapshot,
                       const CanvasComponentRenderContext&        context,
                       const Config::CanvasComponentLayoutConfig& config);

    /// @brief 将判定线时间格式化为固定精度显示文本。
    /// @param currentTime 当前判定线时间，单位秒。
    /// @return `HH:MM:SS.mmm` 格式文本；负值保留前导负号。
    /// @warning 热路径：组件启用时每次快照生成调用；不得引入堆分配。
    [[nodiscard]] static std::array<char, 16> formatJudgmentLineTime(
        double currentTime);

    /// @brief 将一基拍号格式化为 ASCII 文本。
    /// @param beatIndex 从首个 BPM Timing 起算的一基拍号。
    /// @return 带 `#` 前缀的十进制拍号文本；非正值返回 `#0`。
    /// @warning 热路径：每个可见拍号实例调用；不得引入堆分配。
    [[nodiscard]] static std::array<char, 24> formatBeatNumber(
        std::int64_t beatIndex);
};

}  // namespace MMM::Logic::System
