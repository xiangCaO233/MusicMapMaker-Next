#pragma once

#include "ui/IEditorApplicationService.h"

namespace MMM::Game
{

/// @brief 组合根中的编辑器应用适配器，桥接 UIManager 与 Logic/Audio 服务。
class EditorApplicationService final : public UI::IEditorApplicationService
{
public:
    /// @copydoc UI::IEditorApplicationService::currentProjectUiSnapshot
    [[nodiscard]] bool currentProjectUiSnapshot(
        UI::EditorProjectUiSnapshot& snapshot) const override;

    /// @copydoc UI::IEditorApplicationService::mutableCurrentWorkspace
    [[nodiscard]] ProjectWorkspaceState* mutableCurrentWorkspace(
        const std::filesystem::path& expectedProjectRoot) override;

    /// @copydoc UI::IEditorApplicationService::markProjectAudioToolOpenAndSave
    void markProjectAudioToolOpenAndSave() override;

    /// @copydoc UI::IEditorApplicationService::requestAutoSave
    void requestAutoSave(UI::EditorAutoSaveReason reason) override;

    /// @copydoc UI::IEditorApplicationService::publishRenderFps
    void publishRenderFps(float fps) override;

    /// @copydoc UI::IEditorApplicationService::ensureEffectAudioTrackLoaded
    [[nodiscard]] bool ensureEffectAudioTrackLoaded(
        const std::string& trackId) override;
};

}  // namespace MMM::Game
