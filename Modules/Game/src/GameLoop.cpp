#include "game/GameLoop.h"
#include "audio/AudioManager.h"
#include "canvas/PreviewCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "config/FrameLimitUtils.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "game/GlobDefs.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "network/collaboration/CollaborationRoom.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/imgui/CanvasTabManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/AudioManagerView.h"
#include "ui/imgui/manager/BeatMapManagerView.h"
#include "ui/imgui/manager/CollaborationLogWindow.h"
#include "ui/imgui/manager/CollaborationView.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/manager/NewProjectWizard.h"
#include "ui/imgui/manager/SearchView.h"
#include <array>
#include <chrono>
#include <nfd.h>
#include <thread>

#ifdef _WIN32
#    include <shellapi.h>
#endif

namespace MMM
{

namespace
{
/// @brief 主循环限帧等待使用的时钟类型，要求单调递增避免系统时间跳变影响。
using FrameLimitClock = std::chrono::steady_clock;

/// @brief 限帧粗睡眠预留量，给操作系统调度精度留出少量余量。
constexpr auto FRAME_LIMIT_SLEEP_MARGIN = std::chrono::microseconds(250);

/// @brief 等待到目标帧时间点，避免用 yield 反复忙等。
/// @warning 热路径等待：主渲染循环提前到达目标帧间隔时执行；只能包含
/// sleep 和时间查询，禁止加入锁、分配或业务逻辑。
void sleepUntilFrameDeadline(FrameLimitClock::time_point deadline)
{
    while ( true ) {
        auto now = FrameLimitClock::now();
        if ( now >= deadline ) {
            return;
        }

        auto remaining = deadline - now;
        if ( remaining > FRAME_LIMIT_SLEEP_MARGIN ) {
            std::this_thread::sleep_for(remaining - FRAME_LIMIT_SLEEP_MARGIN);
        } else {
            std::this_thread::sleep_until(deadline);
            return;
        }
    }
}
}  // namespace

/**
 * @brief 获取 GameLoop 单例实例
 * @return GameLoop& 唯一实例引用
 */
GameLoop& GameLoop::instance()
{
    static GameLoop loopInstance;
    return loopInstance;
}

GameLoop::GameLoop() : g_vkContext(Graphic::VKContext::get())
{
    XINFO("GameLoop created");

    // 注册ui视图
    m_uiManager.registerView(
        "MainDockSpaceUI",
        std::make_unique<UI::MainDockSpaceUI>("MainDockSpaceUI"));
    m_uiManager.registerView("SideBarUI",
                             std::make_unique<UI::SideBarUI>("SideBarUI"));
    m_uiManager.registerView(
        "SideBarManager",
        std::make_unique<UI::FloatingManagerUI>("SideBarManager"));
    auto sidebar_manager =
        m_uiManager.getView<UI::FloatingManagerUI>("SideBarManager");
    sidebar_manager->registerSubView(
        TR("title.search_manager").toString(),
        std::make_unique<UI::SearchView>(
            TR("title.search_manager").toString()));
    sidebar_manager->registerSubView(TR("title.file_manager").toString(),
                                     std::make_unique<UI::FileManagerView>(
                                         TR("title.file_manager").toString()));
    sidebar_manager->registerSubView(TR("title.audio_manager").toString(),
                                     std::make_unique<UI::AudioManagerView>(
                                         TR("title.audio_manager").toString()));
    sidebar_manager->registerSubView(
        TR("title.beatmap_manager").toString(),
        std::make_unique<UI::BeatMapManagerView>(
            TR("title.beatmap_manager").toString()));
    auto collaborationRoom =
        std::make_shared<Network::Collaboration::CollaborationRoom>();
    m_uiManager.setCollaborationRoom(collaborationRoom.get());
    sidebar_manager->registerSubView(
        TR("title.collaboration_manager").toString(),
        std::make_unique<UI::CollaborationView>(
            TR("title.collaboration_manager").toString(), collaborationRoom));
    m_uiManager.registerView("CollaborationLogWindow",
                             std::make_unique<UI::CollaborationLogWindow>(
                                 "CollaborationLogWindow", collaborationRoom));

    // 注册新建向导
    m_uiManager.registerView("NewProjectWizard",
                             std::make_unique<UI::NewProjectWizard>());
    m_uiManager.registerView("NewBeatmapWizard",
                             std::make_unique<UI::NewBeatmapWizard>());

    auto& engine = Logic::EditorEngine::instance();

    m_uiManager.registerView("CanvasTabManager",
                             std::make_unique<UI::CanvasTabManager>());

    // 默认创建一个初始 Logo 占位画布
    engine.createSession(nullptr, TR("canvas.welcome").data(), true);

    // 注册预览窗口 (Preview Window)
    m_uiManager.registerView(
        "PreviewWindow",
        std::make_unique<Canvas::PreviewCanvas>(
            "PreviewWindow", 200, 200, engine.getSyncBuffer("Preview")));

    // 注册时间线标尺 (Timeline Window)
    m_uiManager.registerView(
        "TimelineWindow",
        std::make_unique<Canvas::TimelineCanvas>(
            "Timeline", 60, 200, engine.getSyncBuffer("Timeline")));
}

GameLoop::~GameLoop() {}

// clang-format off
/**
 * @brief 启动游戏循环
 *
 * 初始化窗口、图形上下文，并进入主消息/渲染循环。
 * 该函数会阻塞直到窗口关闭。
 *
 * @param window 窗口上下文
 * @param shutdownUiTask 关闭前可选 UI 收尾任务。
 * @return int 退出代码 (0 表示正常退出)
 */
/// @warning 热路径：进入 while 后主线程逐帧执行渲染。
/// 循环体禁止文件系统访问、完整 ECS 遍历、完整排序和每帧堆分配。
// clang-format on
int GameLoop::start(Graphic::NativeWindow& window, int argc, char* argv[],
                    ShutdownUiTask shutdownUiTask)
{
    m_uiManager.setNativeWindow(&window);

    // 初始化窗口
    // VKContext 表面资源后续初始化
    if ( g_vkContext ) {
        Runtime::AppThreadPool::instance().init();

        auto& context = g_vkContext->get();
        int   fbWidth, fbHeight;
        window.getFramebufferSize(fbWidth, fbHeight);
        if ( !context.initVKWindowRess(&window, fbWidth, fbHeight) ) {
            XERROR("Failed to initialize Vulkan window resources.");
            return EXIT_WINDOW_EXEPTION;
        }

        // 初始化音频引擎
        Audio::AudioManager::instance().init();

        // 初始化原生对话框引擎
        NFD_Init();

        // 预加载音效文件
        auto& skinData = Config::SkinManager::instance().getData();
        for ( const auto& [key, path] : skinData.audioPaths ) {
            const auto   leadInIt = skinData.audioLeadInSeconds.find(key);
            const double leadInSeconds =
                leadInIt != skinData.audioLeadInSeconds.end() ? leadInIt->second
                                                              : 0.0;
            Audio::AudioManager::instance().preloadSoundEffect(
                key, Config::pathToUtf8(path), 1.0f, leadInSeconds);
        }

        // 启动独立逻辑线程 (必须在音频加载后启动，防止字典竞态)
        Logic::EditorEngine::instance().start();

        // 处理命令行参数：如果有文件路径输入，触发打开项目/谱面事件
        if ( argc > 1 ) {
#ifdef _WIN32
            int     argcW;
            LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
            if ( argcW > 1 ) {
                std::filesystem::path inputPath(argvW[1]);
                std::error_code       inputExistsError;
                if ( std::filesystem::exists(inputPath, inputExistsError) &&
                     !inputExistsError ) {
                    Event::OpenProjectEvent openEv;
                    openEv.m_projectPath = inputPath;
                    Event::EventBus::instance().publish(openEv);
                    XINFO("Handling command line argument: {}",
                          Config::pathToUtf8(inputPath));
                }
            }
            LocalFree(argvW);
#else
            std::filesystem::path inputPath(argv[1]);
            std::error_code       inputExistsError;
            if ( std::filesystem::exists(inputPath, inputExistsError) &&
                 !inputExistsError ) {
                Event::OpenProjectEvent openEv;
                openEv.m_projectPath = inputPath;
                Event::EventBus::instance().publish(openEv);
                XINFO("Handling command line argument: {}",
                      Config::pathToUtf8(inputPath));
            }
#endif
        }

        auto   nextRenderDeadline = FrameLimitClock::now();
        double lastRenderTargetDt = 0.0;

        // 进入主循环
        while ( !window.shouldClose() ) {
            auto& settings = Config::AppConfig::instance().getEditorSettings();

            // FIFO 继续负责无撕裂呈现；同时按刷新率做 CPU 侧兜底，避免部分
            // 驱动或合成器未通过 acquire/present 对主循环形成有效背压。
            const int refreshRate =
                Config::AppConfig::instance().getDeviceRefreshRate();
            const double targetDt = Config::frameLimitTargetInterval(
                settings.frameLimit, refreshRate);

            auto currentRenderTime = FrameLimitClock::now();
            if ( targetDt > 0.0 ) {
                /// @brief 主渲染限帧使用累计 deadline，避免 Windows sleep
                /// 过冲被逐帧累计到 fps 统计中。
                /// @warning 渲染热路径：每帧只做时间计算和必要 sleep；禁止加入
                /// 业务逻辑或资源操作。
                const auto targetDuration =
                    std::chrono::duration_cast<FrameLimitClock::duration>(
                        std::chrono::duration<double>(targetDt));

                if ( targetDt != lastRenderTargetDt ) {
                    nextRenderDeadline = currentRenderTime + targetDuration;
                    lastRenderTargetDt = targetDt;
                }

                if ( currentRenderTime < nextRenderDeadline ) {
                    sleepUntilFrameDeadline(nextRenderDeadline);
                    currentRenderTime = FrameLimitClock::now();
                }

                if ( currentRenderTime - nextRenderDeadline > targetDuration ) {
                    nextRenderDeadline = currentRenderTime + targetDuration;
                } else {
                    nextRenderDeadline += targetDuration;
                }
            } else {
                nextRenderDeadline = currentRenderTime;
                lastRenderTargetDt = targetDt;
            }

            // 3.1 让操作系统处理窗口事件 (缩放、关闭、鼠标按键等)
            // 已移至渲染循环内以降低 VSync 延迟 window.pollEvents();

            // 3.1.5 处理光标 BPM 同步逻辑
            float cursorSmokeLifeOverride = -1.0f;
            if ( settings.cursorStyle == Config::CursorStyle::Software &&
                 settings.softwareCursorConfig.enableBpmSyncSmokeLife ) {
                cursorSmokeLifeOverride = Logic::EditorEngine::instance()
                                              .getCursorSmokeLifeOverride();
            }
            context.getRenderer().setCursorSmokeLifeOverride(
                cursorSmokeLifeOverride);

            // 3.2 执行渲染
            context.checkAndApplySystemTheme();
            context.checkAndRebuildFonts();
            /// @brief 本帧渲染用户钩子列表，使用栈上数组避免热路径内分配。
            std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
                &m_uiManager
            };
            context.getRenderer().render(window, graphicUserHooks);
        }

        // 保存当前项目工作区和项目配置
        m_uiManager.captureProjectWorkspaceState();
        Logic::EditorEngine::instance().saveProject();

        // 停止逻辑线程
        Logic::EditorEngine::instance().stop();

        // 保存配置
        Config::AppConfig::instance().save();

        if ( shutdownUiTask ) {
            shutdownUiTask(context, window);
        }

        // 关闭音频引擎
        Audio::AudioManager::instance().shutdown();

        // 关闭原生对话框引擎
        NFD_Quit();

        // 2. 主动清理 UI 管理器里存的所有视图
        // 这样 VKOffScreenRenderer 的析构就会在这里发生，
        // 此时 VKContext 还健在，m_device 也是有效的！
        (void)context.getLogicalDevice().waitIdle();
        m_uiManager.clearAllViews();
        context.release();
        Runtime::AppThreadPool::instance().shutdown();
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
