#pragma once

#include "imgui.h"
#include "logic/EditorEngine.h"

namespace MMM::UI::ClipboardBridge
{

/// @brief 将待发布的编辑器剪贴板文本写入系统剪贴板。
/// @details 编辑逻辑线程先把序列化文本放入单槽队列，UI 线程在拥有 ImGui
/// 平台后端的帧内消费并发布到操作系统。
/// @warning UI 热路径：每帧调用一次；只消费一个小型 optional 字符串。
inline void publishPendingEditorClipboard()
{
    // consume 同时清空待发布槽，避免相同内容每帧重复写入系统剪贴板。
    auto pendingText =
        Logic::EditorEngine::instance().consumePendingSystemClipboardText();
    if ( !pendingText ) {
        // 没有编辑命令产生新载荷时不接触平台剪贴板。
        return;
    }

    // ImGui 后端负责把 UTF-8 文本转换为平台剪贴板格式。
    ImGui::SetClipboardText(pendingText->c_str());
}

/// @brief 从当前系统剪贴板文本导入 MMM 载荷。
/// @details 只把非空 UTF-8 文本交给编辑器解析；普通文本或损坏载荷由逻辑层
/// 返回失败，本桥接层不展示提示也不修改编辑状态。
/// @warning UI 快捷键路径：只在粘贴命令前调用。
inline void importEditorClipboardFromSystem()
{
    const char* text = ImGui::GetClipboardText();
    if ( !text || text[0] == '\0' ) {
        // 平台后端不可用与空剪贴板都视为没有可导入载荷。
        return;
    }

    // 导入结果由后续粘贴命令读取，此处仅完成系统到编辑器的同步。
    (void)Logic::EditorEngine::instance().importSystemClipboardText(text);
}

}  // namespace MMM::UI::ClipboardBridge
