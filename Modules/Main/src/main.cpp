#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/CreatorIdentity.h"
#include "config/FontPreferenceValidator.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/TranslationResourceMigration.h"
#include "config/skin/translation/Translation.h"
#include "game/GameLoop.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"
#include "main/PGOProfiler.h"
#include "main/StartupProgressDialog.h"
#include "network/AssetSyncService.h"
#include "network/collaboration/CollaborationBuildFingerprint.h"
#include "network/collaboration/RtcDiagnosticLogging.h"
#include "runtime/AppThreadPool.h"
#include "ui/utils/UIWidgetUtils.h"

#include <fmt/core.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    // 空值不能解析为 skins 根目录本身，调用方应显式回退默认目录。
    if ( directoryName.empty() ) {
        return false;
    }

    // 配置只保存单级目录名，禁止绝对路径和父级片段越过受控资源根。
    const auto directoryPath = Config::utf8ToPath(directoryName);
    return !directoryPath.is_absolute() && !directoryPath.has_parent_path();
}

/// @brief 判断皮肤入口脚本是否存在。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 文件存在且是普通文件时返回 true。
bool skinLuaFileExists(const std::filesystem::path& skinLuaPath)
{
    // error_code 版本把权限或编码问题转换为不可用结果，不中断启动界面。
    std::error_code ec;
    // 同时检查普通文件，避免把同名目录交给 Lua 加载器产生误导错误。
    return std::filesystem::exists(skinLuaPath, ec) &&
           std::filesystem::is_regular_file(skinLuaPath, ec);
}

