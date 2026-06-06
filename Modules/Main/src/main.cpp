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
#include "main/StartupProgressDialog.h"
#include "network/AssetSyncService.h"
#include <filesystem>
#include <optional>

int main(int argc, char* argv[])
{
    using namespace MMM;

    /// @brief 启动期资源下载进度弹窗，仅在实际下载或解压时创建。
    std::optional<Main::StartupProgressDialog> startupProgressDialog;

    /// @brief 启动时同步用户 .config/mmm 下的资源包。
    auto assetSyncOptions = Network::AssetSyncService::defaultOptions();
    std::error_code assetsExistsError;
    const bool      assetsMissingBeforeSync = !std::filesystem::exists(
        assetSyncOptions.assetsRootPath, assetsExistsError);
    assetSyncOptions.progressCallback =
        [&startupProgressDialog,
         assetsMissingBeforeSync](const Network::AssetSyncProgress& progress) {
            const bool shouldOpenImmediately =
                assetsMissingBeforeSync &&
                progress.stage ==
                    Network::AssetSyncProgressStage::kCheckingManifest;
            if ( !startupProgressDialog &&
                 (shouldOpenImmediately ||
                  Main::StartupProgressDialog::shouldOpenFor(progress)) ) {
                startupProgressDialog.emplace();
            }
            if ( startupProgressDialog ) {
                startupProgressDialog->update(progress);
            }
        };
    const auto assetSyncResult =
        Network::AssetSyncService::sync(assetSyncOptions);
    if ( startupProgressDialog ) startupProgressDialog->close();
    if ( assetSyncResult.status == Network::AssetSyncStatus::kError ) {
        const auto  assetPath = Config::AppPaths::assetsRootPath();
        std::string msg =
            "Could not download or verify assets automatically.\n"
            "Please check your network connection, or download assets.zip from "
            "the website and extract it to:\n" +
            Config::pathToUtf8(assetPath) +
            "\n\nError: " + assetSyncResult.errorMessage;
        XERROR("Fatal: {}", msg);
        UI::showFatalError("MusicMapMaker - Assets Sync Failed", msg);
        return -1;
    }

    /// @brief 检查默认皮肤入口脚本是否已由同步流程准备完成。
    const auto      defaultSkinPath = Config::AppPaths::defaultSkinFilePath();
    std::error_code defaultSkinExistsError;
    if ( !std::filesystem::exists(defaultSkinPath, defaultSkinExistsError) ) {
        const auto  assetPath = Config::AppPaths::assetsRootPath();
        std::string msg =
            "Could not find default skin after assets sync.\n"
            "Please download assets.zip from the website and extract it to:\n" +
            Config::pathToUtf8(assetPath);
        XERROR("Fatal: {}", msg);
        UI::showFatalError("MusicMapMaker - Assets Missing", msg);
        return -1;
    }

    using namespace Config;
    // 载入应用全局配置 (序列化/反序列化测试)
    AppConfig::instance().load();

    // 载入皮肤配置
    SkinManager::instance().loadSkin(Config::pathToUtf8(defaultSkinPath));
    auto [r, g, b, a] = SkinManager::instance().getColor("background");

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

    Graphic::NativeWindow nativeWindow(1400, 900, "MusicMapMaker(Gamma)");

    const auto ret = gameLoop.start(nativeWindow, argc, argv);

    // PGO — 强制写出 profile 并按运行时长阈值上传
    Main::shutdownPGOProfiler();

    return ret;
}
