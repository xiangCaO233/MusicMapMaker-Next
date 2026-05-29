#include "common/MessageBox.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "game/GameLoop.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "log/colorful-log.h"
#include "main/PGOProfiler.h"
#include <filesystem>

int main(int argc, char* argv[])
{
    using namespace MMM;

    /// @brief 用户 .config/mmm 下的资源包根目录。
    const auto assetPath = Config::AppPaths::assetsRootPath();
    /// @brief 检查用户资源包目录是否存在时接收的文件系统错误。
    std::error_code assetExistsError;
    if ( !std::filesystem::exists(assetPath, assetExistsError) ) {
        std::string msg =
            "Could not find assets directory!\n"
            "Please download the resource package (assets.zip) from the "
            "website "
            "and extract it to:\n" +
            Config::pathToUtf8(assetPath);
        XERROR("Fatal: {}", msg);
        UI::showFatalError("MusicMapMaker - Assets Missing", msg);
        return -1;
    }

    using namespace Config;
    // 载入应用全局配置 (序列化/反序列化测试)
    AppConfig::instance().load();

    // 载入皮肤配置
    SkinManager::instance().loadSkin(
        Config::pathToUtf8(Config::AppPaths::defaultSkinFilePath()));
    auto [r, g, b, a] = SkinManager::instance().getColor("background");
    XINFO("background color:[{},{},{},{}]", r, g, b, a);

    XINFO(TR("tips.welcome"));

    // PGO instrumentation — 设置 profile 输出路径
    Main::initPGOProfiler();

    // 测试vulkan
    auto& gameLoop = GameLoop::instance();

    // 检查 Vulkan 环境
    if ( !gameLoop.g_vkContext ) {
        // 这里会打印 VKContext::get() 的 catch 块里填入的 e.what()
        XERROR("Start Failed, graphic enc initialize failed with:\n {}",
               gameLoop.g_vkContext.error());
        return 1;
    }

    // 正常运行
    XINFO("entering gameloop...");

    Graphic::NativeWindow nativeWindow(1280, 720, "MusicMapMaker(Gamma)");

    const auto ret = gameLoop.start(nativeWindow, argc, argv);

    // PGO — 强制写出 profile 并异步上传
    Main::shutdownPGOProfiler();

    return ret;
}