/// @brief 根据编辑器配置解析启动时应加载的皮肤入口脚本。
/// @param defaultSkinPath 默认皮肤入口脚本路径。
/// @return 可加载的皮肤入口脚本路径，配置无效时返回默认皮肤。
std::filesystem::path resolveStartupSkinPath(
    const std::filesystem::path& defaultSkinPath)
{
    // 空配置代表内置默认皮肤，保持首次启动和旧配置的兼容行为。
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const auto  selectedDirectory = settings.selectedSkinDirectory.empty()
                                        ? std::string(kDefaultSkinDirectoryName)
                                        : settings.selectedSkinDirectory;

    if ( isValidSkinDirectoryName(selectedDirectory) ) {
        // 受控单级目录与固定入口名组合，配置不能指定任意脚本路径。
        auto skinPath = Config::AppPaths::skinsRootPath();
        skinPath /= Config::utf8ToPath(selectedDirectory);
        skinPath /= "skin.lua";
        if ( skinLuaFileExists(skinPath) ) {
            return skinPath;
        }
        // 所选皮肤缺失时记录原因，并继续使用保证随资源包提供的默认项。
        XWARN("Configured skin not found: {}", Config::pathToUtf8(skinPath));
    } else {
        // 非法目录名不参与拼接，避免路径穿越后再以存在性决定是否加载。
        XWARN("Configured skin directory is invalid: {}", selectedDirectory);
    }

    // 默认路径由 AppPaths 统一解析，调用方仍会验证实际加载结果。
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
        // 弹窗只使用当前 ImGui 字体和基础几何，不创建专属 GPU 对象。
    }

    /// @brief 渲染 PGO 上传进度模态窗口。
    /// @warning 退出低频渲染路径：关闭软件时短暂执行，只读取进度快照并绘制 UI。
    void onUpdateUI() override
    {
        // 显示标题可本地化，### 后缀保证每帧使用同一个 ImGui 弹窗 ID。
        const std::string popupId = TR("ui.pgo.upload.title").toString() +
                                    "###PgoShutdownUploadProgressModal";
        // OpenPopup 每帧调用可让模态在上传期间持续存在，完成后外层停止渲染。
        ::MMM::UI::FeedbackOpenPopup(popupId.c_str());

        // 弹窗尺寸随当前内容缩放变化，并由作用域对象成对恢复临时样式。
        const float dpiScale =
            Config::AppConfig::instance().getWindowContentScale();
        UI::Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( !popupStyle.begin(
                 popupId.c_str(),
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove,
                 ImVec2(460.0f * dpiScale, 0.0f)) ) {
            // 作用域对象负责匹配 BeginPopupModal 的关闭约定，无需手工
            // EndPopup。
            return;
        }

        // 进度以值快照返回，绘制期间不持有上传线程使用的互斥锁。
        const auto progress = getPGOProfilerShutdownProgress();
        ImGui::TextWrapped("%s", TR("ui.pgo.upload.uploading").data());
        ImGui::Spacing();

        // 已知总字节时显示确定进度，未知时由辅助函数生成活动态循环。
        const float fraction = uploadFraction(progress);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));

        if ( progress.totalBytes > 0 ) {
            // KiB 仅为面向用户的紧凑展示，内部传输计数继续保留字节精度。
            const double uploadedKiB =
                static_cast<double>(progress.uploadedBytes) / 1024.0;
            const double totalKiB =
                static_cast<double>(progress.totalBytes) / 1024.0;
            const auto bytesText =
                // 翻译字符串作为运行时格式串，参数顺序由语言资源控制。
                fmt::format(fmt::runtime(TR("ui.pgo.upload.bytes_fmt").data()),
                            uploadedKiB,
                            totalKiB);
            ImGui::TextWrapped("%s", bytesText.c_str());
        }

        // 运行时长帮助用户理解样本价值，与上传百分比相互独立。
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
            // 防御回调瞬时超界，进度条永远不会超过完整比例。
            const auto clampedUploaded =
                std::min(progress.uploadedBytes, progress.totalBytes);
            return static_cast<float>(static_cast<double>(clampedUploaded) /
                                      static_cast<double>(progress.totalBytes));
        }
        // 未知长度上传以缓慢循环表示仍在活动，不伪造可完成的百分比。
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
    // 用户设置在图形和配置资源释放前读取，避免退出阶段访问失效单例。
    const bool uploadAllowed =
        Config::AppConfig::instance().getEditorSettings().autoUploadPgoProfiles;
    // 无需上传或写出失败时直接保持既有退出流程，不创建额外界面。
    if ( !beginShutdownPGOProfilerAsync(uploadAllowed) ) return;

    // 钩子为栈对象，渲染循环结束前始终覆盖其观察指针生命周期。
    PgoShutdownUploadProgressHook             progressHook;
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{ &progressHook };

    do {
        // 退出弹窗仍需响应运行期字体重建请求，保证 DPI 或字形状态有效。
        context.checkAndRebuildFonts();
        context.getRenderer().render(window, graphicUserHooks);
        // 低频退出界面限制轮询速率，上传本身在共享线程池继续推进。
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    } while ( !isShutdownPGOProfilerFinished() );

    // 终态先由进度快照发布，再等待 future 完整回收后台任务资源。
    // 此时图形上下文仍然有效，wait 返回后调用方才继续常规释放顺序。
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

/// @brief 启动翻译覆写提示状态的逻辑高度。
constexpr int STARTUP_WARNING_WINDOW_HEIGHT = 440;

/// @brief 正式主窗口的默认逻辑宽度。
constexpr int APPLICATION_WINDOW_WIDTH = 1400;

/// @brief 正式主窗口的默认逻辑高度。
constexpr int APPLICATION_WINDOW_HEIGHT = 900;

/// @brief 启动资源准备流程的最终状态。
enum class StartupPreparationResult : std::uint8_t {
    Ready,      ///< 资源与皮肤已经准备完成，可以创建正式主窗口。
    Cancelled,  ///< 用户关闭或退出资源准备窗口。
    Failed      ///< 临时图形上下文或资源准备流程初始化失败。
};

