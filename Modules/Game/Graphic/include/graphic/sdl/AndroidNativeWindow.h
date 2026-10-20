#pragma once
#include "event/core/EventBus.h"
#include "graphic/window/WindowPlatform.h"
#include <cstdint>

struct GLFWwindow;
namespace MMM::Graphic
{
class IWindowFrameAdapter;
/// @brief 保持启动窗口与正式窗口角色接口，Android 均使用单个 Activity 窗口。
enum class NativeWindowMode : std::uint8_t { Application, Startup };
/// @brief Android 原生窗口平台层，只在 SDL 原生主线程访问窗口和事件。
/// @details 不承担双指手势或编辑命令转换，输入统一交给 ImGui SDL3 后端。
class NativeWindow final
{
public:
    /// @brief 绑定 SDLActivity 当前原生窗口，尺寸由 Android 最终决定。
    NativeWindow(int width, int height, const char* title,
                 NativeWindowMode mode = NativeWindowMode::Application);
    /// @brief 取消事件订阅并释放窗口；Vulkan Surface 必须已由上下文回收。
    ~NativeWindow();
    NativeWindow(const NativeWindow&)            = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;
    /// @brief 读取 UI 主线程维护的关闭请求。
    bool shouldClose() const;
    /// @brief 更新关闭状态，允许未保存工程对话框暂时撤销退出。
    void setShouldClose(bool close);
    /// @brief 轮询输入与生命周期，保持平台事件只进入 ImGui 一次。
    /// @warning 每帧主线程调用，不访问文件，不等待业务线程持锁区。
    void pollEvents() const;
    /// @brief 获取借用的 SDL 窗口句柄，不能将其转换成 GLFWwindow。
    PlatformWindowHandle* getWindowHandle() const;
    /// @brief 查询 framebuffer 像素尺寸，不能与 ImGui 的逻辑尺寸混用。
    void getFramebufferSize(int& width, int& height) const;
    /// @brief Android 没有桌面窗口图标，图标由 APK 资源声明。
    void reloadWindowIcon();
    /// @brief Android 不恢复桌面窗口矩形，继续使用系统分配的全屏区域。
    void resizeAndCenter(int width, int height);
    /// @brief 返回当前 Activity 窗口几何，保持桌面配置序列化接口兼容。
    void getWindowPlacement(int& x, int& y, int& width, int& height,
                            bool& maximized) const;
    /// @brief 忽略桌面保存的位置与尺寸，避免把设备窗口缩为桌面逻辑尺寸。
    void applyWindowPlacement(int x, int y, int width, int height,
                              bool maximized);
    /// @brief Android 窗口没有桌面最大化切换，保留命令入口。
    void toggleMaximized();
    /// @brief Android Activity 占据系统分配的应用区域。
    bool isWindowMaximized() const;
    /// @brief Android 不创建桌面窗口边框适配器。
    IWindowFrameAdapter* getWindowFrameAdapter() const;
    /// @brief Activity 窗口没有桌面全屏与窗口模式切换。
    void ToggleFullscreen();
    /// @brief 消费本线程发布的 framebuffer 脏标记，不使用固定时长消抖。
    /// @warning 每帧执行，只交换一个普通布尔值。
    bool shouldRecreate() const;
    /// @brief 后台或最小化窗口不可提交 GPU 绘制。
    bool isMinimized() const;
    /// @brief 消费 Android 原生 Surface 已更换的通知。
    bool consumeSurfaceChange() const;
    /// @brief Vulkan 检测到 Surface 丢失时推迟至下一轮生命周期处理后重建。
    void markSurfaceChanged() const;

private:
    /// @brief SDL 窗口资源由本对象持有；创建与销毁都在 UI 主线程。
    PlatformWindowHandle* m_window = nullptr;
    /// @brief 关闭请求只经平台主线程提交与撤销。
    mutable bool m_shouldClose = false;
    /// @brief 后台状态由 SDL 生命周期事件更新，不读取可变会话状态。
    mutable bool m_background = false;
    /// @brief 尺寸变化只标记重建，实际 Vulkan 操作由渲染安全边界执行。
    mutable bool m_resized = false;
    /// @brief Surface 更换不同于尺寸变化，旧 VkSurfaceKHR 不能复用。
    mutable bool m_surfaceChanged = false;
    /// @brief 与窗口寿命相同的既有原生窗口命令订阅。
    Event::SubscriptionID m_subscription = 0;
};
}  // namespace MMM::Graphic
