#include "game/CanvasWorkspaceService.h"

#include "canvas/AnnotationTableWindow.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/PreviewCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include <mutex>

namespace MMM::Game
{

void CanvasWorkspaceService::fillEntries(
    std::vector<UI::CanvasWorkspaceEntry>& entries)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());

    const std::int32_t entryCount = engine.getSessionCount();
    entries.resize(static_cast<std::size_t>(entryCount));
    for ( std::int32_t index = 0; index < entryCount; ++index ) {
        const auto* entry = engine.getSessionEntry(index);
        if ( !entry ) {
            entries.resize(static_cast<std::size_t>(index));
            return;
        }
        auto& target             = entries[static_cast<std::size_t>(index)];
        target.cameraId          = entry->cameraId;
        target.isLogoPlaceholder = entry->isLogoPlaceholder;
        target.restoreDockFromWorkspace = entry->restoreDockFromWorkspace;
    }
}

std::int32_t CanvasWorkspaceService::getActiveEntryIndex() const
{
    return Logic::EditorEngine::instance().getActiveSessionIndex();
}

bool CanvasWorkspaceService::hasPendingProjectSwitch() const
{
    return Logic::ProjectController::instance().hasPendingProjectSwitch();
}

void CanvasWorkspaceService::saveProject()
{
    Logic::EditorEngine::instance().saveProject();
}

void CanvasWorkspaceService::createLogoPlaceholderSession(
    const std::string& displayName)
{
    Logic::EditorEngine::instance().createSession(nullptr, displayName, true);
}

void CanvasWorkspaceService::closeSession(std::int32_t index,
                                          bool         updateWorkspace)
{
    Logic::EditorEngine::instance().closeSession(index, updateWorkspace);
}

std::int32_t CanvasWorkspaceService::getEntryCount() const
{
    return Logic::EditorEngine::instance().getSessionCount();
}

std::int32_t CanvasWorkspaceService::consumePendingFocusIndex()
{
    return Logic::EditorEngine::instance().consumePendingFocusSessionIndex();
}

void CanvasWorkspaceService::requestEntryFocus(std::int32_t index)
{
    Logic::EditorEngine::instance().requestSessionFocus(index);
}

std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createMainCanvas(
    const UI::CanvasWorkspaceEntry& entry, std::uint32_t width,
    std::uint32_t height)
{
    return std::make_unique<Canvas::Basic2DCanvas>(
        entry.cameraId,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer(entry.cameraId),
        entry.cameraId);
}

std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createPreviewCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height)
{
    return std::make_unique<Canvas::PreviewCanvas>(
        name,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer("Preview"));
}

std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createTimelineCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height)
{
    return std::make_unique<Canvas::TimelineCanvas>(
        name,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer("Timeline"));
}

std::unique_ptr<UI::IUIView>
CanvasWorkspaceService::createAnnotationTableWindow(const std::string& name)
{
    return std::make_unique<Canvas::AnnotationTableWindow>(name);
}

}  // namespace MMM::Game
