#include "graphic/sdl/AndroidNativeWindow.h"
#include "config/AppConfig.h"
#include "event/ui/GLFWNativeEvent.h"
#include "log/colorful-log.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

// Android 显示模式可能在系统授权页返回后变化，刷新率不能永久沿用启动值。
// 逻辑更新频率和渲染上限继续由公共配置计算，此后端只提供当前显示能力。
// 刷新率更新仅由 SDL 低频窗口事件触发，禁止逐帧查询系统显示模式。
// 系统实际模式可能低于请求上限，不能用 Surface 偏好伪造显示测量值。
namespace MMM::Graphic
{
namespace
{
/// @brief Android 只有一个 Activity 窗口，此观察指针仅在 SDL 主线程使用。
NativeWindow* activeWindow = nullptr;
}  // namespace

/// @brief 使用同一 SDL3 实例创建 Vulkan 窗口，正式 UI 与启动流程共用该边界。
NativeWindow::NativeWindow(int width, int height, const char* title,
                           NativeWindowMode)
{
    // 触屏仅采用 SDL 自带的主触点鼠标合成，不增加手势消抖或编辑命令旁路。
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    m_window = SDL_CreateWindow(title,
                                width,
                                height,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN |
                                    SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if ( !m_window ) {
        XERROR("Failed to create Android window: {}", SDL_GetError());
        return;
    }
    // 只有创建成功的窗口能成为活动观察对象，失败对象不能接收 UI 关闭请求。
    // SDLActivity 当前只承载一个原生窗口，桌面多窗口功能仍由 GLFW 实现。
    activeWindow = this;
    // 帧率倍数基于当前显示模式，不能保留 AppConfig 的桌面默认 60Hz。
    // 当前速率可能随后因 Activity 的高刷请求变化，显示事件会再次同步。
    if ( const auto* mode =
             SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window)) ) {
        Config::AppConfig::instance().setDeviceRefreshRate(
            static_cast<int>(std::lround(mode->refresh_rate)));
    }
    // Android Surface 的实际尺寸来自系统，不恢复桌面窗口坐标。
    Config::AppConfig::instance().setWindowContentScale(
        SDL_GetWindowDisplayScale(m_window));
    // 保留业务已经使用的原生窗口命令类型，避免 Android 分支复制 UI 事件层。
    // 状态回传事件不会再次触发命令，防止关闭与最小化事件形成反馈环。
    m_subscription =
        Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
            [this](const Event::GLFWNativeEvent& event) {
                if ( event.hasStateChange ) return;
                if ( event.type == Event::NativeEventType::GLFW_CLOSE_WINDOW )
                    m_shouldClose = true;
                else if ( event.type ==
                          Event::NativeEventType::GLFW_ICONFY_WINDOW )
                    SDL_MinimizeWindow(m_window);
            });
}

/// @brief Vulkan 已释放后回收平台窗口，取消订阅以免回调访问析构对象。
NativeWindow::~NativeWindow()
{
    // 回调捕获本对象，必须先取消订阅，再释放平台资源。
    // 活动句柄只是非拥有观察指针，不能跨本对象生命周期缓存。
    if ( m_subscription )
        Event::EventBus::instance().unsubscribe<Event::GLFWNativeEvent>(
            m_subscription);
    // 初始化失败的对象未必注册为活动窗口，不能清掉其他仍有效的观察对象。
    // 对象退出后 UI 平台辅助函数安全返回未请求关闭的状态。
    if ( activeWindow == this ) activeWindow = nullptr;
    // VKContext 关闭视频子系统时可能已经销毁窗口，不能再次解引用旧指针。
    if ( SDL_WasInit(SDL_INIT_VIDEO) && m_window ) SDL_DestroyWindow(m_window);
}

/// @brief SDL 主线程只发布状态，不在事件回调中操作 Vulkan 资源。
/// @warning 每帧事件入口：不访问文件，不等待会话或音频线程持有的锁。
void NativeWindow::pollEvents() const
{
    // ImGui 上下文建立前的窗口事件仍须消费，但不能调用未初始化的后端。
    // 所有输入直接使用 SDL 后端的坐标与触点鼠标合成，不额外修改业务状态。
    SDL_Event event;
    while ( SDL_PollEvent(&event) ) {
        if ( ImGui::GetCurrentContext() ) ImGui_ImplSDL3_ProcessEvent(&event);
        // 关闭请求保留到既有未保存工程确认流程处理。
        if ( event.type == SDL_EVENT_QUIT ||
             event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED )
            m_shouldClose = true;
        // 像素尺寸脏位与 Surface 更换分开发布，普通调整尺寸无需替换 VkSurface。
        // 多个事件只覆盖最新状态，Vulkan 资源在下一安全帧边界统一更新。
        if ( event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED )
            m_resized = true;
        // 显示模式变化在 SDL 主线程同步，倍数限帧下一帧立即使用真实刷新率。
        if ( event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ||
             event.type == SDL_EVENT_DID_ENTER_FOREGROUND ) {
            if ( const auto* mode = SDL_GetCurrentDisplayMode(
                     SDL_GetDisplayForWindow(m_window)) ) {
                Config::AppConfig::instance().setDeviceRefreshRate(
                    static_cast<int>(std::lround(mode->refresh_rate)));
            }
        }
        if ( event.type == SDL_EVENT_WILL_ENTER_BACKGROUND )
            m_background = true;
        // 返回前台时系统可能已创建新的 ANativeWindow，即使尺寸相同也不能复用旧
        // Surface。 这里只置位，设备等待与资源销毁不在 SDL
        // 生命周期分发栈中执行。
        if ( event.type == SDL_EVENT_DID_ENTER_FOREGROUND ) {
            m_background     = false;
            m_surfaceChanged = true;
        }
    }
}

