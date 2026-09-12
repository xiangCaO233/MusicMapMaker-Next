#include "game/GameLoop.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/FrameLimitUtils.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "game/CanvasWorkspaceService.h"
#include "game/EditorApplicationService.h"
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
#include <utility>

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
///
/// 剩余时间大于该值时先 sleep_for 粗等待，最后余量交给 sleep_until，减少
/// 调度过冲同时避免纯 yield 忙等。
constexpr auto FRAME_LIMIT_SLEEP_MARGIN = std::chrono::microseconds(250);

/// @brief GLFW 请求关闭后允许全部业务线程和后端正常收尾的最长时间。
///
/// 看门狗只覆盖退出阶段，用于在逻辑、音频或图形后端永久卡住时终止进程。
constexpr auto APPLICATION_SHUTDOWN_TIMEOUT = std::chrono::seconds(45);

/// @brief 等待到目标帧时间点，避免用 yield 反复忙等。
/// @param deadline 单调时钟上的下一帧绝对目标时间。
///
/// 粗等待后重新读取时钟；进入预留区后直接 sleep_until 并返回。操作系统提前
/// 唤醒时外层循环会再次计算 remaining，不执行固定次数自旋。
/// @warning 热路径等待：主渲染循环提前到达目标帧间隔时执行；只能包含
/// sleep 和时间查询，禁止加入锁、分配或业务逻辑。
void sleepUntilFrameDeadline(FrameLimitClock::time_point deadline)
{
    while ( true ) {
        // 每次唤醒后重新取单调时钟，系统时间校准不会影响 deadline。
        auto now = FrameLimitClock::now();
        if ( now >= deadline ) {
            return;
        }

        auto remaining = deadline - now;
        if ( remaining > FRAME_LIMIT_SLEEP_MARGIN ) {
            // 先扣除预留量，给调度器和最终精确等待留出空间。
            std::this_thread::sleep_for(remaining - FRAME_LIMIT_SLEEP_MARGIN);
        } else {
            // 最后阶段交给操作系统绝对时间等待，不使用 CPU 忙等。
            std::this_thread::sleep_until(deadline);
            return;
        }
    }
}
}  // namespace

/// @brief 获取进程唯一 GameLoop 实例。
/// @return 函数内静态对象引用。
///
/// C++ 静态局部初始化保证首次访问时构造，实例生命周期覆盖 UI、逻辑线程和
/// 图形上下文协调阶段。调用方不得保存到静态析构之后。
GameLoop& GameLoop::instance()
{
    static GameLoop loopInstance;
    return loopInstance;
}

/// @brief 建立 UI 服务、全局窗口与初始画布视图。
///
/// 构造只组装对象关系，不初始化 Vulkan 窗口资源、音频后端或逻辑线程；这些
/// 需要 NativeWindow 的步骤延迟到 start。UIManager 接管所有 unique_ptr 视图，
/// CollaborationRoom 通过 shared_ptr 跨协作视图和日志窗口共享生命周期。
///
/// 注册顺序保证工作区服务先于画布创建，批注表先于 Timeline 独立存在，初始
/// Logo 占位会话为没有项目的启动状态提供稳定标签页。
GameLoop::GameLoop() : g_vkContext(Graphic::VKContext::get())
{
    XINFO("GameLoop created");

    // 先注入 UI 与逻辑层之间的服务适配器，再构造依赖它们的视图。
    m_uiManager.setCanvasWorkspaceService(
        std::make_unique<Game::CanvasWorkspaceService>());
    m_uiManager.setEditorApplicationService(
        std::make_unique<Game::EditorApplicationService>());

    // 注册主停靠区、侧栏与承载侧栏子视图的浮动管理器。
    m_uiManager.registerView(
        "MainDockSpaceUI",
        std::make_unique<UI::MainDockSpaceUI>("MainDockSpaceUI"));
    m_uiManager.registerView("SideBarUI",
                             std::make_unique<UI::SideBarUI>("SideBarUI"));
    m_uiManager.registerView(
        "SideBarManager",
        std::make_unique<UI::FloatingManagerUI>("SideBarManager"));
    // FloatingManagerUI 由 UIManager 拥有，此处观察指针仅用于构造期登记子视图。
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
    // 协作房间在管理视图和日志窗口间共享，退出时由这些所有者共同释放。
    auto collaborationRoom =
        std::make_shared<Network::Collaboration::CollaborationRoom>();
    // 持久化服务器配置按值转换为网络层端点，UI 不直接持有设置引用。
    const auto& collaborationServer =
        Config::AppConfig::instance().getEditorSettings().collaborationServer;
    Network::Collaboration::CollaborationServerEndpoint endpoint;
    endpoint.address       = collaborationServer.address;
    endpoint.signalingPort = collaborationServer.signalingPort;
    endpoint.useTls        = collaborationServer.useTls;
    if ( !collaborationRoom->setServerEndpoint(std::move(endpoint)) ) {
        // 无效旧配置只记录错误，房间对象仍保持可用以便用户在设置中修正。
        XERROR("Invalid persisted collaboration server endpoint");
    }
    // UIManager 保存非拥有访问，shared_ptr 由两个已注册协作视图维持。
    m_uiManager.setCollaborationRoom(collaborationRoom.get());
    sidebar_manager->registerSubView(
        TR("title.collaboration_manager").toString(),
        std::make_unique<UI::CollaborationView>(
            TR("title.collaboration_manager").toString(), collaborationRoom));
    m_uiManager.registerView("CollaborationLogWindow",
                             std::make_unique<UI::CollaborationLogWindow>(
                                 "CollaborationLogWindow", collaborationRoom));

    // 新建向导作为常驻视图注册，显示状态由菜单动作控制。
    m_uiManager.registerView("NewProjectWizard",
                             std::make_unique<UI::NewProjectWizard>());
    m_uiManager.registerView("NewBeatmapWizard",
                             std::make_unique<UI::NewBeatmapWizard>());

    m_uiManager.registerView("CanvasTabManager",
                             std::make_unique<UI::CanvasTabManager>());

    // 服务指针由 UIManager 拥有，在 GameLoop 生命周期内稳定。
    auto* workspace = m_uiManager.getCanvasWorkspaceService();

    // 默认创建一个初始 Logo 占位画布
    workspace->createLogoPlaceholderSession(TR("canvas.welcome").toString());

    // 注册预览窗口 (Preview Window)
    m_uiManager.registerView(
        "PreviewWindow",
        workspace->createPreviewCanvas("PreviewWindow", 200, 200));

    // 批注表先于 Timeline 永久注册，数据与窗口生命周期均不依赖 Timeline。
    auto annotationTable =
        workspace->createAnnotationTableWindow("AnnotationTableWindow");
    if ( annotationTable ) {
        m_uiManager.registerView("AnnotationTableWindow",
                                 std::move(annotationTable));
    }

    // 注册时间线标尺 (Timeline Window)
    m_uiManager.registerView(
        "TimelineWindow", workspace->createTimelineCanvas("Timeline", 60, 200));
}

