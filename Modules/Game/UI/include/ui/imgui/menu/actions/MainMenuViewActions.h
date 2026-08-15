#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建时间线窗口显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createTimelineWindowToggleAction();

/// @brief 创建预览窗口显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createPreviewWindowToggleAction();

/// @brief 创建主画布批注详情显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createAnnotationDetailsToggleAction();

/// @brief 创建工具按钮文本显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createToolLabelsToggleAction();

/// @brief 创建固定工具窗口开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createFixedToolWindowToggleAction();

/// @brief 创建管理器标签显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createManagerLabelsToggleAction();

}  // namespace MMM::UI
