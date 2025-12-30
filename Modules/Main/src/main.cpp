#include <TestCanvas.h>
#include <colorful-log.h>

int main(int argc, char* argv[])
{
    XLogger::init("MMM");
    XINFO("test main nmsl");
    Canvas::TestCanvas canvas(100, 100);
    XLogger::shutdown();
    return 0;
}
