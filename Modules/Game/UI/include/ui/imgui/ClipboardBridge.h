#pragma once

#include "imgui.h"
#include "logic/EditorEngine.h"

namespace MMM::UI::ClipboardBridge
{

/// @brief 将待发布的编辑器剪贴板文本写入系统剪贴板。
/// @warning UI 热路径：每帧调用一次；只消费一个小型 optional 字符串。
inline void publishPendingEditorClipboard()
{
    auto pendingText =
        Logic::EditorEngine::instance().consumePendingSystemClipboardText();
    if ( !pendingText ) {
        return;
    }

    ImGui::SetClipboardText(pendingText->c_str());
}

/// @brief 从当前系统剪贴板文本导入 MMM 载荷。
/// @warning UI 快捷键路径：只在粘贴命令前调用。
inline void importEditorClipboardFromSystem()
{
    const char* text = ImGui::GetClipboardText();
    if ( !text || text[0] == '\0' ) {
        return;
    }

    (void)Logic::EditorEngine::instance().importSystemClipboardText(text);
}

}  // namespace MMM::UI::ClipboardBridge
