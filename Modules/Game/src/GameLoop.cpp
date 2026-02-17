#include "game/GameLoop.h"
#include "game/GlobDefs.h"
#include "canvas/TestCanvas.h"
#include "log/colorful-log.h"
#include "graphic/vk/VKContext.h"

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

GameLoop::GameLoop() : vkContext(Graphic::VKContext::get())
{
    XINFO("GameLoop created");
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
int GameLoop::start(std::string_view window_title) const {
    // 初始化窗口
    Canvas::TestCanvas canvas(800, 600, window_title);

    // VKContext 表面资源后续初始化
    if ( vkContext ) {
        auto& context = vkContext->get();
        context.initVKWindowRess(canvas.getWindowHandle(), 800, 600);
        // 进入主循环
        while ( !canvas.shouldClose() ) {
            canvas.update(context);
        }
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
