#pragma once

#include "graphic/imguivk/VKQueueFamilyDef.h"
#include "graphic/window/WindowPlatform.h"
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef VULKAN_HPP_NO_EXCEPTIONS
#    define VULKAN_HPP_NO_EXCEPTIONS
#endif
#include <vulkan/vulkan.hpp>

struct GLFWwindow;

namespace MMM::Config
{
enum class FrameLimitPreference;
}

namespace MMM::Graphic
{
class ImGuiThemeRegistry;
class NativeWindow;
class VKRenderer;
class VKRenderPass;
class VKSwapchain;
struct ThemePluginReloadResult;

/// @brief 窗口图形资源的初始化模式。
enum class VKWindowResourceMode : std::uint8_t {
    Bootstrap,   ///< 资源同步前仅加载系统字体和最小 ImGui 样式。
    Application  ///< 资源就绪后加载完整皮肤字体、主题和软件光标。
};

class VKContext final
{
public:
    /**
     * @brief 获取 VKContext 单例实例
     *
     * 如果实例尚未创建，将尝试进行初始化。
     *
     * @return 成功时返回上下文引用，失败时返回错误信息字符串
     */
    static std::expected<std::reference_wrapper<VKContext>, std::string> get();

    VKContext();
    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;
    ~VKContext();

    /// @brief 获取基础图形上下文初始化失败原因。
    /// @return 初始化成功时为空字符串，否则返回可记录的失败原因。
    [[nodiscard]] const std::string& getInitializationError() const
    {
        return m_initializationError;
    }

    /**
     * @brief 初始化 Vulkan 窗体相关资源
     *
     * 包括 Surface, Swapchain, RenderPass, Pipeline 等依赖窗口尺寸的资源。
     *
     * @param native_window_ptr 原生窗口封装。
     * @param w 窗口宽度
     * @param h 窗口高度
     * @param mode 窗口图形资源初始化模式。
     * @return 初始化成功或同一窗口已经初始化时返回 true。
     * @warning 启动低频路径：会创建 Vulkan surface、device、swapchain 和
     * ImGui 后端，禁止放入渲染循环。
     */
    bool initVKWindowRess(
        NativeWindow* native_window_ptr, int width, int height,
        VKWindowResourceMode mode = VKWindowResourceMode::Application);

    /**
     * @brief 获取渲染器实例
     * @return VKRenderer& 渲染器引用
     */
    inline VKRenderer& getRenderer() { return *m_vkRenderer; }

    /**
     * @brief 获取逻辑设备句柄
     * @return vk::Device& 逻辑设备引用
     */
    inline vk::Device& getLogicalDevice() { return m_vkLogicalDevice; }

    /// @brief 根据帧率限制策略切换交换链呈现模式。
    /// @param frameLimit 帧率限制策略。
    /// @warning 低频同步点：呈现模式实际变化时会 waitIdle 并标记交换链重建，
    /// 只能由设置变更路径调用。
    void setFrameLimitPresentMode(Config::FrameLimitPreference frameLimit);

    /**
     * @brief 全屏
     */
    void ToggleFullscreen();

    /**
     * @brief 显示在窗口正中央的临时通知/Tooltip
     * @param message 提示消息文本
     * @param durationSeconds 持续时间 (秒)
     */
    void showCenterNotification(const std::string& message,
                                float              durationSeconds = 1.5f);

    /**
     * @brief 绘制临时中央通知 (每帧渲染)
     * @warning
     * 热路径：渲染循环每帧执行；禁止文件系统访问、阻塞等待和异常处理。

     */
    void drawCenterNotification();

    /**
     * @brief 设置 ImGui 字体 (供初始化和重载使用)
     */
    void setupFonts();

    /**
     * @brief 热重载 ImGui 字体并重建纹理
     */
    void rebuildFonts();

    /**
     * @brief 恢复当前字体 atlas 生命周期内固定的字体缩放。
     *
     * @warning 低频设置路径：禁止在这里读取实时字体倍率并触发动态
     * atlas 重烘焙；字体尺寸变更只能在下一次字体 atlas 重建后生效。
     */
    void updateFontScales();

