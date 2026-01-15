#include "GameLoop.h"
#include "GlobDefs.h"
#include "TestCanvas.h"
#include "colorful-log.h"
#include "vk/VKContext.h"

namespace MMM
{
/*
 * vk上下文引用
 * */
std::expected<std::reference_wrapper<Graphic::VKContext>, std::string>
    GameLoop::vkContext = Graphic::VKContext::get();

GameLoop& GameLoop::instance()
{
    static GameLoop loopInstance;
    return loopInstance;
}

GameLoop::GameLoop()
{
    XINFO("GameLoop created");
}
GameLoop::~GameLoop() {}

int GameLoop::start(std::string_view window_title)
{
    // 初始化窗口
    Canvas::TestCanvas canvas(800, 600, window_title);

    // VKContext 表面资源后续初始化
    if ( vkContext ) {
        vkContext->get().initVKWindowRess(canvas.getWindowHandle());
    }

    while ( !canvas.shouldClose() ) {
        canvas.update();
    }

    return EXIT_NORMAL;
}
}  // namespace MMM
