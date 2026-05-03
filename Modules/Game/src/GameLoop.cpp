#include "game/GameLoop.h"
#include "audio/AudioManager.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/PreviewCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "game/GlobDefs.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/DebugWindowUI.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/AudioManagerView.h"
#include "ui/imgui/manager/BeatMapManagerView.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/manager/SearchView.h"
#include "ui/imgui/manager/SettingsView.h"
#include <chrono>
#include <nfd.h>

namespace MMM
{

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
        TR("title.search_manager"),
        std::make_unique<UI::SearchView>(TR("title.search_manager")));
    sidebar_manager->registerSubView(
        TR("title.file_manager"),
        std::make_unique<UI::FileManagerView>(TR("title.file_manager")));
    sidebar_manager->registerSubView(
        TR("title.audio_manager"),
        std::make_unique<UI::AudioManagerView>(TR("title.audio_manager")));
    sidebar_manager->registerSubView(
        TR("title.beatmap_manager"),
        std::make_unique<UI::BeatMapManagerView>(TR("title.beatmap_manager")));
    sidebar_manager->registerSubView(
        TR("title.settings_manager"),
        std::make_unique<UI::SettingsView>(TR("title.settings_manager")));

    // 注册新建谱面向导
    m_uiManager.registerView("NewBeatmapWizard",
                             std::make_unique<UI::NewBeatmapWizard>());

    // 初始化时默认激活第一个 Tab（文件管理器）
    sidebar_manager->toggleSubView(TR("title.file_manager"));

    auto& engine = Logic::EditorEngine::instance();

    m_uiManager.registerView(
        "Basic2DCanvas",
        std::make_unique<Canvas::Basic2DCanvas>(
            "Basic2DCanvas", 200, 200, engine.getSyncBuffer("Basic2DCanvas")));

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
    // m_uiManager.registerView(
    //     "ImguiTestWindowUI",
    //     std::make_unique<Graphic::UI::ImguiTestWindowUI>("ImguiTestWindowUI"));
    // m_uiManager.registerView(
    //     "CLayoutTestUI",
    //     std::make_unique<Graphic::UI::CLayoutTestUI>("CLayoutTestUI"));
    // m_uiManager.registerView(
    //     "DebugWindowUI",
    //     std::make_unique<UI::DebugWindowUI>("DebugWindowUI"));
}

GameLoop::~GameLoop() {}

/**
 * @brief 启动游戏循环
 *
 * 初始化窗口、图形上下文，并进入主消息/渲染循环。
 * 该函数会阻塞直到窗口关闭。
 *
 * @param window 窗口上下文
 * @return int 退出代码 (0 表示正常退出)
 */
int GameLoop::start(Graphic::NativeWindow& window)
{
    // 初始化窗口
    // VKContext 表面资源后续初始化
    if ( g_vkContext ) {
        auto& context = g_vkContext->get();
        int   fbWidth, fbHeight;
        window.getFramebufferSize(fbWidth, fbHeight);
        context.initVKWindowRess(&window, fbWidth, fbHeight);

        // 初始化音频引擎
        Audio::AudioManager::instance().init();

        // 初始化原生对话框引擎
        NFD_Init();

        // 预加载音效文件
        auto& skinData = Config::SkinManager::instance().getData();
        for ( const auto& [key, path] : skinData.audioPaths ) {
            Audio::AudioManager::instance().preloadSoundEffect(key,
                                                               path.string());
        }

        // 启动独立逻辑线程 (必须在音频加载后启动，防止字典竞态)
        Logic::EditorEngine::instance().start();

        // [MVP架构测试] 在主线程创建 Model (BeatMap)，通过指令推送给 ViewModel
        // (ECS)
        // 测试载入谱面
        // auto map = std::make_shared<BeatMap>(
        //     BeatMap::loadFromFile("/home/xiang/Documents/MusicMapRepo/rm/"
        //                           "xiuluo/Redemptione/Redemptione_4k_hd.imd"));

        // auto map = std::make_shared<BeatMap>(BeatMap::loadFromFile(
        //     "/home/xiang/Documents/MusicMapRepo/osu/Designant - "
        //     "Designant/Designant - Designant. (Benson_) [Designant].osu"));

        // auto map = std::make_shared<BeatMap>(BeatMap::loadFromFile(
        //     "/home/xiang/Documents/MusicMapRepo/osu/493316 Camellia - I Can "
        //     "Fly In "
        //     "The Universe/Camellia - I Can Fly In The Universe (Evening) "
        //     "[Schizophrenia].osu"));

        // 发送给逻辑引擎
        // Logic::EditorEngine::instance().pushCommand(
        //     Logic::CmdLoadBeatmap{ map });

        // 进入主循环
        while ( !window.shouldClose() ) {
            // 3.1 让操作系统处理窗口事件 (缩放、关闭、鼠标按键等)
            // 已移至渲染循环内以降低 VSync 延迟 window.pollEvents();

            // 3.1.5 处理光标 BPM 同步逻辑
            float cursorSmokeLifeOverride = -1.0f;
            auto& settings = Config::AppConfig::instance().getEditorSettings();
            if ( settings.cursorStyle == Config::CursorStyle::Software &&
                 settings.softwareCursorConfig.enableBpmSyncSmokeLife ) {
                auto& engine = Logic::EditorEngine::instance();
                if ( auto session = engine.getActiveSession() ) {
                    auto& ctx = session->getContext();
                    if ( ctx.currentBeatmap ) {
                        double time = ctx.currentTime;
                        double bpm  = ctx.currentBeatmap->m_baseMapMetadata
                                          .preference_bpm;

                        // 查找当前时间点的 BPM
                        for ( const auto& t : ctx.currentBeatmap->m_timings ) {
                            if ( t.m_timingEffect == MMM::TimingEffect::BPM ) {
                                if ( t.m_timestamp <= time ) {
                                    bpm = t.m_bpm;
                                } else {
                                    break;
                                }
                            }
                        }

                        if ( bpm > 0 ) {
                            cursorSmokeLifeOverride =
                                static_cast<float>(60.0 / bpm);
                        }
                    }
                }
            }
            context.getRenderer().setCursorSmokeLifeOverride(
                cursorSmokeLifeOverride);

            // 3.2 执行渲染
            context.checkAndRebuildFonts();
            context.getRenderer().render(
                window,
                std::vector<Graphic::IGraphicUserHook*>{ &m_uiManager });
        }

        // 停止逻辑线程
        Logic::EditorEngine::instance().stop();

        // 保存配置
        Config::AppConfig::instance().save();

        // 关闭音频引擎
        Audio::AudioManager::instance().shutdown();

        // 关闭原生对话框引擎
        NFD_Quit();

        // 2. 主动清理 UI 管理器里存的所有视图
        // 这样 VKOffScreenRenderer 的析构就会在这里发生，
        // 此时 VKContext 还健在，m_device 也是有效的！
        (void)context.getLogicalDevice().waitIdle();
        m_uiManager.clearAllViews();
        return EXIT_NORMAL;
    } else {
        return EXIT_WINDOW_EXEPTION;
    }
}
}  // namespace MMM
