#include "game/GameLoop.h"
#include "canvas/TestCanvas.h"
#include "game/GlobDefs.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/CLayoutTestUI.h"
#include "ui/imgui/ImguiTestWindowUI.h"
#include "ui/imgui/MainDockSpaceUI.h"

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
    m_uiManager.registerView("MainDockSpaceUI",
                             std::make_unique<Graphic::UI::MainDockSpaceUI>());
    m_uiManager.registerView("TestCanvas",
                             std::make_unique<Canvas::TestCanvas>(200, 200));
    m_uiManager.registerView(
        "ImguiTestWindowUI",
        std::make_unique<Graphic::UI::ImguiTestWindowUI>());
    m_uiManager.registerView("CLayoutTestUI",
                             std::make_unique<Graphic::UI::CLayoutTestUI>());
}

GameLoop::~GameLoop() {}

/**
 * @brief 启动游戏循环
 *
 * 初始化窗口、图形上下文，并进入主消息/渲染循环。
 * 该函数会阻塞直到窗口关闭。
 *
 * @param window_title 窗口标题
 * @return int 退出代码 (0 表示正常退出)
 */
int GameLoop::start(std::string_view window_title)
{
    // 初始化窗口
    Graphic::NativeWindow window(1280, 720, window_title.data());
    // VKContext 表面资源后续初始化
    if ( g_vkContext ) {
        auto& context = g_vkContext->get();
        int   fbWidth, fbHeight;
        window.getFramebufferSize(fbWidth, fbHeight);
        context.initVKWindowRess(window.getWindowHandle(), fbWidth, fbHeight);
        context.setVSync(false);
        // 进入主循环
        while ( !window.shouldClose() ) {
            // 3.1 让操作系统处理窗口事件 (缩放、关闭、鼠标按键等)
            window.pollEvents();
            // 3.2 执行渲染
            context.getRenderer().render(
                window, std::vector<Graphic::UI::UIManager*>{ &m_uiManager });
        }
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
