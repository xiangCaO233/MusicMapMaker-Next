#pragma once

// 平台句柄只用于窗口、Vulkan 与平台后端之间借用，不转移所有权。
#ifdef __ANDROID__
struct SDL_Window;
#else
struct GLFWwindow;
#endif

namespace MMM::Graphic
{
#ifdef __ANDROID__
/// @brief Android 的窗口由 SDLActivity 提供并由 SDL3 管理。
using PlatformWindowHandle = SDL_Window;
#else
/// @brief 桌面维持现有 GLFW 窗口 ABI。
using PlatformWindowHandle = GLFWwindow;
#endif
/// @brief 读取主视口的关闭请求；参数遵循当前 ImGui 平台后端的句柄约定。
/// @warning UI 热路径：只读取窗口状态，不分配、不访问文件或等待逻辑线程。
bool platformWindowShouldClose(void* platformHandle);
/// @brief 提交或撤销主视口关闭请求，供未保存项目确认流程使用。
/// @param requested true 表示用户已经确认关闭，false 表示继续编辑。
void setPlatformWindowShouldClose(void* platformHandle, bool requested);
}  // namespace MMM::Graphic
