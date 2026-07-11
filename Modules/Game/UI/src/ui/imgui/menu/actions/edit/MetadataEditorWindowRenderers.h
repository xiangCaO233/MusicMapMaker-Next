#pragma once

namespace MMM::UI
{

/// @brief 渲染谱面元数据编辑窗口。
/// @param showWindow 窗口开关状态，由触发该窗口的 action 持有。
/// @warning UI 热路径：每帧执行；仅在窗口打开时访问当前谱面元数据。
void renderMetadataEditorWindow(bool& showWindow);

/// @brief 渲染选中音符元数据编辑窗口。
/// @param showWindow 窗口开关状态，由触发该窗口的 action 持有。
/// @warning UI 热路径：每帧执行；仅在窗口打开时访问选中音符元数据。
void renderNoteMetadataEditorWindow(bool& showWindow);

}  // namespace MMM::UI