    /**
     * @brief 请求在下一帧开始前进行字体重建 (线程安全)
     * @warning
     * 原子：设置跨
     * UI/渲染边界的字体重建请求；只发布布尔脏位，不同步字体资源本身。

     */
    void requestFontRebuild();

    /**
     * @brief 检查并执行字体重建 (由主循环在 NewFrame 前调用)
     *
     * @warning 热路径/原子：渲染循环每帧调用一次；原子 exchange
     * 不可避免，用于低频字体重建请求消耗。
     */
    void checkAndRebuildFonts();

    /// @brief 低频检查系统亮暗外观并在自动模式下更新皮肤主题。
    /// @warning 渲染热路径每帧调用，但每帧只进行枚举和时间点比较；平台状态
    /// 刷新被限制为每秒一次，禁止在未节流分支加入阻塞操作。
    void checkAndApplySystemTheme();

    /**
     * @brief 应用当前主题配置
     */
    void applyTheme();

    /// @brief 删除所有自定义插件实例并重新载入插件目录。
    /// @return 主题插件扫描与加载结果。
    /// @warning 低频插件重载路径：会访问文件系统并执行
    /// Lua，只能从启动或菜单动作调用。
    ThemePluginReloadResult reloadPlugins();

    /// @brief 热切换单个已扫描插件的启用状态并立即重载插件。
    /// @param pluginId 配置根目录相对插件 ID。
    /// @param enabled 切换后的启用状态。
    /// @return 找到插件且配置成功保存时返回 true；运行时切换不因保存失败回滚。
    /// @warning 低频插件管理路径：会保存配置、访问文件系统并执行 Lua。
    bool setPluginEnabled(std::string_view pluginId, bool enabled);

    /// @brief 获取当前上下文持有的主题实例注册表。
    /// @return 主题注册表只读引用。
    [[nodiscard]] const ImGuiThemeRegistry& getThemeRegistry() const;

    /**
     * @brief 显式释放资源
     *
     * 在程序退出前，且窗口句柄仍然有效时调用，
     * 以确保 ImGui 和 Vulkan 资源的正确释放顺序。
     */
    void release();

private:
    /// @brief 持有字体 atlas 状态及主题实例，图形上下文初始化时创建一次。
    struct Appearance;
    /// @brief 渲染线程访问，其他线程只投递字体重建请求。
    std::unique_ptr<Appearance> m_appearance;

    /// @brief 持有启动诊断快照、验证层配置和与实例绑定的调试消息器。
    struct StartupDiagnostics;
    /// @brief 在 Vulkan 实例释放之前清理其调试消息器。
    std::unique_ptr<StartupDiagnostics> m_diagnostics;

    /// @brief 资源是否已释放
    bool m_isReleased{ false };

    /// @brief 窗口相关 Vulkan 与 ImGui 资源是否已经完成初始化。
    bool m_windowResourcesInitialized{ false };

    /// @brief 当前窗口图形资源所处的初始化模式。
    VKWindowResourceMode m_windowResourceMode{
        VKWindowResourceMode::Application
    };

    /// @brief GLFW 键盘事件订阅 ID，随上下文资源释放而取消订阅。
    std::uint64_t m_glfwKeySubscription{ 0 };

    /// @brief 逻辑配置命令订阅 ID，随上下文资源释放而取消订阅。
    std::uint64_t m_logicCommandSubscription{ 0 };

    /// @brief 初始化失败原因；为空表示基础上下文初始化成功。
    std::string m_initializationError;

    /// @brief 临时中央通知文本
    std::string m_notificationMessage;
    /// @brief 临时中央通知过期系统时间戳
    double m_notificationExpireTime{ 0.0 };

    /// @brief 记录初始化失败原因。
    /// @param message 失败原因。
    void failInitialization(std::string message);

    /// @brief 判断初始化是否已经失败。
    /// @return 已记录失败原因时返回 true。
    bool hasInitializationError() const
    {
        return !m_initializationError.empty();
    }

    /// @brief 根据帧率限制策略选择当前设备支持的呈现模式。
    /// @param frameLimit 帧率限制策略。
    /// @return 当前设备可用的 Vulkan 呈现模式。
    vk::PresentModeKHR selectPresentMode(
        Config::FrameLimitPreference frameLimit) const;

