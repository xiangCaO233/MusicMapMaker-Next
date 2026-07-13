#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/FontPreferenceValidator.h"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fmt/core.h>
#include <imgui.h>
#include <string>
#include <thread>
#include <utility>

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
        ::MMM::UI::FeedbackOpenPopup(popupId.c_str());

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

namespace
{
/// @brief 启动资源检查窗口的逻辑宽度。
constexpr int STARTUP_WINDOW_WIDTH = 600;

/// @brief 启动资源检查进度状态的逻辑高度。
constexpr int STARTUP_PROGRESS_WINDOW_HEIGHT = 270;

/// @brief 启动资源检查错误状态的逻辑高度。
constexpr int STARTUP_ERROR_WINDOW_HEIGHT = 320;

/// @brief 正式主窗口的默认逻辑宽度。
constexpr int APPLICATION_WINDOW_WIDTH = 1400;

/// @brief 正式主窗口的默认逻辑高度。
constexpr int APPLICATION_WINDOW_HEIGHT = 900;

/// @brief 将启动窗口基础尺寸换算为包含用户 UI 倍率的逻辑尺寸。
/// @param value 未应用用户 UI 倍率的基础尺寸。
/// @return 可传给 NativeWindow 的正整数逻辑尺寸。
int scaledStartupWindowDimension(int value)
{
    float multiplier =
        Config::AppConfig::instance().getEditorSettings().uiScaleMultiplier;
    if ( !std::isfinite(multiplier) || multiplier <= 0.0f ) multiplier = 1.0f;
    return std::max(
        static_cast<int>(std::lround(static_cast<float>(value) * multiplier)),
        1);
}

/// @brief 后台资源同步任务的跨线程完成状态。
struct AssetSyncTaskState final {
    /// @brief 后台同步结束后写入的最终结果。
    Network::AssetSyncResult m_result;

