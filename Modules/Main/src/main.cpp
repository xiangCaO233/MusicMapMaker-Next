#include "GameLoop.h"
#include "TestCanvas.h"
#include "colorful-log.h"

#include "SkinConfig.h"
#include <filesystem>

int main(int argc, char* argv[])
{
    using namespace MMM;
    namespace fs = std::filesystem;
    XLogger::init("MMM");

    // 获取当前工作目录路径
    // Windows 上通常是反斜杠，Linux 是正斜杠
    // 统一输出为 '/' (给 Lua 用)，可以使用 generic_string()
    std::string cwdStr = fs::current_path().generic_string();
    XINFO("Current CWD:{}", cwdStr);

    // 载入皮肤配置
    Config::SkinManager::instance().loadSkin(
        cwdStr + "/../../../assets/skins/mmm-nightly/skin.lua");
    // 启动循环
    int exitcode = GameLoop::instance().start();

    XLogger::shutdown();
    return exitcode;
}