/// @brief 确保临时图形上下文在关联窗口销毁前释放 Vulkan 与 ImGui 资源。
class ScopedVKContextRelease final
{
public:
    /// @brief 绑定需要按窗口生命周期提前释放的图形上下文。
    /// @param context 临时图形上下文。
    explicit ScopedVKContextRelease(Graphic::VKContext& context)
        : m_context(context)
    {
        // 仅保存非拥有引用，外层局部变量的声明顺序保证其生命周期更长。
    }

    /// @brief 在原生窗口仍有效时释放图形资源。
    ~ScopedVKContextRelease()
    {
        // 显式 release 早于 NativeWindow 析构，满足 surface 和设备资源顺序。
        m_context.release();
    }

    ScopedVKContextRelease(ScopedVKContextRelease&&)                 = delete;
    ScopedVKContextRelease(const ScopedVKContextRelease&)            = delete;
    ScopedVKContextRelease& operator=(ScopedVKContextRelease&&)      = delete;
    ScopedVKContextRelease& operator=(const ScopedVKContextRelease&) = delete;

private:
    /// @brief 生命周期由外层启动资源准备函数保证的临时上下文引用。
    Graphic::VKContext& m_context;
};

/// @brief 将启动窗口基础尺寸换算为包含用户 UI 倍率的逻辑尺寸。
/// @param value 未应用用户 UI 倍率的基础尺寸。
/// @return 可传给 NativeWindow 的正整数逻辑尺寸。
int scaledStartupWindowDimension(int value)
{
    // 用户倍率只影响逻辑窗口尺寸，平台内容缩放仍由 NativeWindow 独立处理。
    float multiplier =
        Config::AppConfig::instance().getEditorSettings().uiScaleMultiplier;
    // 损坏配置回退到原始尺寸，避免 NaN 经 lround 产生未定义布局结果。
    if ( !std::isfinite(multiplier) || multiplier <= 0.0f ) multiplier = 1.0f;
    // 最终至少保留一个逻辑像素，使 GLFW 不接收零或负窗口尺寸。
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
/// @param window 独立的启动资源准备窗口。
/// @param progressDialog 启动进度界面。
/// @param forcePreciseVerification 是否忽略版本标记并逐文件校验资源。
/// @return 本次资源同步结果。
/// @warning 启动低频循环：同步期间持续执行 Vulkan 帧循环，网络和文件系统
/// 操作只运行在后台线程。
Network::AssetSyncResult runAssetSyncWithStartupUI(
    Graphic::VKContext& context, Graphic::NativeWindow& window,
    StartupProgressDialog& progressDialog, bool forcePreciseVerification)
{
    // 每次重试重新初始化界面和请求标志，不继承上一轮错误状态。
    progressDialog.beginSync();
    // 取消位由渲染线程写、后台同步线程读，只有单一布尔状态无需更强顺序。
    std::atomic<bool> cancellationRequested{ false };
    // 默认选项集中维护服务地址、资源根和校验策略，本函数只覆盖本轮差异。
    auto options = Network::AssetSyncService::defaultOptions();
    options.forcePreciseVerification = forcePreciseVerification;
    // 后台回调只发布值快照，StartupProgressDialog 负责跨线程同步。
    options.progressCallback =
        [&progressDialog](const Network::AssetSyncProgress& progress) {
            progressDialog.update(progress);
        };
    // 服务在自己的网络和文件循环中轮询取消位，渲染线程不会阻塞等待响应。
    options.cancellationCallback = [&cancellationRequested]() {
        // relaxed 足够表达单向取消事实，不通过该原子发布其他数据。
        return cancellationRequested.load(std::memory_order_relaxed);
    };

    // 结果和完成位共处栈状态，future 回收前函数不会离开该作用域。
    AssetSyncTaskState taskState;
    // 资源同步复用全局工作池，启动阶段若未初始化则构造明确错误结果。
    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        // 缺少共享池表示启动顺序被破坏，返回可由同一错误界面展示的结果。
        Network::AssetSyncResult result;
        result.status       = Network::AssetSyncStatus::kError;
        result.errorMessage = "Runtime thread pool is not initialized";
        return result;
    }
    // options 移入任务避免异步执行期间引用本函数的临时配置对象。
    auto syncFuture = appThreadPool->enqueue(
        [options = std::move(options), &taskState]() mutable {
            // 先完整写入结果，再以 release 发布完成位供渲染线程消费。
            taskState.m_result = Network::AssetSyncService::sync(options);
            taskState.m_finished.store(true, std::memory_order_release);
        });

    // 启动界面是本轮唯一图形钩子，数组只在同步 future 完成前使用。
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
        &progressDialog
    };
    do {
        // 主线程持续提交图形帧，网络、校验和解压均留在后台任务。
        context.getRenderer().render(window, graphicUserHooks);
        if ( window.shouldClose() || progressDialog.isExitRequested() ) {
            // 请求取消不等待服务立即停止，循环继续渲染直至任务发布终态。
            cancellationRequested.store(true, std::memory_order_relaxed);
        }
        // 8 毫秒只限制启动等待循环占用，不用于业务状态同步或视觉延迟。
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    } while ( !taskState.m_finished.load(std::memory_order_acquire) );

    // acquire 已保证结果可见，wait 进一步回收线程池任务的 future 状态。
    syncFuture.wait();
    // future 已完成后结果对象不再被后台写入，可以安全移动给调用方。
    return std::move(taskState.m_result);
}

