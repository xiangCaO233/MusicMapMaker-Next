#include "game/GameLoop.h"
#include "log/colorful-log.h"
#include "config/skin/SkinConfig.h"
#include "config/translation/Translation.h"
#include <filesystem>

int main(int argc, char* argv[])
{
    using namespace MMM;

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

    // 跨平台（自动处理 / 或 \）
    const auto assetPath = rootDir / "assets";

    using namespace Translation;
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "en_us.lua").generic_string());
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "zh_cn.lua").generic_string());
    Translator::instance().switchLang("zh_cn");
    XINFO(TR("tips.welcome"));

    using namespace Config;
    // 载入皮肤配置
    SkinManager::instance().loadSkin(
        (assetPath / "skins" / "mmm-nightly" / "skin.lua").generic_string());
    auto [r, g, b, a] = SkinManager::instance().getColor("background");
    XINFO("background color:[{},{},{},{}]",
          r,
          g,
          b,
          a);

    // 测试vulkan
    auto& gameLoop = GameLoop::instance();

    // 检查 Vulkan 环境
    if ( !gameLoop.vkContext ) {
        // 这里会打印 VKContext::get() 的 catch 块里填入的 e.what()
        XERROR("Start Failed, graphic enc initialize failed with:\n {}",
               gameLoop.vkContext.error());
        return 1;
    }

    // 正常运行
    XINFO("entering gameloop...");

    const auto ret = gameLoop.start("Vulkan Test");

    return ret;
}
