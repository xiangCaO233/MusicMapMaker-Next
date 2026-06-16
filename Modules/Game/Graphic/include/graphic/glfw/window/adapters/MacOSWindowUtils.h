#pragma once

#include "graphic/glfw/GLFWHeader.h"

namespace MMM::Graphic
{

/// @brief 将 macOS 原生窗口最小化到 Dock。
/// @param window GLFW 窗口句柄。
/// @return 成功请求最小化时返回 true。
bool miniaturizeMacOSWindow(GLFWwindow* window);

/// @brief 将 macOS 原生窗口调整到当前屏幕的可见工作区。
/// @param window GLFW 窗口句柄。
/// @return 成功应用可见工作区窗口矩形时返回 true。
bool applyMacOSVisibleWindowFrame(GLFWwindow* window);

/// @brief 将 macOS 原生窗口以留边尺寸居中到当前屏幕可见工作区。
/// @param window GLFW 窗口句柄。
/// @param requestedWidth 请求的内容宽度。
/// @param requestedHeight 请求的内容高度。
/// @return 成功设置窗口矩形时返回 true。
bool centerMacOSWindowInVisibleFrame(GLFWwindow* window, int requestedWidth,
                                     int requestedHeight);

/// @brief 允许 macOS 非活跃窗口的第一次鼠标点击直接交给 GLFW 内容视图。
/// @param window GLFW 窗口句柄。
/// @return 成功启用 first mouse 支持时返回 true。
bool enableMacOSFirstMouse(GLFWwindow* window);

}  // namespace MMM::Graphic
