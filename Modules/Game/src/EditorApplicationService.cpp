#include "game/EditorApplicationService.h"

#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"

namespace MMM::Game
{

bool EditorApplicationService::currentProjectUiSnapshot(
    UI::EditorProjectUiSnapshot& snapshot) const
{
    const auto* project = Logic::ProjectController::instance().currentProject();
    if ( !project ) {
        return false;
    }

    snapshot.projectRoot    = project->m_projectRoot;
    snapshot.workspace      = project->m_settings.m_workspace;
    snapshot.audioResources = project->m_audioResources;
    return true;
}

ProjectWorkspaceState* EditorApplicationService::mutableCurrentWorkspace(
    const std::filesystem::path& expectedProjectRoot)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.lexically_normal() !=
                         expectedProjectRoot.lexically_normal() ) {
        return nullptr;
    }
    return &project->m_settings.m_workspace;
}

void EditorApplicationService::markProjectAudioToolOpenAndSave()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) {
        return;
    }
    project->m_settings.m_workspace.m_projectAudioToolOpen = true;
    engine.saveProject();
}

Logic::EditTool EditorApplicationService::currentTool() const
{
    return Logic::EditorEngine::instance().getCurrentTool();
}

Config::EditorConfig EditorApplicationService::editorConfig() const
{
    return Logic::EditorEngine::instance().getEditorConfig();
}

void EditorApplicationService::updateEditorConfig(
    const Config::EditorConfig& config)
{
    Logic::EditorEngine::instance().setEditorConfig(config);
}

bool EditorApplicationService::isPlaybackPlaying() const
{
    return Logic::EditorEngine::instance().isPlaybackPlaying();
}

bool EditorApplicationService::isSelectingMarquee() const
{
    return Logic::EditorEngine::instance().isActiveSessionSelectingMarquee();
}

bool EditorApplicationService::isDraggingNote() const
{
    return Logic::EditorEngine::instance().isActiveSessionDraggingNote();
}

bool EditorApplicationService::isDrawingBrush() const
{
    return Logic::EditorEngine::instance().isActiveSessionDrawingBrush();
}

void EditorApplicationService::requestAutoSave(UI::EditorAutoSaveReason reason)
{
    const auto trigger =
        reason == UI::EditorAutoSaveReason::ImGuiWindowFocusLost
            ? Logic::AutoSaveTrigger::ImGuiWindowFocusLost
            : Logic::AutoSaveTrigger::NativeWindowFocusLost;
    Logic::EditorEngine::instance().requestAutoSaveForActiveSession(trigger);
}

void EditorApplicationService::publishRenderFps(float fps)
{
    Logic::EditorEngine::instance().publishRenderFps(fps);
}

bool EditorApplicationService::ensureEffectAudioTrackLoaded(
    const std::string& trackId)
{
    auto& audio = Audio::AudioManager::instance();
    if ( audio.isSoundEffectLoaded(trackId) ) {
        return true;
    }

    const auto* project = Logic::ProjectController::instance().currentProject();
    if ( project ) {
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id != trackId ||
                 resource.m_type != AudioTrackType::Effect ) {
                continue;
            }

            const auto absolutePath =
                project->m_projectRoot / Config::utf8ToPath(resource.m_path);
            audio.registerSoundEffect(
                trackId, Config::pathToUtf8(absolutePath), resource.m_config);
            return audio.ensureSoundEffectLoaded(trackId);
        }
    }

    const auto& skinData = Config::SkinManager::instance().getData();
    if ( auto path = skinData.audioPaths.find(trackId);
         path != skinData.audioPaths.end() ) {
        audio.registerSoundEffect(trackId,
                                  Config::pathToUtf8(path->second),
                                  audio.getSFXPoolVolume(trackId));
    }
    return audio.ensureSoundEffectLoaded(trackId);
}

}  // namespace MMM::Game
