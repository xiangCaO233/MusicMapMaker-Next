#pragma once

#include <nfd.h>

struct GLFWwindow;

namespace MMM::UI::NativeFileDialog
{

/// @brief 绑定全部原生文件选择器使用的 GLFW 主窗口。
/// @param window 主窗口观察指针；窗口销毁前应传入 nullptr 解除绑定。
/// @warning 仅在 UI 组合根初始化或销毁时调用；指针生命周期由 GameLoop 管理。
void bindMainWindow(GLFWwindow* window);

/// @brief 打开带统一父窗口和前台激活处理的原生文件选择器。
/// @param outPath 输出 UTF-8 文件路径。
/// @param filters 文件类型过滤器，可为空。
/// @param filterCount 过滤器数量。
/// @param defaultPath 默认目录，可为空。
/// @return NFD 对话框结果。
/// @warning 用户触发的低频阻塞路径；必须在 UI 线程调用。
[[nodiscard]] nfdresult_t openFile(nfdu8char_t**            outPath,
                                   const nfdu8filteritem_t* filters,
                                   nfdfiltersize_t          filterCount,
                                   const nfdu8char_t*       defaultPath);

/// @brief 打开带统一父窗口和前台激活处理的原生保存选择器。
/// @param outPath 输出 UTF-8 文件路径。
/// @param filters 文件类型过滤器，可为空。
/// @param filterCount 过滤器数量。
/// @param defaultPath 默认目录，可为空。
/// @param defaultName 默认文件名，可为空。
/// @return NFD 对话框结果。
/// @warning 用户触发的低频阻塞路径；必须在 UI 线程调用。
[[nodiscard]] nfdresult_t saveFile(nfdu8char_t**            outPath,
                                   const nfdu8filteritem_t* filters,
                                   nfdfiltersize_t          filterCount,
                                   const nfdu8char_t*       defaultPath,
                                   const nfdu8char_t*       defaultName);

/// @brief 打开带统一父窗口和前台激活处理的原生文件夹选择器。
/// @param outPath 输出 UTF-8 文件夹路径。
/// @param defaultPath 默认目录，可为空。
/// @return NFD 对话框结果。
/// @warning 用户触发的低频阻塞路径；必须在 UI 线程调用。
[[nodiscard]] nfdresult_t pickFolder(nfdu8char_t**      outPath,
                                     const nfdu8char_t* defaultPath);

}  // namespace MMM::UI::NativeFileDialog
