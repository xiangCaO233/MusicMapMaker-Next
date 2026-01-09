#include "GameLoop.h"
#include "colorful-log.h"

#include "skin/SkinConfig.h"
#include "translation/Translation.h"
#include <filesystem>

int main(int argc, char* argv[])
{
    using namespace MMM;
    XLogger::init("MMM");

    // 假设 assets 肯定在运行目录上n级
    // 而 build 目录通常在 root/build/Modules/Main/ 下 (深度为 3 或 4)
    auto rootDir = std::filesystem::current_path();
    // 向上查找直到找到 assets 文件夹
    while ( !std::filesystem::exists(rootDir / "assets") &&
            rootDir.has_parent_path() ) {
        rootDir = rootDir.parent_path();
    }

    if ( !std::filesystem::exists(rootDir / "assets") ) {
        XERROR("Fatal: Could not find assets directory!");
        return -1;
    }

    // 现在的调用变得非常优雅，且跨平台（自动处理 / 或 \）
    auto assetPath = rootDir / "assets";

    using namespace Translation;
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "en_us.lua").generic_string());
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "zh_cn.lua").generic_string());
    Translator::instance().switchLang("zh_cn");
    XINFO(TR("tips.welcom"));

    using namespace Config;
    // 载入皮肤配置
    SkinManager::instance().loadSkin(
        (assetPath / "skins" / "mmm-nightly" / "skin.lua").generic_string());
    auto backgroundColor = SkinManager::instance().getColor("background");
    XINFO("background color:[{},{},{},{}]",
          backgroundColor.r,
          backgroundColor.g,
          backgroundColor.b,
          backgroundColor.a);
    // 启动循环
    int exitcode = GameLoop::instance().start();

    XLogger::shutdown();
    return exitcode;
}
