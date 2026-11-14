#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace MMM::Graphic
{
/// @brief Android 使用 SDL3 初始化窗口系统，保留上下文原有的私有启动顺序。
/// @warning 启动低频路径：加载设备 Vulkan loader，不能在帧循环重复调用。
void VKContext::initGLFW()
{
    // 只初始化视频引用，不能用全局 SDL_Quit 关闭仍由音频引擎持有的子系统。
    // loader 选择交给 SDL，避免误载桌面 Vulkan loader 或固定厂商驱动路径。
    if ( !SDL_InitSubSystem(SDL_INIT_VIDEO) ||
         !SDL_Vulkan_LoadLibrary(nullptr) ) {
        failInitialization("Failed to initialize SDL3 Android Vulkan: " +
                           std::string(SDL_GetError()));
        return;
    }
    // 诊断仍使用原上下文的收集流程，手机初始化错误可以从同一日志入口定位。
    // 窗口创建在视频子系统成功后由既有 NativeWindow 生命周期完成。
    addStartupDiagnostic("Window backend: SDL3 Android");
    collectVulkanLoaderDiagnostics();
}
/// @brief 从 SDL 查询真实 Android WSI 扩展，不借用宿主的 X11 或 Wayland 列表。
void VKContext::registerGLFWExtensions()
{
    // SDL 返回扩展名称的借用数组，不能释放或缓存到视频子系统关闭之后。
    // 使用设备实际 WSI 扩展，不按编译宿主推断 surface 平台。
    std::uint32_t      count      = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    // 缺少扩展是初始化失败，不能带着空列表继续创建 Vulkan instance。
    // 错误交由现有失败状态收敛，保持构造阶段无异常退出契约。
    if ( !extensions ) {
        failInitialization("Failed to query SDL3 Vulkan extensions.");
        return;
    }
    // 名称存储由 SDL 维护，直到 instance 创建完成前不得关闭视频子系统。
    for ( std::uint32_t i = 0; i < count; ++i )
        m_vkExtensions.push_back(extensions[i]);
}
/// @brief Vulkan 子资源均已释放后结束 SDL 视频子系统，音频子系统独立管理。
// 释放的是本上下文初始化的视频引用，SDL 音频实例由引擎独立释放。
// Vulkan instance 和 Surface 必须先于这一步销毁，避免回调访问失效窗口。
void VKContext::releaseGLFW()
{
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
}  // namespace MMM::Graphic
