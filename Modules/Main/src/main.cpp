#include <GameLoop.h>
#include <TestCanvas.h>
#include <WindowContext.hpp>
#include <colorful-log.h>

int main(int argc, char* argv[])
{
    XLogger::init("MMM");
    XINFO("test main nmsl");

    MMM::WindowContext context;
    auto               exitcode = MMM::GameLoop::instance().start();

    XLogger::shutdown();
    return exitcode;
}
