#include "common/MessageBox.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "game/GameLoop.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include "main/PGOProfiler.h"
#include "main/StartupProgressDialog.h"
#include "network/AssetSyncService.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fmt/core.h>
#include <imgui.h>
#include <optional>
#include <string>
#include <thread>

namespace MMM::Main
{
namespace
{
/// @brief 默认皮肤目录名。
constexpr const char* kDefaultSkinDirectoryName = "mmm-default";

/// @brief 判断配置中的皮肤目录名是否只指向 skins 下一级目录。
/// @param directoryName 配置保存的皮肤目录名。
/// @return 目录名可用于拼接 skins 根目录时返回 true。
bool isValidSkinDirectoryName(const std::string& directoryName)
{
    if ( directoryName.empty() ) {
        return false;
    }

    const auto directoryPath = Config::utf8ToPath(directoryName);
    return !directoryPath.is_absolute() && !directoryPath.has_parent_path();
}

/// @brief 判断皮肤入口脚本是否存在。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 文件存在且是普通文件时返回 true。
bool skinLuaFileExists(const std::filesystem::path& skinLuaPath)
{
    std::error_code ec;
    return std::filesystem::exists(skinLuaPath, ec) &&
           std::filesystem::is_regular_file(skinLuaPath, ec);
}

/// @brief 根据编辑器配置解析启动时应加载的皮肤入口脚本。
/// @param defaultSkinPath 默认皮肤入口脚本路径。
/// @return 可加载的皮肤入口脚本路径，配置无效时返回默认皮肤。
std::filesystem::path resolveStartupSkinPath(
    const std::filesystem::path& defaultSkinPath)
{
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const auto  selectedDirectory = settings.selectedSkinDirectory.empty()
                                        ? std::string(kDefaultSkinDirectoryName)
                                        : settings.selectedSkinDirectory;

    if ( isValidSkinDirectoryName(selectedDirectory) ) {
        auto skinPath = Config::AppPaths::skinsRootPath();
        skinPath /= Config::utf8ToPath(selectedDirectory);
        skinPath /= "skin.lua";
        if ( skinLuaFileExists(skinPath) ) {
            return skinPath;
        }
        XWARN("Configured skin not found: {}", Config::pathToUtf8(skinPath));
    } else {
        XWARN("Configured skin directory is invalid: {}", selectedDirectory);
    }

    return defaultSkinPath;
}

/// @brief PGO 退出上传进度窗口的渲染钩子。
class PgoShutdownUploadProgressHook final : public Graphic::IGraphicUserHook
{
public:
    /// @brief 退出上传进度窗口不需要准备额外图形资源。
    /// @warning 退出低频渲染路径：关闭软件时短暂执行，不允许加入阻塞操作。
    void onPrepareResources(vk::PhysicalDevice&, vk::Device&,
                            Graphic::VKSwapchain&, vk::CommandPool&,
                            vk::Queue&) override
    {
    }

    /// @brief 渲染 PGO 上传进度模态窗口。
    /// @warning 退出低频渲染路径：关闭软件时短暂执行，只读取进度快照并绘制 UI。
    void onUpdateUI() override
    {
        const std::string popupId =
            std::string(TR("ui.pgo.upload.title").data()) +
            "###PgoShutdownUploadProgressModal";
        ImGui::OpenPopup(popupId.c_str());

        const float dpiScale =
            Config::AppConfig::instance().getWindowContentScale();
        UI::Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( !popupStyle.begin(
                 popupId.c_str(),
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove,
                 ImVec2(460.0f * dpiScale, 0.0f)) ) {
            return;
        }

        const auto progress = getPGOProfilerShutdownProgress();
        ImGui::TextWrapped("%s", TR("ui.pgo.upload.uploading").data());
        ImGui::Spacing();

        const float fraction = uploadFraction(progress);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));

        if ( progress.totalBytes > 0 ) {
            const double uploadedKiB =
                static_cast<double>(progress.uploadedBytes) / 1024.0;
            const double totalKiB =
                static_cast<double>(progress.totalBytes) / 1024.0;
            const auto bytesText =
                fmt::format(fmt::runtime(TR("ui.pgo.upload.bytes_fmt").data()),
                            uploadedKiB,
                            totalKiB);
            ImGui::TextWrapped("%s", bytesText.c_str());
        }

        const auto runtimeText =
            fmt::format(fmt::runtime(TR("ui.pgo.upload.runtime_fmt").data()),
                        progress.runtimeSeconds);
        ImGui::TextWrapped("%s", runtimeText.c_str());
        ImGui::EndPopup();
    }

    /// @brief 进度窗口不录制离屏命令。
    /// @warning 退出低频渲染路径：该钩子不访问离屏渲染资源。
    void onRecordOffscreen(vk::CommandBuffer&, uint32_t) override {}

    /// @brief 获取离屏任务数量。
    /// @return 始终为 0。
    /// @warning 退出低频渲染路径：仅返回稳定常量。
    uint32_t getOffscreenRecordTaskCount() const override { return 0; }

private:
    /// @brief 计算进度条比例；总字节未知时使用活动态动画。
    /// @param progress PGO 退出上传进度。
    /// @return 0 到 1 之间的进度比例。
    static float uploadFraction(const PGOProfilerShutdownProgress& progress)
    {
        if ( progress.totalBytes > 0 ) {
            const auto clampedUploaded =
                std::min(progress.uploadedBytes, progress.totalBytes);
            return static_cast<float>(static_cast<double>(clampedUploaded) /
                                      static_cast<double>(progress.totalBytes));
        }
        return static_cast<float>(std::fmod(ImGui::GetTime() * 0.35, 1.0));
    }
};
}  // namespace

/// @brief 在图形上下文释放前展示 PGO 退出上传进度。
/// @param context Vulkan 图形上下文。
/// @param window 原生窗口。
/// @warning 退出低频路径：只在关闭软件且实际需要上传 profraw 时执行。
void renderPgoShutdownUploadProgress(Graphic::VKContext&    context,
                                     Graphic::NativeWindow& window)
{
    const bool uploadAllowed =
        Config::AppConfig::instance().getEditorSettings().autoUploadPgoProfiles;
    if ( !beginShutdownPGOProfilerAsync(uploadAllowed) ) return;

    PgoShutdownUploadProgressHook             progressHook;
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{ &progressHook };

    do {
        context.checkAndRebuildFonts();
        context.getRenderer().render(window, graphicUserHooks);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    } while ( !isShutdownPGOProfilerFinished() );

    waitForShutdownPGOProfiler();
}
}  // namespace MMM::Main

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
    const auto startupSkinPath = Main::resolveStartupSkinPath(defaultSkinPath);
    if ( !SkinManager::instance().loadSkin(
             Config::pathToUtf8(startupSkinPath)) &&
         startupSkinPath != defaultSkinPath ) {
        XWARN("Fallback to default skin: {}",
              Config::pathToUtf8(defaultSkinPath));
        SkinManager::instance().loadSkin(Config::pathToUtf8(defaultSkinPath));
    }
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

    const auto ret = gameLoop.start(
        nativeWindow, argc, argv, Main::renderPgoShutdownUploadProgress);

    // PGO — 强制写出 profile 并按运行时长阈值上传
    Main::shutdownPGOProfiler(Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .autoUploadPgoProfiles);

    return ret;
}