    /// @brief 后台资源同步是否已经结束。
    /// @warning 跨线程原子完成位：后台线程 release 写入，主渲染线程 acquire
    /// 读取；同时保证最终结果在主线程可见。
    std::atomic<bool> m_finished{ false };
};

/// @brief 在后台同步资源，并由主线程持续渲染启动界面。
/// @param context 已初始化为 bootstrap 模式的图形上下文。
/// @param window 最终主窗口。
/// @param progressDialog 启动进度界面。
/// @param forcePreciseVerification 是否忽略版本标记并逐文件校验资源。
/// @return 本次资源同步结果。
/// @warning 启动低频循环：同步期间持续执行 Vulkan 帧循环，网络和文件系统
/// 操作只运行在后台线程。
Network::AssetSyncResult runAssetSyncWithStartupUI(
    Graphic::VKContext& context, Graphic::NativeWindow& window,
    StartupProgressDialog& progressDialog, bool forcePreciseVerification)
{
    progressDialog.beginSync();
    std::atomic<bool> cancellationRequested{ false };
    auto              options = Network::AssetSyncService::defaultOptions();
    options.forcePreciseVerification = forcePreciseVerification;
    options.progressCallback =
        [&progressDialog](const Network::AssetSyncProgress& progress) {
            progressDialog.update(progress);
        };
    options.cancellationCallback = [&cancellationRequested]() {
        return cancellationRequested.load(std::memory_order_relaxed);
    };

    AssetSyncTaskState taskState;
    std::jthread       syncThread(
        [options = std::move(options), &taskState]() mutable {
            taskState.m_result = Network::AssetSyncService::sync(options);
            taskState.m_finished.store(true, std::memory_order_release);
        });

    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
        &progressDialog
    };
    do {
        context.getRenderer().render(window, graphicUserHooks);
        if ( window.shouldClose() || progressDialog.isExitRequested() ) {
            cancellationRequested.store(true, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    } while ( !taskState.m_finished.load(std::memory_order_acquire) );

    syncThread.join();
    return std::move(taskState.m_result);
}

/// @brief 启动错误状态下等待用户选择重试或退出。
/// @param context bootstrap 图形上下文。
/// @param window 最终主窗口。
/// @param progressDialog 当前错误界面。
/// @return 用户选择重试时返回 true，退出或关闭窗口时返回 false。
/// @warning 启动低频循环：只渲染错误界面并读取按钮状态。
bool waitForStartupRetry(Graphic::VKContext&    context,
                         Graphic::NativeWindow& window,
                         StartupProgressDialog& progressDialog)
{
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
        &progressDialog
    };
    while ( !window.shouldClose() && !progressDialog.isExitRequested() ) {
        context.getRenderer().render(window, graphicUserHooks);
        if ( progressDialog.consumeRetryRequest() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return false;
}
}  // namespace
}  // namespace MMM::Main

int main(int argc, char* argv[])
{
    using namespace MMM;
    using namespace Config;

    // 先加载不依赖资源包的全局配置，供窗口缩放与呈现模式使用。
    AppConfig::instance().load();

    auto contextResult = Graphic::VKContext::get();
    if ( !contextResult ) {
        XERROR("Start Failed, graphic enc initialize failed with:\n {}",
               contextResult.error());
        return 1;
    }
    auto& context = contextResult->get();

    Graphic::NativeWindow nativeWindow(
        Main::scaledStartupWindowDimension(Main::STARTUP_WINDOW_WIDTH),
        Main::scaledStartupWindowDimension(
            Main::STARTUP_PROGRESS_WINDOW_HEIGHT),
        "MusicMapMaker(Gamma)");
    int framebufferWidth  = 0;
    int framebufferHeight = 0;
    nativeWindow.getFramebufferSize(framebufferWidth, framebufferHeight);

    UI::SetInteractionFeedbackEnabled(false);
    if ( !context.initVKWindowRess(&nativeWindow,
                                   framebufferWidth,
                                   framebufferHeight,
                                   Graphic::VKWindowResourceMode::Bootstrap) ) {
        XERROR("Failed to initialize bootstrap Vulkan window resources");
        context.release();
        return 1;
    }

    Main::StartupProgressDialog startupProgressDialog;
    const auto defaultSkinPath          = AppPaths::defaultSkinFilePath();
    bool       skinLoaded               = false;
    bool       forcePreciseVerification = false;

    while ( !skinLoaded ) {
        const auto assetSyncResult =
            Main::runAssetSyncWithStartupUI(context,
                                            nativeWindow,
                                            startupProgressDialog,
                                            forcePreciseVerification);

        if ( assetSyncResult.status == Network::AssetSyncStatus::kCancelled ||
             nativeWindow.shouldClose() ||
             startupProgressDialog.isExitRequested() ) {
            UI::SetInteractionFeedbackEnabled(true);
            context.release();
            return 0;
        }

        std::string startupErrorTitle;
        std::string startupErrorMessage;
        if ( assetSyncResult.status == Network::AssetSyncStatus::kError ) {
            const auto assetPath = AppPaths::assetsRootPath();
            startupErrorTitle    = "资源同步失败";
            startupErrorMessage =
                "无法自动下载或校验资源。请检查网络连接，或从网站下载 "
                "assets.zip 并解压到：\n\n" +
                pathToUtf8(assetPath) + "\n\n错误详情：" +
                assetSyncResult.errorMessage;
        } else {
            std::error_code defaultSkinExistsError;
            if ( !std::filesystem::exists(defaultSkinPath,
                                          defaultSkinExistsError) ) {
                startupErrorTitle = "缺少应用资源";
                startupErrorMessage =
                    "资源同步完成后仍未找到默认皮肤。请从网站下载 "
                    "assets.zip 并解压到：\n\n" +
                    pathToUtf8(AppPaths::assetsRootPath());
            } else {
                const auto startupSkinPath =
                    Main::resolveStartupSkinPath(defaultSkinPath);
                skinLoaded = SkinManager::instance().loadSkin(
                    pathToUtf8(startupSkinPath));
                if ( !skinLoaded && startupSkinPath != defaultSkinPath ) {
                    XWARN("Fallback to default skin: {}",
                          pathToUtf8(defaultSkinPath));
                    skinLoaded = SkinManager::instance().loadSkin(
                        pathToUtf8(defaultSkinPath));
                }
                if ( !skinLoaded ) {
                    startupErrorTitle = "皮肤资源加载失败";
                    startupErrorMessage =
                        "默认皮肤文件存在，但无法完成解析。可以重试资源同步，"
                        "或重新下载 assets.zip 后解压到：\n\n" +
                        pathToUtf8(AppPaths::assetsRootPath());
                }
            }
        }

        if ( skinLoaded ) break;

        XERROR("Startup asset preparation failed: {}", startupErrorMessage);
        startupProgressDialog.showError(std::move(startupErrorTitle),
                                        std::move(startupErrorMessage));
        nativeWindow.resizeAndCenter(
            Main::scaledStartupWindowDimension(Main::STARTUP_WINDOW_WIDTH),
            Main::scaledStartupWindowDimension(
                Main::STARTUP_ERROR_WINDOW_HEIGHT));
        if ( !Main::waitForStartupRetry(
                 context, nativeWindow, startupProgressDialog) ) {
            UI::SetInteractionFeedbackEnabled(true);
            context.release();
            return -1;
        }
        // 用户主动重试时绕过版本快路径，确保缺失或损坏资源真正得到修复。
        forcePreciseVerification = true;
        nativeWindow.resizeAndCenter(
            Main::scaledStartupWindowDimension(Main::STARTUP_WINDOW_WIDTH),
            Main::scaledStartupWindowDimension(
                Main::STARTUP_PROGRESS_WINDOW_HEIGHT));
    }

    if ( nativeWindow.shouldClose() ) {
        UI::SetInteractionFeedbackEnabled(true);
        context.release();
        return 0;
    }

    if ( skinLoaded && resetUnavailableFontPreferences(
                           AppConfig::instance().getEditorSettings(),
                           SkinManager::instance()) ) {
        XWARN("Unavailable font preference reset to skin default");
        if ( !AppConfig::instance().save() ) {
            XERROR("Failed to save reset font preference");
        }
    }

    nativeWindow.resizeAndCenter(Main::APPLICATION_WINDOW_WIDTH,
                                 Main::APPLICATION_WINDOW_HEIGHT);
    context.promoteBootstrapResources();
    nativeWindow.reloadWindowIcon();
    UI::SetInteractionFeedbackEnabled(true);

    XINFO(TR("tips.welcome"));

    // PGO instrumentation — 设置 profile 输出路径
    Main::initPGOProfiler();

    // 测试vulkan
    auto& gameLoop = GameLoop::instance();

    // 检查 Vulkan 环境
    if ( !gameLoop.g_vkContext ) {
        // 这里会打印 VKContext::get() 返回的初始化失败原因。
        XERROR("Start Failed, graphic enc initialize failed with:\n {}",
               gameLoop.g_vkContext.error());
        return 1;
    }

    // 正常运行
    XINFO("entering gameloop...");

    const auto ret = gameLoop.start(
        nativeWindow, argc, argv, Main::renderPgoShutdownUploadProgress);

    // PGO — 强制写出 profile 并按运行时长阈值上传
    Main::shutdownPGOProfiler(Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .autoUploadPgoProfiles);

    return ret;
}
