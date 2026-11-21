#include "main/StartupWindow.h"

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>

namespace MMM::Main
{
namespace
{

/// @brief 启动页基准宽度，单位为逻辑像素。
constexpr int STARTUP_WINDOW_WIDTH = 780;
/// @brief 启动页、进度面板和展开日志共用的基准高度。
constexpr int STARTUP_WINDOW_HEIGHT = 480;

/// @brief 换算用户设置的界面倍率，平台 DPI 由原生窗口另行应用。
/// @param value 未应用界面倍率的逻辑尺寸。
/// @return 可传给原生窗口的正尺寸。
/// @note 仅启动时读取一次窗口设置，渲染期间的资源区使用同一个工作区。
/// 初始尺寸以逻辑像素提供，不能预乘 framebuffer 缩放。
int scaledStartupWindowDimension(int value)
{
    float multiplier =
        Config::AppConfig::instance().getEditorSettings().uiScaleMultiplier;
    // 无效配置回退基准尺寸，不能把 NaN 和零传给窗口系统。
    if ( !std::isfinite(multiplier) || multiplier <= 0.0f ) multiplier = 1.0f;
    return std::max(
        static_cast<int>(std::lround(static_cast<float>(value) * multiplier)),
        1);
}

}  // namespace

/// @brief 使用 Startup 窗口模式和 Bootstrap 资源模式初始化完整预览。
/// @details 启动窗口不依赖正式编辑器的皮肤和持久化窗口布局。
/// 初始化失败时保留已经创建的对象，析构统一按依赖顺序释放。
/// @warning 启动低频路径：可能枚举驱动、创建交换链和上传字体。
StartupWindow::StartupWindow(const char* title)
{
    // 保留驱动诊断，平台或 Vulkan 实例初始化失败时不创建窗口。
    if ( !m_context.getInitializationError().empty() ) {
        XERROR("Failed to initialize startup graphic context: {}",
               m_context.getInitializationError());
        return;
    }

    // 窗口自身不消费正式编辑器窗口命令，可在应用资源尚未准备好时打开。
    // 可选值只在实例有效后构造，失败分支不会创建空的备用窗口。
    m_window.emplace(scaledStartupWindowDimension(STARTUP_WINDOW_WIDTH),
                     scaledStartupWindowDimension(STARTUP_WINDOW_HEIGHT),
                     title,
                     Graphic::NativeWindowMode::Startup);
    // 平台窗口是挑选呈现设备及队列的前置条件，空句柄不能用于交换链初始化。
    if ( !m_window->getWindowHandle() ) {
        XERROR("Failed to create startup resource window");
        return;
    }

    // 交换链使用物理像素尺寸；Bootstrap 只加载界面和系统字体所需资源。
    int width  = 0;
    int height = 0;
    // 查询结果是这一个窗口的实际像素，不能复用其它窗口的 DPI。
    m_window->getFramebufferSize(width, height);
    // 只有窗口上下文完整建立，调用方才能拿到可驱动帧循环的渲染器。
    m_ready = m_context.initVKWindowRess(
        &*m_window, width, height, Graphic::VKWindowResourceMode::Bootstrap);
    if ( !m_ready )
        XERROR("Failed to initialize startup Vulkan window resources");
}

/// @brief 在原生窗口析构之前完成上下文释放。
/// @warning 退出低频路径：release 可以等待队列空闲，不允许由绘制回调调用。
StartupWindow::~StartupWindow()
{
    // 成员随后按逆序销毁，必须先在窗口有效时释放 surface 和设备。
    m_context.release();
}

}  // namespace MMM::Main