/// @brief 启动错误状态下等待用户选择重试或退出。
/// @param context bootstrap 图形上下文。
/// @param window 独立的启动资源准备窗口。
/// @param progressDialog 当前错误界面。
/// @return 用户选择重试时返回 true，退出或关闭窗口时返回 false。
/// @warning 启动低频循环：只渲染错误界面并读取按钮状态。
bool waitForStartupRetry(Graphic::VKContext&    context,
                         Graphic::NativeWindow& window,
                         StartupProgressDialog& progressDialog)
{
    // 钩子数组生命周期覆盖整个等待循环，渲染器只借用其中指针。
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
        &progressDialog
    };
    while ( !window.shouldClose() && !progressDialog.isExitRequested() ) {
        // 错误界面仍保持窗口事件和 Vulkan 帧循环推进。
        context.getRenderer().render(window, graphicUserHooks);
        // 请求采用消费语义，true 只会结束本轮一次。
        if ( progressDialog.consumeRetryRequest() ) return true;
        // 低频等待限制空闲 CPU，占用时间不参与同步正确性判断。
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    // 循环只在关闭或退出请求下结束，因此没有重试时统一返回 false。
    return false;
}

/// @brief 启动提示状态下等待用户选择继续或退出。
/// @param context bootstrap 图形上下文。
/// @param window 独立的启动资源准备窗口。
/// @param progressDialog 当前提示界面。
/// @return 用户确认并继续时返回 true，退出或关闭窗口时返回 false。
/// @warning 启动低频循环：只渲染提示界面并读取按钮状态。
bool waitForStartupContinue(Graphic::VKContext&    context,
                            Graphic::NativeWindow& window,
                            StartupProgressDialog& progressDialog)
{
    // 警告等待与重试等待独立，按钮语义不会在两个状态间串用。
    std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
        &progressDialog
    };
    while ( !window.shouldClose() && !progressDialog.isExitRequested() ) {
        // 每帧绘制长文本子窗口并处理继续或退出输入。
        context.getRenderer().render(window, graphicUserHooks);
        if ( progressDialog.consumeContinueRequest() ) return true;
        // 休眠仅做退出界面限频，不阻塞后台任务或延迟本地按钮反馈。
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    // 关闭路径不会伪造继续确认，调用方据此终止启动。
    return false;
}

