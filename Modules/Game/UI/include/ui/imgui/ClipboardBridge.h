#pragma once

#include "imgui.h"
#include "logic/EditorEngine.h"

namespace MMM::UI::ClipboardBridge
{

/// @brief Publish pending editor clipboard text to the system clipboard.
/// @warning UI hot path: called once per frame; it only consumes a small
/// optional string.
inline void publishPendingEditorClipboard()
{
    auto pendingText =
        Logic::EditorEngine::instance().consumePendingSystemClipboardText();
    if ( !pendingText ) {
        return;
    }

    ImGui::SetClipboardText(pendingText->c_str());
}

/// @brief Import an MMM payload from the current system clipboard text.
/// @warning UI shortcut path: called only before paste commands.
inline void importEditorClipboardFromSystem()
{
    const char* text = ImGui::GetClipboardText();
    if ( !text || text[0] == '\0' ) {
        return;
    }

    (void)Logic::EditorEngine::instance().importSystemClipboardText(text);
}

}  // namespace MMM::UI::ClipboardBridge
