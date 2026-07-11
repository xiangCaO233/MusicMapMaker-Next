#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建撤销动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createUndoAction();

/// @brief 创建重做动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createRedoAction();

/// @brief 创建剪切动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCutAction();

/// @brief 创建复制动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCopyAction();

/// @brief 创建粘贴动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createPasteAction();

/// @brief 创建镜像粘贴动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createMirrorPasteAction();

/// @brief 创建镜像选中音符动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createMirrorAction();

/// @brief 创建全选动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllAction();

/// @brief 创建打开音符元数据编辑器动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNoteMetadataAction();

/// @brief 创建播放暂停切换动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createTogglePlaybackAction();

/// @brief 创建打开谱面设置动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBeatmapSettingsAction();

}  // namespace MMM::UI