/// @brief 生成皮肤翻译覆写未知字段的启动提示内容。
/// @param fields 带语言 ID 的未知字段列表。
/// @return 可完整展示给用户的多行提示文本。
std::string buildTranslationOverrideWarning(
    const std::vector<std::string>& fields)
{
    // 固定前言解释处理策略，字段列表保持服务报告的原始语言 ID。
    std::string message =
        "当前皮肤包含默认语言字典中不存在的翻译覆写字段。以下字段已被忽略，"
        "其他有效覆写仍会正常使用：\n\n";
    for ( const auto& field : fields ) {
        // Markdown 风格项目符号便于在可换行 ImGui 文本中逐项浏览。
        message += "- ";
        message += field;
        message += '\n';
    }
    // 调用方只在列表非空时展示，因此无需为无字段状态生成替代文案。
    return message;
}

/// @brief 使用独立窗口和独立图形上下文完成启动资源校验与皮肤加载。
/// @return 资源准备结果；只有 Ready 状态允许继续创建正式主窗口。
/// @warning 启动低频路径：内部创建并完整释放一套 GLFW、Vulkan 与 ImGui
/// 资源，禁止从主渲染循环调用。
StartupPreparationResult prepareStartupAssets()
{
    // 临时上下文先于窗口构造，失败时不创建任何可见启动界面。
    Graphic::VKContext startupContext;
    if ( !startupContext.getInitializationError().empty() ) {
        // 初始化原因来自 VKContext 内部诊断，保留原文便于驱动问题定位。
        XERROR("Failed to initialize startup graphic context: {}",
               startupContext.getInitializationError());
        return StartupPreparationResult::Failed;
    }

    // 启动窗口使用独立模式，避免读取正式编辑器窗口的保存位置与尺寸。
    Graphic::NativeWindow startupWindow(
        scaledStartupWindowDimension(STARTUP_WINDOW_WIDTH),
        scaledStartupWindowDimension(STARTUP_PROGRESS_WINDOW_HEIGHT),
        "MusicMapMaker - Resource Update",
        Graphic::NativeWindowMode::Startup);
    // 释放守卫在窗口之后构造，因此离开作用域时会先释放 Vulkan 上下文。
    ScopedVKContextRelease contextRelease(startupContext);
    if ( !startupWindow.getWindowHandle() ) {
        // 守卫仍会调用幂等 release，随后窗口对象安全析构空句柄。
        XERROR("Failed to create startup resource window");
        return StartupPreparationResult::Failed;
    }

    // Vulkan swapchain 必须使用平台返回的 framebuffer 像素尺寸而非逻辑尺寸。
    int framebufferWidth  = 0;
    int framebufferHeight = 0;
    startupWindow.getFramebufferSize(framebufferWidth, framebufferHeight);
    // Bootstrap 模式只准备启动进度界面所需的最小渲染资源和字体。
    if ( !startupContext.initVKWindowRess(
             &startupWindow,
             framebufferWidth,
             framebufferHeight,
             Graphic::VKWindowResourceMode::Bootstrap) ) {
        // 部分创建的 Vulkan 资源由上下文释放守卫统一回收。
        XERROR("Failed to initialize startup Vulkan window resources");
        return StartupPreparationResult::Failed;
    }

    // 对话框跨重试复用，beginSync 会在每轮开始时清理错误和按钮状态。
    StartupProgressDialog startupProgressDialog;
    // 默认皮肤是资源准备完成的最终哨兵，必须在进入正式 UI 前可加载。
    const auto defaultSkinPath = Config::AppPaths::defaultSkinFilePath();
    bool       skinLoaded      = false;
    // 首轮可使用版本快路径；用户重试后强制逐文件验证以真正修复损坏。
    bool forcePreciseVerification = false;

    while ( !skinLoaded ) {
        // 每轮同步在后台执行，当前线程持续拥有和渲染临时图形资源。
        const auto assetSyncResult =
            runAssetSyncWithStartupUI(startupContext,
                                      startupWindow,
                                      startupProgressDialog,
                                      forcePreciseVerification);

        // 服务取消、窗口关闭和 UI 退出请求统一映射为正常用户取消结果。
        if ( assetSyncResult.status == Network::AssetSyncStatus::kCancelled ||
             startupWindow.shouldClose() ||
             startupProgressDialog.isExitRequested() ) {
            return StartupPreparationResult::Cancelled;
        }

        // 错误文本在每轮重新构造，只有 skinLoaded=false 时才进入重试界面。
        std::string startupErrorTitle;
        std::string startupErrorMessage;
        if ( assetSyncResult.status == Network::AssetSyncStatus::kError ) {
            // 同步错误同时给出离线手工恢复目录，用户不依赖程序自动下载。
            const auto assetPath = Config::AppPaths::assetsRootPath();
            startupErrorTitle    = "资源同步失败";
            startupErrorMessage =
                "无法自动下载或校验资源。请检查网络连接，或从网站下载 "
                "assets.zip 并解压到：\n\n" +
                Config::pathToUtf8(assetPath) + "\n\n错误详情：" +
                assetSyncResult.errorMessage;
        } else {
            // kFinished 之外的非错误终态仍通过后续文件检查验证实际资源。
            // 资源可用后先迁移旧翻译文件，皮肤加载只能读取新目录布局。
            const auto migrationResult =
                Config::migrateLegacySkinTranslationFiles();
            if ( !migrationResult.completed ) {
                // 迁移失败视为可重试资源错误，并提供完整 assets 根目录。
                startupErrorTitle = "翻译资源迁移失败";
                startupErrorMessage =
                    migrationResult.errorMessage +
                    "\n\n请重试资源同步，或重新下载 assets.zip 后解压到：\n\n" +
                    Config::pathToUtf8(Config::AppPaths::assetsRootPath());
            } else {
                if ( !migrationResult.removedFiles.empty() ) {
                    // 仅记录删除数量，不把可能含用户目录的完整路径逐项写入日志。
                    XINFO("Removed {} legacy skin translation file(s)",
                          migrationResult.removedFiles.size());
                }

                // 同步报告成功后仍独立验证默认入口，防御残缺压缩包或权限问题。
                std::error_code defaultSkinExistsError;
                if ( !std::filesystem::exists(defaultSkinPath,
                                              defaultSkinExistsError) ) {
                    // exists 的错误码和确实缺失都对用户表现为资源不可用。
                    startupErrorTitle = "缺少应用资源";
                    startupErrorMessage =
                        "资源同步完成后仍未找到默认皮肤。请从网站下载 "
                        "assets.zip 并解压到：\n\n" +
                        Config::pathToUtf8(Config::AppPaths::assetsRootPath());
                } else {
                    // 用户选择只决定首选路径，解析失败仍必须回退随包默认皮肤。
                    const auto startupSkinPath =
                        resolveStartupSkinPath(defaultSkinPath);
                    skinLoaded = Config::SkinManager::instance().loadSkin(
                        Config::pathToUtf8(startupSkinPath));
                    if ( !skinLoaded && startupSkinPath != defaultSkinPath ) {
                        // 仅当首选不是默认项时重试，避免对同一路径加载两次。
                        XWARN("Fallback to default skin: {}",
                              Config::pathToUtf8(defaultSkinPath));
                        skinLoaded = Config::SkinManager::instance().loadSkin(
                            Config::pathToUtf8(defaultSkinPath));
                    }
                    if ( !skinLoaded ) {
                        // 默认文件存在但不可解析时保留重试入口，让精确同步替换它。
                        startupErrorTitle = "皮肤资源加载失败";
                        startupErrorMessage =
                            "默认皮肤文件存在，但无法完成解析。可以重试资源同步"
                            "，"
                            "或重新下载 assets.zip 后解压到：\n\n" +
                            Config::pathToUtf8(
                                Config::AppPaths::assetsRootPath());
                    }
                }
            }
        }

        // 成功加载立即退出重试循环，不再触碰仅失败分支使用的错误字符串。
        if ( skinLoaded ) break;

        // 错误日志用于诊断，界面文本同时保留用户可执行的恢复方案。
        XERROR("Startup asset preparation failed: {}", startupErrorMessage);
        startupProgressDialog.showError(std::move(startupErrorTitle),
                                        std::move(startupErrorMessage));
        // 错误详情需要比普通进度状态更高的窗口以减少文本截断。
        startupWindow.resizeAndCenter(
            scaledStartupWindowDimension(STARTUP_WINDOW_WIDTH),
            scaledStartupWindowDimension(STARTUP_ERROR_WINDOW_HEIGHT));
        if ( !waitForStartupRetry(
                 startupContext, startupWindow, startupProgressDialog) ) {
            // 关闭窗口或点击退出都结束整个启动准备阶段。
            return StartupPreparationResult::Cancelled;
        }

        // 用户主动重试时绕过版本快路径，确保缺失或损坏资源真正得到修复。
        forcePreciseVerification = true;
        // 回到进度状态时恢复紧凑高度，并重新居中避免窗口下沿漂移。
        startupWindow.resizeAndCenter(
            scaledStartupWindowDimension(STARTUP_WINDOW_WIDTH),
            scaledStartupWindowDimension(STARTUP_PROGRESS_WINDOW_HEIGHT));
    }

    // 皮肤加载成功后检查翻译覆写契约，未知键不阻止其他有效覆写生效。
    const auto& unknownOverrideFields = Config::SkinManager::instance()
                                            .getData()
                                            .missingTranslationOverrideFields;
    if ( !unknownOverrideFields.empty() ) {
        // 用户必须明确确认忽略未知键，避免静默掩盖皮肤版本不匹配。
        startupProgressDialog.showWarning(
            "皮肤翻译覆写包含未知字段",
            buildTranslationOverrideWarning(unknownOverrideFields));
        // 警告列表可能较长，扩展窗口并使用对话框内部滚动区域承载。
        startupWindow.resizeAndCenter(
            scaledStartupWindowDimension(STARTUP_WINDOW_WIDTH),
            scaledStartupWindowDimension(STARTUP_WARNING_WINDOW_HEIGHT));
        if ( !waitForStartupContinue(
                 startupContext, startupWindow, startupProgressDialog) ) {
            // 未确认未知字段时不进入可能显示错误翻译的正式界面。
            return StartupPreparationResult::Cancelled;
        }
    }

    // 最后再次读取窗口关闭位，覆盖用户在状态切换边界点击关闭的情况。
    return startupWindow.shouldClose() ? StartupPreparationResult::Cancelled
                                       : StartupPreparationResult::Ready;
}
}  // namespace
}  // namespace MMM::Main

