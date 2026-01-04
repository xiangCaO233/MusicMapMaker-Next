#include "GameLoop.h"
#include "GLFW/glfw3.h"
#include "GlobDefs.h"
#include "TestCanvas.h"
#include "colorful-log.h"

namespace MMM
{
GameLoop& GameLoop::instance()
{
    static GameLoop loopInstance;
    return loopInstance;
}

GameLoop::GameLoop()
{
    XINFO("GameLoop created");
}
GameLoop::~GameLoop()
{
    glfwTerminate();
}

int GameLoop::start()
{
    Canvas::TestCanvas canvas(800, 600);
    while ( !canvas.shouldClose() )
    {
        canvas.update();
    }
    return EXIT_NORMAL;
}
}  // namespace MMM