    /// @brief 仅更新全局呈现模式参数，不触发重建。
    /// @param frameLimit 帧率限制策略。
    void updateGlobalPresentMode(Config::FrameLimitPreference frameLimit);

    /**
     * @brief 初始化 GLFW 上下文
     */
    void initGLFW();

    /**
     * @brief 释放 GLFW 资源
     */
    static void releaseGLFW();

    /// @brief 原生窗口指针
    NativeWindow* m_nativeWindow_ptr{ nullptr };

    /**
     * @brief 注册 GLFW 所需的 Vulkan 扩展
     */
    void registerGLFWExtensions();

    /// @brief Vulkan 应用程序信息结构体
    vk::ApplicationInfo m_vkAppInfo{};

    /// @brief Vulkan 实例创建信息结构体
    vk::InstanceCreateInfo m_vkInstanceCreateInfo{};

    /// @brief Vulkan 动态函数加载器 (用于加载扩展函数)
    vk::detail::DispatchLoaderDynamic m_vkDldy{};

    /// @brief Vulkan 实例句柄
    vk::Instance m_vkInstance{};

    /**
     * @brief 初始化 Vulkan 应用程序信息
     */
    void initVkAppInfo();

    /**
     * @brief 初始化 Vulkan 实例创建信息
     */
    void initVkInstanceCreateInfo();

    /// @brief 启用的 Vulkan 扩展列表类型定义
    using VKExtensions = std::vector<const char*>;

    /// @brief 当前启用的 Vulkan 扩展列表
    VKExtensions m_vkExtensions{};

    // =========================================================================
    // 物理设备与逻辑设备相关
    // =========================================================================

    /// @brief 选定的 Vulkan 物理设备 (GPU)
    vk::PhysicalDevice m_vkPhysicalDevice{};

    /// @brief 队列族索引结构体
    QueueFamilyIndices m_queueFamilyIndices{};

    /// @brief 逻辑设备图形队列句柄
    vk::Queue m_LogicDeviceGraphicsQueue;

    /// @brief 逻辑设备呈现队列句柄
    vk::Queue m_LogicDevicePresentQueue;

    /**
     * @brief 使用imgui自动选择物理设备和队列族
     */
    void imguiAutoSelect();

    /// @brief Vulkan 逻辑设备句柄
    vk::Device m_vkLogicalDevice{};

    /**
     * @brief 初始化逻辑设备
     */
    void initLogicDevice();

    /// @brief Vulkan 表面句柄 (Window Surface)
    vk::SurfaceKHR m_vkSurface{};

    // =========================================================================
    // 渲染资源相关
    // =========================================================================

    /// @brief Vulkan 交换链封装对象
    std::unique_ptr<VKSwapchain> m_swapchain{ nullptr };

    /**
     * @brief 重建交换链
     */
    /// @return 只有完整呈现资源恢复后返回 true，Surface
    /// 失效时允许后续事件重试。
    bool recreateSwapchain(PlatformWindowHandle* window_context, int width,
                           int height);
#ifdef __ANDROID__
    /// @brief Android 恢复后更换原生 Surface，保留 device 和已有 UI/GPU 资源。
    /// @warning 仅 Surface 失效低频调用，会等待设备空闲并重建交换链。
    bool restoreAndroidSurface(NativeWindow& window, int width, int height);
#endif


    /// @brief Vulkan 渲染流程封装对象 (Render Pass)
    std::unique_ptr<VKRenderPass> m_vkRenderPass{ nullptr };

    // =========================================================================
    // 光标管理
    // =========================================================================

    /// @brief Vulkan 渲染器封装对象 (负责 Command Buffer 和 Draw Call)
    std::unique_ptr<VKRenderer> m_vkRenderer{ nullptr };

    // =========================================================================
    // imgui - 实现相关
    // =========================================================================

    /// @brief 初始化 ImGui GLFW/Vulkan 后端。
    /// @param windowHandle GLFW 窗口句柄。
    /// @param mode 字体、主题和功能开关的初始化模式。
    /// @warning 启动低频路径：创建 ImGui 上下文并初始化后端，禁止重复调用。
    void imguiVulkanInit(PlatformWindowHandle* windowHandle,
                         VKWindowResourceMode  mode);

    friend class VKRenderer;
};

}  // namespace MMM::Graphic
