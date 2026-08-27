#pragma once

#include "ui/ICanvasViewFactory.h"

namespace MMM::Canvas
{

/// @brief Canvas 模块提供的具体视图工厂，仅组合根需要知道该实现。
class CanvasViewFactory final : public UI::ICanvasViewFactory
{
public:
    /// @brief 创建具体主编辑画布。
    std::unique_ptr<UI::IUIView> createBasic2DCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
        const std::string&                        cameraId) override;

    /// @brief 创建具体预览画布。
    std::unique_ptr<UI::IUIView> createPreviewCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer) override;

    /// @brief 创建具体时间线画布。
    std::unique_ptr<UI::IUIView> createTimelineCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer) override;
};

}  // namespace MMM::Canvas
