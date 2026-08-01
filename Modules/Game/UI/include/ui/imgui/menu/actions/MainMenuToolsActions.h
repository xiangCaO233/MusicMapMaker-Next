#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建打开 BPM 测量工具动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBpmMeasurementAction();

/// @brief 创建切换重叠检测窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleOverlapCheckWindowAction();

/// @brief 创建切换谱面额外元数据编辑窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleMetadataEditorWindowAction();

/// @brief 创建打开数据来源替换工具动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenDataSourceReplaceWindowAction();

/// @brief 创建选中音符节拍对齐动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createAlignSelectedToCommonBeatsAction();

/// @brief 创建打开谱面倍速制作窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenBeatmapSpeedExportAction();

/// @brief 创建打开插件列表窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginListAction();

/// @brief 创建重载全部插件实例动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createReloadPluginsAction();

}  // namespace MMM::UI