/// @brief 读取稳定的主线程关闭标志。
bool NativeWindow::shouldClose() const
{
    return m_shouldClose;
}
/// @brief 允许取消退出后继续使用同一 Activity。
void NativeWindow::setShouldClose(bool close)
{
    m_shouldClose = close;
}
/// @brief 返回当前 SDL 窗口观察句柄，不复制任何资源所有权。
PlatformWindowHandle* NativeWindow::getWindowHandle() const
{
    return m_window;
}
/// @brief 像素尺寸仅供交换链使用，平台 DPI 由 ImGui 后端单独处理。
void NativeWindow::getFramebufferSize(int& width, int& height) const
{
    // 交换链使用物理像素；UI 逻辑尺寸由 ImGui_ImplSDL3_NewFrame 更新。
    // 创建失败的窗口不会进入可渲染上下文，此处不伪造备用分辨率。
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
}
/// @brief APK 管理应用图标，不在启动或帧循环读取桌面图标文件。
void NativeWindow::reloadWindowIcon() {}
/// @brief Android 应用区域由系统管理，不把桌面窗口矩形应用到手机。
void NativeWindow::resizeAndCenter(int, int) {}
/// @brief 持久化接口报告系统当前分配的区域，不生成桌面屏幕坐标。
void NativeWindow::getWindowPlacement(int& x, int& y, int& width, int& height,
                                      bool& maximized) const
{
    x         = 0;
    y         = 0;
    maximized = true;
    // 配置继续记录逻辑窗口大小；Android 不使用其中的桌面位置恢复窗口。
    // 不把物理分辨率写入用户缩放配置，避免恢复后产生重复 DPI 缩放。
    SDL_GetWindowSize(m_window, &width, &height);
}
/// @brief 读取桌面配置不会改变 Android 原生窗口几何。
void NativeWindow::applyWindowPlacement(int, int, int, int, bool) {}
/// @brief Android 不提供桌面的最大化状态转换。
void NativeWindow::toggleMaximized() {}
/// @brief Activity 窗口始终由系统填充其应用区域。
bool NativeWindow::isWindowMaximized() const
{
    return true;
}
/// @brief 不向 Android 提供桌面边框适配器。
IWindowFrameAdapter* NativeWindow::getWindowFrameAdapter() const
{
    return nullptr;
}
/// @brief Android 的系统栏与安全区由平台管理，不切换桌面窗口模式。
void NativeWindow::ToggleFullscreen() {}
/// @brief 消费尺寸脏位，连续尺寸事件立即更新，不引入固定时间等待。
/// @warning 每帧只读写本线程布尔值，不分配、不加锁。
bool NativeWindow::shouldRecreate() const
{
    // 消费后清空旧状态，后续相同尺寸不会因历史事件永久触发重建。
    // 新尺寸事件仍可在下一次 pollEvents 中立即重新置位。
    bool dirty = m_resized;
    m_resized  = false;
    return dirty;
}
/// @brief 后台、隐藏或最小化窗口不提交 GPU 命令。
bool NativeWindow::isMinimized() const
{
    // 后台标志优先于窗口 flag，避免 Surface
    // 已不可用但隐藏状态尚未同步时取得图像。 查询只发生在 SDL
    // 主线程，不需要原子值或跨线程互斥锁。
    return m_background || !m_window ||
           (SDL_GetWindowFlags(m_window) &
            (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}
/// @brief Surface 失效由上下文在安全边界处理，事件层只发布通知。
bool NativeWindow::consumeSurfaceChange() const
{
    // 多个恢复事件只需一次重建，失败重试由调用者显式置回脏位。
    // 交换链对象不保存 SDL 生命周期，因此标记保留在窗口边界。
    bool changed     = m_surfaceChanged;
    m_surfaceChanged = false;
    return changed;
}
/// @brief acquire 或 present 遇到 Surface 丢失时不立即退出应用。
void NativeWindow::markSurfaceChanged() const
{
    m_surfaceChanged = true;
}

/// @brief SDL3 的 ImGui PlatformHandle 保存 SDL_WindowID，而不是 SDL_Window
/// 指针。
/// @warning UI 热路径只比较主窗口 ID，不访问文件或业务状态。
bool platformWindowShouldClose(void* handle)
{
    // ImGui 的 SDL3 PlatformHandle 是整数 ID，强制转换成 SDL_Window*
    // 会解引用无效地址。 只接受当前主窗口的 ID；其他句柄不会参与 Android
    // 的退出状态。
    return activeWindow &&
           reinterpret_cast<std::uintptr_t>(handle) ==
               SDL_GetWindowID(activeWindow->getWindowHandle()) &&
           activeWindow->shouldClose();
}
/// @brief 既有项目关闭确认流程通过正确的平台句柄更新同一窗口的退出标志。
void setPlatformWindowShouldClose(void* handle, bool requested)
{
    // 未保存确认窗口可撤销退出，不能将一次 SDL 关闭事件直接变成进程终止。
    // 空句柄或其他窗口 ID 都不改变当前主窗口状态。
    if ( activeWindow && reinterpret_cast<std::uintptr_t>(handle) ==
                             SDL_GetWindowID(activeWindow->getWindowHandle()) )
        activeWindow->setShouldClose(requested);
}
}  // namespace MMM::Graphic
