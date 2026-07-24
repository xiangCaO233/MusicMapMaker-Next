#pragma once

#include "config/visual/CanvasComponentConfig.h"
#include <array>

namespace MMM::Logic
{
struct RenderSnapshot;
}

namespace MMM::Logic::System
{

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
    /// @param currentTime 当前判定线时间，单位秒。
    /// @param viewportWidth 主画布宽度。
    /// @param viewportHeight 主画布高度。
    /// @param config 画布组件布局配置。
    /// @warning 热路径：每个主画布快照生成末尾执行；只允许遍历固定组件表和
    /// 短 ASCII 文本，禁止字体加载、文件系统访问、完整 ECS 遍历、阻塞操作或
    /// 共享所有权复制。
    static void render(RenderSnapshot* snapshot, double currentTime,
                       float viewportWidth, float viewportHeight,
                       const Config::CanvasComponentLayoutConfig& config);

    /// @brief 将判定线时间格式化为固定精度显示文本。
    /// @param currentTime 当前判定线时间，单位秒。
    /// @return `HH:MM:SS.mmm` 格式文本；负值保留前导负号。
    /// @warning 热路径：组件启用时每次快照生成调用；不得引入堆分配。
    [[nodiscard]] static std::array<char, 16> formatJudgmentLineTime(
        double currentTime);
};

}  // namespace MMM::Logic::System