/// @brief 销毁 GameLoop 持有的服务和视图容器。
///
/// 正常图形资源释放已在 start 退出顺序中显式完成，析构函数不重复访问
/// VKContext。
GameLoop::~GameLoop() {}

// clang-format off
/**
 * @brief 启动游戏循环
 *
 * 初始化窗口、图形上下文，并进入主消息/渲染循环。
 * 该函数会阻塞直到窗口关闭。
 *
 * @param window 窗口上下文
 * @param argc 命令行参数数量。
 * @param argv UTF-8 或平台约定编码的命令行参数数组。
 * @param shutdownUiTask 关闭前可选 UI 收尾任务。
 * @return int 退出代码 (0 表示正常退出)
 *
 * 启动顺序为线程池、Vulkan 窗口资源、音频、原生文件对话框、皮肤音效预载、
 * 逻辑线程和命令行打开请求。退出顺序先启动看门狗，再保存工作区与项目、停止
 * 逻辑线程、保存软件配置、执行调用方 UI 收尾、关闭音频和文件对话框，最后在
 * Vulkan 设备空闲后清空 UI 资源并释放上下文。
 */
/// @warning 热路径：进入 while 后主线程逐帧执行渲染。
/// 循环体禁止文件系统访问、完整 ECS 遍历、完整排序和每帧堆分配。
// clang-format on
int GameLoop::start(Graphic::NativeWindow& window, int argc, char* argv[],
                    ShutdownUiTask shutdownUiTask)
{
    // UIManager 保存非拥有窗口访问，生命周期由调用 start 的上层保证。
    m_uiManager.setNativeWindow(&window);

    // 只有全局 VKContext 可用时才进入后端初始化；否则立即返回窗口异常码。
    if ( g_vkContext ) {
        // 应用线程池先启动，供后续逻辑和资源任务共享。
        auto& appThreadPool = Runtime::AppThreadPool::instance();
        appThreadPool.init();

        auto& context = g_vkContext->get();
        int   fbWidth, fbHeight;
        // Vulkan 表面资源使用物理 framebuffer 尺寸而非逻辑窗口尺寸。
        window.getFramebufferSize(fbWidth, fbHeight);
        if ( !context.initVKWindowRess(&window, fbWidth, fbHeight) ) {
            XERROR("Failed to initialize Vulkan window resources.");
            return EXIT_WINDOW_EXEPTION;
        }

        // 音频必须先于逻辑线程启动，避免逻辑线程首次访问未初始化的音效字典。
        Audio::AudioManager::instance().init();

        // 原生对话框库的进程级初始化与退出在同一 start 调用内配对。
        NFD_Init();

        // 预加载当前皮肤声明的交互音效，降低首次 UI 反馈延迟。
        auto& skinData = Config::SkinManager::instance().getData();
        for ( const auto& [key, path] : skinData.audioPaths ) {
            // 每个音效可选独立前导时间，缺失时按零秒处理。
            const auto   leadInIt = skinData.audioLeadInSeconds.find(key);
            const double leadInSeconds =
                leadInIt != skinData.audioLeadInSeconds.end() ? leadInIt->second
                                                              : 0.0;
            Audio::AudioManager::instance().preloadSoundEffect(
                key, Config::pathToUtf8(path), 1.0f, leadInSeconds);
        }

        // 启动独立逻辑线程 (必须在音频加载后启动，防止字典竞态)
        Logic::EditorEngine::instance().start();

        // 首个命令行路径存在时发布统一打开事件，由项目控制器判断具体类型。
        if ( argc > 1 ) {
#ifdef _WIN32
            // Windows 重新读取宽字符命令行，避免 argv 中的本地路径编码损失。
            int     argcW;
            LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
            if ( argcW > 1 ) {
                std::filesystem::path inputPath(argvW[1]);
                // error_code 重载保证无权限或无效路径不会抛出异常。
                std::error_code inputExistsError;
                if ( std::filesystem::exists(inputPath, inputExistsError) &&
                     !inputExistsError ) {
                    Event::OpenProjectEvent openEv;
                    openEv.m_projectPath = inputPath;
                    Event::EventBus::instance().publish(openEv);
                    XINFO("Handling command line argument: {}",
                          Config::pathToUtf8(inputPath));
                }
            }
            // CommandLineToArgvW 返回内存由 LocalFree 释放。
            LocalFree(argvW);
#else
            // POSIX 平台按进程 argv 字节构造 filesystem 路径。
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

        // 累计 deadline 与上一目标间隔共同处理限帧配置热切换。
        auto   nextRenderDeadline = FrameLimitClock::now();
        double lastRenderTargetDt = 0.0;

        // 进入主循环
        while ( !window.shouldClose() ) {
            // 本帧只读取设置引用，用户修改在下一帧自然生效。
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
                    // FPS 或刷新率目标改变时重置
                    // deadline，避免沿用旧节奏累计误差。
                    nextRenderDeadline = currentRenderTime + targetDuration;
                    lastRenderTargetDt = targetDt;
                }

                if ( currentRenderTime < nextRenderDeadline ) {
                    sleepUntilFrameDeadline(nextRenderDeadline);
                    currentRenderTime = FrameLimitClock::now();
                }

                if ( currentRenderTime - nextRenderDeadline > targetDuration ) {
                    // 落后一整帧以上时从当前时间重新起步，防止追赶式连续渲染。
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
            // 负值表示不覆盖皮肤默认烟雾寿命，仅软件光标 BPM 同步时查询逻辑值。
            float cursorSmokeLifeOverride = -1.0f;
            if ( settings.cursorStyle == Config::CursorStyle::Software &&
                 settings.softwareCursorConfig.enableBpmSyncSmokeLife ) {
                cursorSmokeLifeOverride = Logic::EditorEngine::instance()
                                              .getCursorSmokeLifeOverride();
            }
            context.getRenderer().setCursorSmokeLifeOverride(
                cursorSmokeLifeOverride);

            // 3.2 执行渲染
            // 主题和字体变更在正式录制本帧命令前处理。
            context.checkAndApplySystemTheme();
            context.checkAndRebuildFonts();
            /// @brief 本帧渲染用户钩子列表，使用栈上数组避免热路径内分配。
            std::array<Graphic::IGraphicUserHook*, 1> graphicUserHooks{
                &m_uiManager
            };
            // UIManager 作为唯一图形用户钩子，在渲染器抽象回调中录制 UI 命令。
            context.getRenderer().render(window, graphicUserHooks);
        }

        // 窗口关闭后启动看门狗，正常收尾完成时显式解除。
        appThreadPool.armApplicationShutdownWatchdog(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                APPLICATION_SHUTDOWN_TIMEOUT));

        // 保存当前项目工作区和项目配置
        m_uiManager.captureProjectWorkspaceState();
        Logic::EditorEngine::instance().saveProject();

        // 停止逻辑线程
        Logic::EditorEngine::instance().stop();

        // 保存配置
        Config::AppConfig::instance().save();

        if ( shutdownUiTask ) {
            // 调用方收尾仍可访问有效 Vulkan 上下文和原生窗口。
            shutdownUiTask(context, window);
        }

        // 关闭音频引擎
        Audio::AudioManager::instance().shutdown();

        // 关闭原生对话框引擎
        NFD_Quit();

        // 2. 主动清理 UI 管理器里存的所有视图
        // 这样 VKOffScreenRenderer 的析构就会在这里发生，
        // 此时 VKContext 还健在，m_device 也是有效的！
        // Vulkan 资源析构前等待设备空闲，这是退出阶段的必要同步点。
        (void)context.getLogicalDevice().waitIdle();
        m_uiManager.clearAllViews();
        // UI 离屏资源销毁后才能释放 Vulkan 上下文和线程池。
        context.release();
        appThreadPool.shutdown();
        appThreadPool.completeApplicationShutdownWatchdog();
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
