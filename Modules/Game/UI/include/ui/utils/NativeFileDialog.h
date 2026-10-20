#pragma once

#include <functional>
#include <nfd.h>
#include <string>

#include "graphic/window/WindowPlatform.h"

namespace MMM::UI::NativeFileDialog
{

/// @brief 绑定全部原生文件选择器使用的 GLFW 主窗口。
/// @param window 主窗口观察指针；窗口销毁前应传入 nullptr 解除绑定。
/// @warning 仅在 UI 组合根初始化或销毁时调用；指针生命周期由 GameLoop 管理。
void bindMainWindow(Graphic::PlatformWindowHandle* window);

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

/// @brief 完成回调在有效 ImGui UI 帧执行，路径值不借用 Java 或 NFD 临时内存。
using DialogCompletion = std::function<void(nfdresult_t, const std::string&)>;
/// @brief 打开选择器前确认平台访问方式，并为已授权兼容模式准备共享根目录。
/// @details Android 原生样式使用 SAF；统一样式仅在拥有用户授权时直接访问目录。
/// @warning 用户触发的低频操作：Android 查询当前权限，禁止在每帧调用。
#ifdef __ANDROID__
bool requiresSystemPicker();
#else
constexpr bool requiresSystemPicker()
{
    return false;
}
#endif
/// @brief 异步打开单文件，Android 完成授权和工作副本准备后回调。
/// @warning 用户触发低频路径；回调捕获的观察对象必须存活至选择完成。
void openFileAsync(const nfdu8filteritem_t* filters, nfdfiltersize_t count,
                   const char* directory, DialogCompletion completion);
/// @brief 异步选择保存目标，writer 必须确认文档发布成功后才报告保存成功。
void saveFileAsync(const nfdu8filteritem_t* filters, nfdfiltersize_t count,
                   const char* directory, const char* name,
                   DialogCompletion completion);
/// @brief 异步授权工程目录；返回普通工作副本路径，URI 留在平台层管理。
void pickFolderAsync(const char* directory, DialogCompletion completion);
/// @brief 由 UI 组合根消费在途选择，取消时不执行文件操作或改变工程。
/// @warning 每帧入口；无请求立即返回，不等待授权或后台 I/O。
void pollAsyncDialogs();

}  // namespace MMM::UI::NativeFileDialog
