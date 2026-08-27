#include "canvas/CanvasViewFactory.h"

#include "canvas/Basic2DCanvas.h"
#include "canvas/PreviewCanvas.h"
#include "canvas/TimelineCanvas.h"

namespace MMM::Canvas
{

std::unique_ptr<UI::IUIView> CanvasViewFactory::createBasic2DCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
    const std::string&                        cameraId)
{
    return std::make_unique<Basic2DCanvas>(
        name, width, height, std::move(syncBuffer), cameraId);
}

std::unique_ptr<UI::IUIView> CanvasViewFactory::createPreviewCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer)
{
    return std::make_unique<PreviewCanvas>(
        name, width, height, std::move(syncBuffer));
}

std::unique_ptr<UI::IUIView> CanvasViewFactory::createTimelineCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer)
{
    return std::make_unique<TimelineCanvas>(
        name, width, height, std::move(syncBuffer));
}

}  // namespace MMM::Canvas