/// @brief 初始化运行期基础设施、准备资源并进入正式游戏循环。
/// @param argc 命令行参数数量。
/// @param argv 传递给 GameLoop 的命令行参数数组。
/// @return 正常退出时返回 GameLoop 结果，启动失败时返回非零值。
int main(int argc, char* argv[])
{
    using namespace MMM;
    using namespace Config;

    // 共享池必须早于指纹、资源同步、音频和逻辑子系统的后台任务初始化。
    auto& appThreadPool = Runtime::AppThreadPool::instance();
    appThreadPool.init();
    // 指纹计算与资源启动界面并行；调度失败只禁用协作能力，不阻止本地启动。
    if ( !Network::Collaboration::
             startCollaborationBuildFingerprintInitialization() ) {
        XERROR("Failed to schedule collaboration client build fingerprint");
    }

    // 先加载不依赖资源包的全局配置，供窗口缩放与呈现模式使用。
    AppConfig::instance().load();
    // WebRTC 诊断开关在协作会话创建前发布，确保首条日志遵循用户设置。
    Network::Collaboration::setRtcDiagnosticLoggingEnabled(
        AppConfig::instance().getEditorSettings().rtcDiagnosticLogging);
    // 环境身份覆盖只在显式非空时启用，普通启动继续使用持久配置。
    if ( const char* creatorOverride = std::getenv("MMM_CREATOR");
         creatorOverride && creatorOverride[0] != '\0' ) {
        // 环境覆盖主要服务隔离测试，仍复用正式身份规范化规则。
        const auto creator = normalizeCreatorIdentity(creatorOverride);
        if ( !creator.empty() ) {
            // 只修改本进程内配置，不在这里保存测试覆盖到用户配置文件。
            AppConfig::instance().getEditorSettings().defaultCreator = creator;
            XINFO("Using isolated collaboration Creator: {}", creator);
        }
        // 规范化为空时忽略覆盖，避免用非法环境值清除持久身份。
    }

    // 启动同步界面尚未加载完整音效资源，因此临时关闭按钮反馈声音。
    UI::SetInteractionFeedbackEnabled(false);
    const auto startupResult = Main::prepareStartupAssets();
    // 资源和皮肤准备完成后恢复正式 UI 的统一按钮反馈。
    UI::SetInteractionFeedbackEnabled(true);
    if ( startupResult == Main::StartupPreparationResult::Cancelled ) {
        // 用户取消属于正常退出，仍需显式回收可能运行过同步任务的线程池。
        appThreadPool.shutdown();
        return 0;
    }
    if ( startupResult == Main::StartupPreparationResult::Failed ) {
        // 图形初始化等不可恢复错误返回失败码，供启动器或脚本识别。
        appThreadPool.shutdown();
        return 1;
    }

    // 皮肤就绪后才能验证字体标识；不可用偏好回退并持久化修正结果。
    if ( resetUnavailableFontPreferences(
             AppConfig::instance().getEditorSettings(),
             SkinManager::instance()) ) {
        XWARN("Unavailable font preference reset to skin default");
        if ( !AppConfig::instance().save() ) {
            // 保存失败不影响本次已修正的内存设置，后续启动可能再次执行回退。
            XERROR("Failed to save reset font preference");
        }
    }

    XINFO(TR("tips.welcome"));

    // PGO instrumentation — 设置 profile 输出路径
    // 非插桩构建中该入口为空操作，主流程无需条件编译。
    Main::initPGOProfiler();

    // GameLoop 单例在这里建立正式 Vulkan 上下文和其余业务系统。
    auto& gameLoop = GameLoop::instance();

    // 检查 Vulkan 环境
    if ( !gameLoop.g_vkContext ) {
        // 这里会打印 VKContext::get() 返回的初始化失败原因。
        XERROR("Start Failed, graphic enc initialize failed with:\n {}",
               gameLoop.g_vkContext.error());
        // 正式窗口尚未创建，直接关闭共享池即可结束失败启动。
        appThreadPool.shutdown();
        return 1;
    }

    // 图形上下文可用后进入正式窗口与游戏循环。
    XINFO("entering gameloop...");

    // 主窗口使用用户布局系统，尺寸常量只提供首次启动默认值。
    Graphic::NativeWindow nativeWindow(Main::APPLICATION_WINDOW_WIDTH,
                                       Main::APPLICATION_WINDOW_HEIGHT,
                                       "MusicMapMaker(Gamma)");

    // 退出回调在图形上下文仍有效时显示可能需要的 PGO 上传进度。
    // argc 与 argv 原样交给 GameLoop，使命令行打开项目等入口保持集中处理。
    const auto ret = gameLoop.start(
        nativeWindow, argc, argv, Main::renderPgoShutdownUploadProgress);

    // 异步展示路径未启动时，这个幂等入口完成写出或等待既有上传。
    Main::shutdownPGOProfiler(Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .autoUploadPgoProfiles);

    // 线程池的常规回收由 GameLoop 退出序列完成，Main 不重复持有其任务。
    // GameLoop 负责常规业务资源退出，原样传播其终止状态给调用环境。
    return ret;
}
