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

/// @brief 在按键边沿入队前同步该 Cocoa 事件自身的客户区位置。
/// @param window 当前 GLFW 窗口，事件必须确实属于对应 NSWindow。
/// @param cursorX 成功时接收左上角原点的逻辑 X 坐标。
/// @param cursorY 成功时接收左上角原点的逻辑 Y 坐标。
/// @return 原生事件有效且完成位置同步时为真；失败保留调用方的 GLFW 回退值。
/// @note ImGui 后端就绪时先排入位置，再由原按键 callback 排入按下/释放。
/// @warning 窗口输入回调路径：不移动系统光标，不等待，不手工合成点击。
bool syncMacOSMouseButtonPosition(GLFWwindow* window, double& cursorX,
                                  double& cursorY);

}  // namespace MMM::Graphic
