#include "game/GameLoop.h"
#include "canvas/Basic2DCanvas.h"
#include "config/skin/translation/Translation.h"
#include "game/GlobDefs.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/AudioManagerView.h"
#include "ui/imgui/manager/FileManagerView.h"

namespace MMM
{

/**
 * @brief 获取 GameLoop 单例实例
 * @return GameLoop& 唯一实例引用
 */
GameLoop& GameLoop::instance()
{
    static GameLoop loopInstance;
    return loopInstance;
}

GameLoop::GameLoop() : g_vkContext(Graphic::VKContext::get())
{
    XINFO("GameLoop created");

    // 注册ui视图
    m_uiManager.registerView(
        "MainDockSpaceUI",
        std::make_unique<Graphic::UI::MainDockSpaceUI>("MainDockSpaceUI"));
    m_uiManager.registerView(
        "SideBarUI", std::make_unique<Graphic::UI::SideBarUI>("SideBarUI"));
    m_uiManager.registerView(
        "SideBarManager",
        std::make_unique<Graphic::UI::FloatingManagerUI>("SideBarManager"));
    auto sidebar_manager =
        m_uiManager.getView<Graphic::UI::FloatingManagerUI>("SideBarManager");
    sidebar_manager->registerSubView(
        TR("title.file_manager"),
        std::make_unique<Graphic::UI::FileManagerView>(
            TR("title.file_manager")));
    sidebar_manager->registerSubView(
        TR("title.audio_manager"),
        std::make_unique<Graphic::UI::AudioManagerView>(
            TR("title.audio_manager")));

    // 初始化时默认激活第一个 Tab（文件管理器）
    sidebar_manager->toggleSubView(TR("title.file_manager"));

    m_uiManager.registerView(
        "Basic2DCanvas",
        std::make_unique<Canvas::Basic2DCanvas>("Basic2DCanvas", 200, 200));
    // m_uiManager.registerView(
    //     "ImguiTestWindowUI",
    //     std::make_unique<Graphic::UI::ImguiTestWindowUI>("ImguiTestWindowUI"));
    // m_uiManager.registerView(
    //     "CLayoutTestUI",
    //     std::make_unique<Graphic::UI::CLayoutTestUI>("CLayoutTestUI"));
    // m_uiManager.registerView(
    //     "DebugWindowUI",
    //     std::make_unique<Graphic::UI::DebugWindowUI>("DebugWindowUI"));
}

GameLoop::~GameLoop() {}

/**
 * @brief 启动游戏循环
 *
 * 初始化窗口、图形上下文，并进入主消息/渲染循环。
 * 该函数会阻塞直到窗口关闭。
 *
 * @param window 窗口上下文
 * @return int 退出代码 (0 表示正常退出)
 */
int GameLoop::start(Graphic::NativeWindow& window)
{
    // 初始化窗口
    // VKContext 表面资源后续初始化
    if ( g_vkContext ) {
        auto& context = g_vkContext->get();
        int   fbWidth, fbHeight;
        window.getFramebufferSize(fbWidth, fbHeight);
        context.initVKWindowRess(&window, fbWidth, fbHeight);
        context.setVSync(false);
        // context.ToggleFullscreen();
        // 进入主循环
        while ( !window.shouldClose() ) {
            // 3.1 让操作系统处理窗口事件 (缩放、关闭、鼠标按键等)
            window.pollEvents();
            // 3.2 执行渲染
            context.getRenderer().render(
                window,
                std::vector<Graphic::IGraphicUserHook*>{ &m_uiManager });
        }
        // 2. 主动清理 UI 管理器里存的所有视图
        // 这样 VKOffScreenRenderer 的析构就会在这里发生，
        // 此时 VKContext 还健在，m_device 也是有效的！
        m_uiManager.clearAllViews();
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
