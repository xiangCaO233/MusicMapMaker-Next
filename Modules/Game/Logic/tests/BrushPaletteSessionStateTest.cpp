// 配色回归用例按值构造编辑配置，显式包含对应定义以固定测试输入。
#include "audio/AudioManager.h"
#include "common/EditorGestureState.h"
#include "common/EditorPlaybackState.h"
#include "common/EditorTrackLayout.h"
#include "common/LogicCommands.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "logic/BeatmapExportPresentation.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSettingsSnapshot.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/TimingTableSnapshot.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/ActionController.h"
#include "logic/session/EditorAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"

#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/VisualConfig.h"
#include "log/colorful-log.h"
#include "mmm/metadata/BaseMapMeta.h"
#include "mmm/metadata/MapMetadata.h"
#include "mmm/metadata/NoteMetadata.h"
#include "mmm/note/Note.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

// 本组使用进程内 EditorEngine 单例，验证编辑器级状态继承与会话级输入路由。
// 创建空谱面会话，不加载音频或谱面文件，也不创建真实画布窗口。
namespace
{

/// @brief 使用小容差比较颜色通道。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个通道足够接近时返回 true。
bool near(float lhs, float rhs)
{
    // 颜色和测试标记都是小范围有限值，绝对误差足以避免浮点表示差异。
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 比较可选颜色与预期颜色。
/// @param actual 实际颜色。
/// @param expected 预期颜色。
/// @return 实际颜色存在且全部通道匹配时返回 true。
bool colorMatches(const std::optional<glm::vec4>& actual,
                  const glm::vec4&                expected)
{
    // 未设置颜色不等于默认黑色，必须先确认 optional 有值再逐通道比较。
    // alpha 与 RGB 同样纳入比较，透明度丢失也应判定为恢复失败。
    return actual && near(actual->r, expected.r) &&
           near(actual->g, expected.g) && near(actual->b, expected.b) &&
           near(actual->a, expected.a);
}

/// @brief 验证新会话恢复关闭前的画笔调色盘。
/// @return 调色盘跨会话保留时返回 true。
/// @note 验证编辑器级预设继承，不验证关闭后写入磁盘或跨进程恢复。
/// @note 目标使用 Logo 占位会话，恢复预设不应依赖已加载的谱面模型。
bool testPaletteRestoredAfterSessionClose()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 单例跨用例存活，先移除既有会话，后续索引零才明确对应本例的源会话。
    while ( engine.getSessionCount() > 0 ) {
        // 从末尾关闭，避免移除元素后继续使用已经移动的后续索引。
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    engine.createSession(nullptr, "Palette Source", false);
    // 六个槽使用不同值，既能发现丢失，也能发现槽位映射顺序错误。
    const std::array<glm::vec4, MMM::Logic::NOTE_COLOR_SLOT_COUNT> colors{
        // 同一颜色的 RGB 也各不相同，可发现通道之间的错误交换。
        glm::vec4{ 0.11F, 0.12F, 0.13F, 1.0F },
        glm::vec4{ 0.21F, 0.22F, 0.23F, 1.0F },
        glm::vec4{ 0.31F, 0.32F, 0.33F, 1.0F },
        glm::vec4{ 0.41F, 0.42F, 0.43F, 1.0F },
        glm::vec4{ 0.51F, 0.52F, 0.53F, 1.0F },
        glm::vec4{ 0.61F, 0.62F, 0.63F, 1.0F },
    };
    // 通过正式命令入口写入，不直接修改目标 SessionContext 来绕开继承流程。
    engine.pushCommand(MMM::Logic::CmdSetBrushNotePalette{ colors });

    // 不额外推进源会话；命令中的编辑器级预设必须能为随后的新会话所用。
    engine.closeSession(0, false);
    // 源会话已经关闭，新会话不能通过保留源会话上下文来满足恢复断言。
    engine.createSession(nullptr, "Palette Target", true);

    auto session = engine.getActiveSession();
    if ( !session ) {
        XERROR("Palette target session was not created");
        return false;
    }
    session->update(0.0, engine.getEditorConfig(), true);

    // 零时间步只消费状态，不依赖播放时间推进或等待固定帧数。
    // 读取目标会话落地后的画笔状态，而非仅检查编辑器暂存的命令或配置。
    const auto& restored = session->getContext().brushState.customColors;
    const bool  matches  = colorMatches(restored.tap, colors[0]) &&
                         colorMatches(restored.head, colors[1]) &&
                         colorMatches(restored.hold, colors[2]) &&
                         colorMatches(restored.end, colors[3]) &&
                         colorMatches(restored.flickArrow, colors[4]) &&
                         colorMatches(restored.node, colors[5]) &&
                         !MMM::Logic::hasAnyNoteColorOverride(
                             session->getContext().brushState.drawCustomColors);
    // 颜色画笔可恢复六槽，并不意味着普通绘制应把这六个值写入 Note。
    // 显式标志必须由命令种类保存，无法从恰好相同的 RGBA 值反推。
    // 单槽命令才表达用户明确选定的新建物件颜色，并应跨会话保留该意图。
    constexpr glm::vec4 explicitTap{ 0.72F, 0.31F, 0.42F, 1.0F };
    engine.pushCommand(MMM::Logic::CmdSetBrushNoteColor{
        MMM::Logic::NoteColorSlot::Tap, explicitTap });
    engine.closeSession(0, false);
    engine.createSession(nullptr, "Explicit Palette Target", true);
    auto explicitSession = engine.getActiveSession();
    if ( !explicitSession ) return false;
    explicitSession->update(0.0, engine.getEditorConfig(), true);
    const auto& explicitBrush = explicitSession->getContext().brushState;
    // 第三会话从引擎缓存恢复，不依赖源会话上下文仍存在。
    // Head 必须保持未染色，防止恢复时将整套方案误当作对象覆写。
    const bool explicitRestored =
        colorMatches(explicitBrush.customColors.tap, explicitTap) &&
        colorMatches(explicitBrush.drawCustomColors.tap, explicitTap) &&
        !explicitBrush.drawCustomColors.head;
    // 在关闭前完成所有引用读取，后面只保留布尔结果用于报告。
    engine.closeSession(0, false);
    // 单例跨用例存活，恢复为方案态，后续场景不继承本例显式画笔颜色。
    engine.pushCommand(MMM::Logic::CmdSetBrushNotePalette{ colors });
    if ( !matches || !explicitRestored ) {
        XERROR("Brush palette or explicit draw color was not restored");
    }
    return matches && explicitRestored;
}

/// @brief 验证新会话恢复编辑器级项目音频画笔选择。
/// @return 稳定资源 ID 与资源类型均跨会话保留时返回 true。
/// @note 不验证 main-track 指向真实资源，只验证选择状态的传递。
/// @note 未覆盖跨项目资源 ID 的有效性判断，不能据此宣称跨项目资源可用。
bool testAudioResourceRestoredAfterSessionClose()
{
    auto& engine   = MMM::Logic::EditorEngine::instance();
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const bool originalProfessional = settings.professionalMode;
    const auto originalTool         = engine.getCurrentTool();
    // 专业能力开启时才允许跨会话恢复音频放置选择，普通模式必须保持空选择。
    settings.professionalMode = true;
    engine.setEditorConfig(
        MMM::Config::AppConfig::instance().getEditorConfig());
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    engine.createSession(nullptr, "Audio Source", false);
    // 同时选择高级工具，确认总开关约束的是全局工具缓存而非单个视图。
    engine.pushCommand(
        MMM::Logic::CmdChangeTool{ MMM::Logic::EditTool::ColorBrush });
    // 主音轨类型须与资源 ID 一同恢复，避免选择被解释成其他音轨类别。
    // 非默认音量可发现只恢复资源 ID、却丢失其附属参数的情况。
    engine.pushCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "main-track",
        .audioTrackType  = MMM::AudioTrackType::Main,
        .volume          = 0.65F,
    });
    engine.closeSession(0, false);
    engine.createSession(nullptr, "Audio Target", true);

    // 占位会话没有谱面采样可供反推，目标选择必须来自编辑器保留的画笔状态。
    // 新会话成为活动会话后执行一次更新，观察正常入口消费后的状态。
    auto session = engine.getActiveSession();
    if ( !session ) {
        XERROR("Audio target session was not created");
        settings.professionalMode = originalProfessional;
        engine.setEditorConfig(
            MMM::Config::AppConfig::instance().getEditorConfig());
        engine.pushCommand(MMM::Logic::CmdChangeTool{ originalTool });
        return false;
    }
    session->update(0.0, engine.getEditorConfig(), true);

    const auto& brush = session->getContext().brushState;
    // 三个字段构成同一次音频选择，不能仅凭资源字符串相同判定恢复成功。
    const bool matches =
        engine.getCurrentTool() == MMM::Logic::EditTool::ColorBrush &&
        session->getContext().currentTool == MMM::Logic::EditTool::ColorBrush &&
        brush.selectedAudioResourceId == "main-track" &&
        brush.selectedAudioTrackType == MMM::AudioTrackType::Main &&
        std::abs(brush.selectedAudioVolume - 0.65F) < 1e-6F;
    // 只读选择字段，不触发实际采样播放，测试不要求音频设备可用。
    engine.closeSession(0, false);
    if ( !matches ) {
        XERROR("New session did not restore the editor audio selection");
    }
    // 配置通过正式引擎入口广播，同时观察工作区外的跨画布缓存。
    // 新目标为空谱面，因此不可能从采样列表反推出刚才选择的资源。
    // 总开关重新打开并不构成新的资源选择动作，选择必须仍然为空。
    // 该往返覆盖源会话已销毁的情况，不能仅依赖源上下文的清理。
    // 测试不保存项目文件，所有身份传播均发生在内存命令队列中。
    // 空选择仍允许普通画笔工作，其基本放置另由画布测试负责覆盖。
    // 关闭总开关后同时模拟一个迟到的选择命令，防止引擎缓存复活该资源。
    // 再次开启并创建新画布，不能从已关闭的源会话或工作区恢复旧选择。
    // 普通模式的命令不会改变已保存的音频数据，只撤销下次放置的身份。
    settings.professionalMode = false;
    engine.setEditorConfig(
        MMM::Config::AppConfig::instance().getEditorConfig());
    // 隐藏按钮之外还需立即发布基础工具；迟到请求不能在重开专业模式后复活。
    // 此处源会话已关闭，检查覆盖的是引擎级缓存而非会话状态的偶然回退。
    // 接着恢复专业能力并建立新会话，验证它收到清洗后的工具和空采样选择。
    // 只恢复能力不重新请求工具，因此新会话仍须使用 Move。
    const bool toolCleared =
        engine.getCurrentTool() == MMM::Logic::EditTool::Move;
    engine.pushCommand(
        MMM::Logic::CmdChangeTool{ MMM::Logic::EditTool::ColorEraser });
    engine.pushCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "stale-track",
        .audioTrackType  = MMM::AudioTrackType::Effect });
    settings.professionalMode = true;
    engine.setEditorConfig(
        MMM::Config::AppConfig::instance().getEditorConfig());
    engine.createSession(nullptr, "Cleared Audio Target", true);
    auto clearedSession = engine.getActiveSession();
    if ( clearedSession )
        clearedSession->update(0.0, engine.getEditorConfig(), true);
    const bool cleared =
        clearedSession &&
        clearedSession->getContext()
            .brushState.selectedAudioResourceId.empty() &&
        clearedSession->getContext().currentTool == MMM::Logic::EditTool::Move;
    // 用例结束时先关闭新增画布再恢复软件配置，避免向后续用例广播残留选择。
    // 原模式可能为 true 或 false，两种状态均按进入用例之前的值复原。
    engine.closeSession(0, false);
    settings.professionalMode = originalProfessional;
    engine.setEditorConfig(
        MMM::Config::AppConfig::instance().getEditorConfig());
    // 恢复原工具，防止单例后续用例继承本次能力切换的活动状态。
    engine.pushCommand(MMM::Logic::CmdChangeTool{ originalTool });
    return matches && cleared && toolCleared;
}

/// @brief 验证主画布鼠标位置按 cameraId 路由到后台 Session。
/// @return 目标后台 Session 独立收到悬停坐标且活动 Session 未被污染时返回
/// true。
/// @note 手动推进两个会话，不依赖系统鼠标事件或操作系统窗口焦点。
/// @note 不验证滚轮焦点切换、点击拾取或屏幕上的悬浮绘制。
bool testBackgroundCanvasMousePositionRouting()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    // 使用两个独立会话，不能用同一会话的两个别名代替路由目标。
    const int32_t activeIndex =
        engine.createSession(nullptr, "Pointer Active", false);
    const int32_t backgroundIndex =
        engine.createSession(nullptr, "Pointer Background", false);
    // 从引擎登记项取得相机标识，避免自行拼接一个碰巧与实现匹配的 ID。
    const auto* activeEntry     = engine.getSessionEntry(activeIndex);
    const auto* backgroundEntry = engine.getSessionEntry(backgroundIndex);
    if ( !activeEntry || !activeEntry->session || !backgroundEntry ||
         !backgroundEntry->session ) {
        XERROR("Pointer routing sessions were not created");
        return false;
    }

    auto activeSession = activeEntry->session;
    // 持有会话直到验证结束，后面关闭登记项时不再读取已失效的 entry 指针。
    auto              backgroundSession  = backgroundEntry->session;
    const std::string backgroundCameraId = backgroundEntry->cameraId;
    // 复制相机 ID 的值，不把登记项内字符串的引用带入后续会话操作。
    engine.setActiveSessionIndex(activeIndex);
    // 显式选回第一会话，使后续命令目标确实处于后台，而不是刚创建的活动页。
    activeSession->update(0.0, engine.getEditorConfig(), true);
    backgroundSession->update(0.0, engine.getEditorConfig(), false);

    // 输入携带后台 cameraId；路由不能仅依据当前活动会话索引。
    // 坐标非零且 X、Y 不同，可发现未更新或两个坐标字段互换。
    engine.pushCommand(MMM::Logic::CmdSetMousePosition{
        .cameraId       = backgroundCameraId,
        .mouseX         = 321.0F,
        .mouseY         = 234.0F,
        .viewportWidth  = 800.0F,
        .viewportHeight = 600.0F,
        .isHovering     = true,
    });
    // 活动状态参数保持不变，后台会话必须在非活动更新中消费自己的鼠标事件。
    activeSession->update(0.0, engine.getEditorConfig(), true);
    backgroundSession->update(0.0, engine.getEditorConfig(), false);

    const auto& activeContext = activeSession->getContext();
    // 同时检查目标收到事件、非目标未被污染，比只检查后台坐标更严格。
    const auto& backgroundContext = backgroundSession->getContext();
    const bool  routed            = activeContext.mouseCameraId.empty() &&
                        // 空相机标识是初始状态，用作活动会话未接收输入的证据。
                        backgroundContext.mouseCameraId == backgroundCameraId &&
                        backgroundContext.isMouseInCanvas &&
                        near(backgroundContext.lastMousePos.x, 321.0F) &&
                        near(backgroundContext.lastMousePos.y, 234.0F);

    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }
    if ( !routed ) {
        // 先关闭本例会话再报告，避免失败路径留下会话影响单例后续状态。
        XERROR("Background canvas mouse position was routed to active session");
    }
    return routed;
}

/// @brief 验证布局拖拽式高频配置写入不会产生撕裂的容器和字符串快照。
/// @return 并发读取到的每份配置都保持内部字段一致时返回 true。
/// @note 仅核对实际观察到的快照，不保证读取到每一代配置或穷尽所有线程交错。
/// @note mutes 容器参与写入，但此测试的显式一致性断言只比较音量标记。
/// @note 不统计循环次数；写线程提前结束时，本轮可能没有观察到中间快照。
bool testConcurrentEditorConfigSnapshots()
{
    auto&      engine     = MMM::Logic::EditorEngine::instance();
    const auto baseConfig = engine.getEditorConfig();
    // 保留原配置供结束后恢复，不把测试键和标记值遗留给后续使用者。
    const std::string STRESS_KEY   = "layout-snapshot-stress";
    constexpr int     UPDATE_COUNT = 256;
    // 小整数可被 float 精确表示，标记不一致不会来自整数转浮点舍入。
    /// @brief 写线程完成全部配置发布后置位，主测试线程读取以结束观察循环。
    /// @warning 仅测试并发控制使用；release/acquire
    /// 传递完成状态，不代替引擎配置同步。
    std::atomic<bool> writerDone{ false };

    std::thread writer([&]() {
        // 只有一个写线程，测试发布与读取并发，不覆盖多写者仲裁。
        // 捕获的局部数据在 join 前保持存活；每轮从同一基线复制独立配置。
        for ( int i = 0; i < UPDATE_COUNT; ++i ) {
            auto        updatedConfig = baseConfig;
            auto&       sfxConfig     = updatedConfig.settings.sfxConfig;
            const float marker        = static_cast<float>(i);
            // 同一代标记同时写入标量和容器，读取到混合代数据时两者会不一致。
            sfxConfig.flickWidthVolumeMultiplier = marker;
            sfxConfig.permanentSfxVolumes.clear();
            // 清空再插入使测试涉及容器内容替换，而非只覆盖已有标量。
            sfxConfig.permanentSfxMutes.clear();
            sfxConfig.permanentSfxVolumes.emplace(STRESS_KEY, marker);
            sfxConfig.permanentSfxMutes.emplace(STRESS_KEY, (i % 2) == 0);
            // 布尔值交替变化用于制造容器更新，断言范围仍以音量字段为准。
            engine.setEditorConfig(updatedConfig);
            // 经由引擎发布接口提交完整值，不对读线程所见容器直接并发写入。
        }
        writerDone.store(true, std::memory_order_release);
        // 完成标志在最后一次发布之后置位，不要求读线程逐一确认每次发布。
    });

    bool consistent = true;
    // 首个不一致即可结束观察，但写线程仍须在下面正常收尾。
    // 此循环属于测试观察，不是产品热路径的等待策略；不插入定时睡眠干预交错。
    while ( !writerDone.load(std::memory_order_acquire) ) {
        const auto snapshot = engine.getEditorConfig();
        // 保存本次返回的配置值，再从这份值中借用字段，避免跨两次读取比较不同代。
        const auto& sfxConfig = snapshot.settings.sfxConfig;
        const auto  marker    = sfxConfig.permanentSfxVolumes.find(STRESS_KEY);
        if ( marker != sfxConfig.permanentSfxVolumes.end() &&
             // 写入尚未被观察到时允许没有测试键，不能把原始配置误判成撕裂快照。
             !near(marker->second, sfxConfig.flickWidthVolumeMultiplier) ) {
            consistent = false;
            break;
        }
    }

    // 即使检测到不一致提前退出，也必须等待写线程结束后才能恢复原配置。
    writer.join();
    engine.setEditorConfig(baseConfig);
    // 恢复发生在写线程退出后，避免恢复值又被最后一次写入覆盖。
    if ( !consistent ) {
        XERROR("Concurrent editor config snapshot contained torn SFX fields");
    }
    return consistent;
}

/// @brief 验证切换皮肤后的旧配色快照不能覆盖全局选择及其持久化值。
/// @return 引擎、全局配置和重新读取的配置都保留新选择时返回 true。
/// @note 默认配置路径由测试启动器隔离，禁止读取或保存个人设置。
bool testSkinSelectionSurvivesPaletteRefresh()
{
    auto& app    = MMM::Config::AppConfig::instance();
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 保存值副本，setEditorConfig 会同步改写全局对象，不能保留借用引用。
    const auto originalApp    = app.getEditorConfig();
    const auto originalEngine = engine.getEditorConfig();
    // 快照明确包含旧目录，不能用刚更新的全局配置绕过覆盖路径。
    auto stalePaletteConfig                           = originalEngine;
    stalePaletteConfig.settings.selectedSkinDirectory = "mmm-default";
    // 模拟设置页先更新软件皮肤，再用旧引擎快照发布拍线配色。
    // 模拟混音面板也已更新音量，旧配色快照不能把当前方案回滚。
    app.getEditorSettings().globalVolume          = 0.37F;
    app.getEditorSettings().bgmGain               = 0.63F;
    app.getEditorSettings().selectedSkinDirectory = "rm";
    // 配色变化必须继续生效，不能用拒绝整份旧快照来规避混音覆盖。
    // 同时验证全局配置与引擎快照，避免下次提交再次传播旧增益。
    stalePaletteConfig.visual.overrideBeatLineColors = true;
    engine.setEditorConfig(stalePaletteConfig);
    // 同时断言新配色仍生效，禁止通过丢弃整份配置来保住皮肤。
    // 两侧都必须一致，否则下一次引擎快照仍会继续传播旧值。
    bool ok = app.getEditorSettings().selectedSkinDirectory == "rm" &&
              engine.getEditorConfig().settings.selectedSkinDirectory == "rm" &&
              engine.getEditorConfig().visual.overrideBeatLineColors &&
              app.getEditorSettings().globalVolume == 0.37F &&
              engine.getEditorConfig().settings.bgmGain == 0.63F;
    // 完整走生产保存和读取路径，防止只修复当前窗口却仍保存旧选择。
    if ( ok ) {
        // 写入后清除内存选择，读取结果必须真正来自持久化文件。
        ok                                            = app.save();
        app.getEditorSettings().selectedSkinDirectory = "mmm-default";
        ok                                            = app.load() && ok &&
             app.getEditorSettings().selectedSkinDirectory == "rm";
    }
    // 测试结束恢复单例，后续会话及并发测试不继承本场景状态。
    // 即使断言失败也执行恢复，避免污染同进程内后续测试。
    app.getEditorConfig() = originalApp;
    engine.setEditorConfig(originalEngine);
    if ( !ok ) XERROR("Palette refresh overwrote persisted skin selection");
    return ok;
}

/// @brief 验证旧引擎快照不能覆盖本机观察参数或将其写入谱面模型。
/// @return 观察参数、二维复位及谱面只读边界均保持时返回 true。
/// @note 配置持久化使用测试启动器隔离的路径，不访问个人设置。
/// @details 本机参数模拟画布直接写入，候选配置模拟稍后的项目恢复。
/// 测试同时检查权威配置、引擎缓存和会话消费，避免只修复单个读取端。
/// 谱面断言检查保存入口使用的同步模型，不依赖窗口或 GPU 状态。
/// 本测试不创建或写出谱面文件，也不宣称覆盖其它谱面导出格式。
bool testObservationCameraSurvivesStaleConfig()
{
    auto& app    = MMM::Config::AppConfig::instance();
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 在创建夹具前保存两份独立配置；测试结束时按原权威来源恢复单例。
    const auto originalApp    = app.getEditorConfig();
    const auto originalEngine = engine.getEditorConfig();
    while ( engine.getSessionCount() > 0 ) {
        // 既有测试可能保留活动会话，先关闭才能让本例的索引零明确对应夹具。
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    // 使用真实谱面和物件，以便配置命令确实经过会话而非仅检查全局缓存。
    // 基础 BPM 和轨道数保持有效，避免无效谱面的规范化混入观察参数断言。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.preference_bpm = 120;
    // 来源扩展放入一个哨兵值，既检查内容保留，也检查没有附加摄像机键。
    beatmap->m_metadata.map_properties[MMM::MapMetadataType::MALODY]["keep"] =
        "source";
    MMM::Note note;
    // 时间和轨道使用非零值，能发现观察被错误实现为谱面几何变换。
    note.m_timestamp = 1000;
    note.m_track     = 2;
    // 原生物件元数据另作检查，防止参数只避开谱面元数据却写入单个物件。
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["annotation"] =
        "keep";
    beatmap->m_noteData.notes.push_back(note);
    // 必须保留值副本；随后同步会重建物件容器，引用会掩盖意外写入。
    const auto mapProperties  = beatmap->m_metadata.map_properties;
    const auto noteProperties = note.m_metadata.note_properties;
    engine.createSession(beatmap, "Observation Config", false);
    // 创建失败仍走统一恢复路径，不能让测试失败污染后续单例使用者。
    auto session = engine.getActiveSession();
    bool ok      = session != nullptr;
    if ( session ) {
        // 初始化布局之后建立未修改基线；随后只允许配置传播而不执行编辑动作。
        // 初始规范化是否产生状态与本修复无关，后面比较相同会话的前后状态。
        session->update(0.0, engine.getEditorConfig(), true);
        const bool wasDirty = session->getContext().actionStack.isDirty();
        // 两组分别覆盖非默认观察以及复位二维；旧快照总与当前三值不同。
        // 负 yaw 与非整数距离同时覆盖符号及倍率，不能只保留原型俯仰字段。
        // 第二组也防止实现使用非零判断，从而把二维默认误判为缺失值。
        const std::array<std::array<float, 3>, 2> cameras{
            std::array<float, 3>{ 65.0F, -135.0F, 2.5F },
            std::array<float, 3>{ 0.0F, 0.0F, 1.0F }
        };
        for ( const auto& camera : cameras ) {
            // 模拟右拖/滚轮仅先写 AppConfig，随后项目恢复提交旧引擎快照。
            // 候选值不取当前 AppConfig，保证修复前会真实覆盖三项观察参数。
            auto staleConfig = engine.getEditorConfig();
            staleConfig.visual.debugCanvasCameraAngleDegrees = 10.0F;
            staleConfig.visual.debugCanvasCameraYawDegrees   = 45.0F;
            staleConfig.visual.debugCanvasCameraDistance     = 3.0F;
            // 无关视觉开关属于候选值，后面用它确认配置合并仍正常执行。
            staleConfig.visual.overrideBeatLineColors =
                !originalEngine.visual.overrideBeatLineColors;
            auto& visual = app.getVisualConfig();
            // 写入权威来源后不能先发布一份新配置，否则旧缓存场景会被绕过。
            visual.debugCanvasCameraAngleDegrees = camera[0];
            visual.debugCanvasCameraYawDegrees   = camera[1];
            visual.debugCanvasCameraDistance     = camera[2];
            engine.setEditorConfig(staleConfig);
            // 引擎缓存必须同步保留当前值，否则下一个配置消费者仍会得到旧视角。
            const auto merged = engine.getEditorConfig();
            // 同时检查无关设置仍应用，禁止靠丢弃整个旧配置来保护摄像机。
            ok = ok &&
                 near(merged.visual.debugCanvasCameraAngleDegrees, camera[0]) &&
                 near(merged.visual.debugCanvasCameraYawDegrees, camera[1]) &&
                 near(merged.visual.debugCanvasCameraDistance, camera[2]) &&
                 merged.visual.overrideBeatLineColors ==
                     staleConfig.visual.overrideBeatLineColors;
            // 经正式保存和重新加载核对三值，确保修复覆盖下一次启动的配置。
            // 读取前人为破坏内存距离，使加载成功不能被已有正确内存值掩盖。
            ok                               = app.save() && ok;
            visual.debugCanvasCameraDistance = 0.5F;
            ok                               = app.load() && ok;
            // AppConfig 加载会替换内部值，此处重新取得引用后只读取最终结果。
            const auto& loaded = app.getVisualConfig();
            ok = ok && near(loaded.debugCanvasCameraAngleDegrees, camera[0]) &&
                 near(loaded.debugCanvasCameraYawDegrees, camera[1]) &&
                 near(loaded.debugCanvasCameraDistance, camera[2]);
            // 会话消费广播后同步保存模型，观察字段不能变成物件或元数据。
            // 零时间步只推进命令与派生状态，不引入播放位置变化。
            session->update(0.0, merged, true);
            MMM::Logic::SessionUtils::syncBeatmap(session->getContextMutable());
            // 撤销脏标记检查编辑副作用，元数据全表比较检查持久化字段泄漏。
            // 物件数量先判定，再用短路求值访问首项，失败时同样保持测试可诊断。
            ok = ok &&
                 session->getContext().actionStack.isDirty() == wasDirty &&
                 beatmap->m_metadata.map_properties == mapProperties &&
                 beatmap->m_noteData.notes.size() == 1 &&
                 beatmap->m_noteData.notes.front().m_timestamp ==
                     note.m_timestamp &&
                 beatmap->m_noteData.notes.front().m_track == note.m_track &&
                 beatmap->m_noteData.notes.front().m_metadata.note_properties ==
                     noteProperties;
        }
    }
    // 即使断言失败也关闭夹具并恢复配置，后续同进程测试不能继承观察状态。
    // 先恢复 AppConfig，再调用合并入口，保留规则才会消费原来的权威值。
    engine.closeSession(0, false);
    app.getEditorConfig() = originalApp;
    engine.setEditorConfig(originalEngine);
    if ( !ok ) XERROR("Stale config overwrote camera or mutated beatmap");
    return ok;
}

/// @brief 验证相机交互发布后逻辑快照立即取得完整姿态，稳定值不重复发布。
/// @return 连续换角度、切回二维与无关配置保留均符合约束时返回 true。
/// @note 使用正式修订读取入口，不通过完整配置广播掩盖缺失的相机发布。
/// @note 测试只检查跨线程快照发布契约，不依赖渲染线程恰好完成一帧。
/// @note 投影后的几何比例由 CameraLayoutEditorTest 另行覆盖。
bool testCameraConfigSnapshot()
{
    // 单例状态必须先按值保存，后续读取不持有引擎内部引用。
    auto&         engine   = MMM::Logic::EditorEngine::instance();
    const auto    original = engine.getEditorConfig();
    auto          snapshot = original;
    std::uint64_t revision = 0;
    // 先吸收前面用例的配置修订，后续才可判断本次相机变化是否被发布。
    (void)engine.refreshEditorConfigSnapshot(snapshot, revision);
    bool ok = true;
    // 角度先增大再反向，最后返回正视，防止实现只处理单调增厚。
    // 每一轮都使用相同强度，发布差异不能依赖补偿滑块再次变化。
    for ( const float angle : { 0.0F, 15.0F, 65.0F, -20.0F, 0.0F } ) {
        auto camera = original.visual;
        // 保持强度为满补偿，只改变视角；回到零角度同时切回非姿态模式。
        camera.canvasCameraPoseEnabled = angle != 0;
        camera.canvasCameraRotation = { angle, angle * 0.5F, -angle * 0.25F };
        camera.canvasCameraPosition = { angle * 0.01F, -0.8F, 1.2F };
        camera.canvasCameraLength   = 2.0F;
        camera.canvasMotionCompensation    = 35.0F;
        camera.canvasThicknessCompensation = 100.0F;
        // 旧观察模式参数也成组更新，防止切换模式后沿用旧方向或距离。
        camera.debugCanvasCameraAngleDegrees = angle;
        camera.debugCanvasCameraYawDegrees   = -angle;
        camera.debugCanvasCameraDistance     = 1.5F;
        // 候选值故意携带不同的无关设置，发布入口必须只复制相机字段。
        camera.overrideBeatLineColors = !original.visual.overrideBeatLineColors;
        engine.updateCanvasCameraConfig(camera);
        const bool changed =
            engine.refreshEditorConfigSnapshot(snapshot, revision);
        // 刷新成功与字段正确必须同时成立，不能读取残留的上轮姿态。
        const auto& actual = snapshot.visual;
        // 位置、旋转、长度及模式组成完整姿态；漏掉任一项均判为失败。
        // 旧模式三字段和新模式两数组一同检查，覆盖模式切换的边界。
        ok = changed && ok &&
             actual.canvasCameraPoseEnabled == camera.canvasCameraPoseEnabled &&
             actual.canvasCameraRotation == camera.canvasCameraRotation &&
             actual.canvasCameraPosition == camera.canvasCameraPosition &&
             near(actual.canvasCameraLength, camera.canvasCameraLength) &&
             near(actual.canvasMotionCompensation,
                  camera.canvasMotionCompensation) &&
             near(actual.canvasThicknessCompensation,
                  camera.canvasThicknessCompensation) &&
             near(actual.debugCanvasCameraAngleDegrees, angle) &&
             near(actual.debugCanvasCameraYawDegrees, -angle) &&
             near(actual.debugCanvasCameraDistance, 1.5F) &&
             actual.overrideBeatLineColors ==
                 original.visual.overrideBeatLineColors;
        // 相同姿态重复输入不应增加修订，也不能制造一次完整配置刷新。
        // 此检查直接覆盖稳定帧，避免靠 UI 恰好未调用才躲过重复发布。
        engine.updateCanvasCameraConfig(camera);
        ok = !engine.refreshEditorConfigSnapshot(snapshot, revision) && ok;
    }
    // 失败也恢复原始相机；不用完整配置入口写回或持久化无关全局设置。
    engine.updateCanvasCameraConfig(original.visual);
    // 错误只在恢复完成后报告，保证同进程其它用例不会继承失败夹具。
    if ( !ok ) XERROR("Canvas camera snapshot did not follow the current pose");
    return ok;
}

/// @brief 验证全谱清理会清除新旧颜色字段、折线子段，并可撤销。
/// @return 正式模型和运行时视图一致且无关数据保留时返回 true。
/// @details
/// 直接使用正式命令入口检查批量动作与撤销历史，而不只调用颜色工具函数。
/// 首个根物件混合新键、旧别名、无效旧值以及其它 MMM 字段。
/// 无效旧值无法进入颜色缓存，但仍须从最终保存的 metadata 中移除。
/// Malody 来源的同名字段属于外部格式，不能随 MMM 颜色一并删除。
/// 折线父物件保存子段数组，子实体负责画布交互，两处都必须清空。
/// 草稿物件由项目草稿存储管理，不属于当前谱面正式物件清理范围。
/// 模型同步后的容器是文件保存的输入，因此也检查实际同步结果。
/// 最后撤销这一次全谱命令，验证缓存、历史别名和子实体同时恢复。
/// 本测试不依赖皮肤当前 RGB 值；皮肤颜色的具体值可随主题变化。
/// 清除的判据是 optional 颜色为空和来源字段消失，而不是变成白色。
/// 这能区分真正恢复继承语义与写入一个恰巧等于当前皮肤的固定色。
/// 命令只作用于当前正式谱面，不会更改后续新建物件使用的画笔调色盘。
/// 文件保存入口读取同步后的模型，因此不需要为夹具创建真实项目目录。
/// @warning 测试只执行一次用户命令及模型同步，不模拟逐帧批量清理。
bool testClearAllNoteColors()
{
    using namespace MMM::Logic;
    constexpr auto mmm = MMM::NoteMetadataType::MMM;
    SessionContext ctx;
    // 测试模型仅驻留内存，避免保存路径触及用户配置或真实项目。
    ctx.currentBeatmap = std::make_shared<MMM::BeatMap>();
    ActionController actions(ctx);
    // ActionController 将单次命令记录进会话的 ActionStack，后续 Undo
    // 使用同一栈。

    // 普通物件同时放入有效新键和无效旧别名，确保清理按键名而非解析结果执行。
    // 另一个非颜色 MMM 属性用于发现误删整个来源域的实现。
    NoteComponent tap;
    tap.m_customColors.tap = glm::vec4{ 0.2F, 0.3F, 0.4F, 1.0F };
    tap.m_metadata.note_properties[mmm]["note_tap"]   = "0.2,0.3,0.4,1";
    tap.m_metadata.note_properties[mmm]["color.note"] = "invalid";
    tap.m_metadata.note_properties[mmm]["annotation"] = "keep";
    // 保留同一来源域的非颜色键，能发现“删掉整个 MMM 元数据域”的误实现。
    tap.m_metadata
        .note_properties[MMM::NoteMetadataType::MALODY]["color.note"] =
        "foreign";
    const auto tapEntity = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<NoteComponent>(tapEntity, tap);

    // 折线内嵌段是持久化真源，派生子实体也须同步清空以立即更新画布。
    // 父级旧别名与子段新旧键混合，覆盖曾经编辑过的旧版谱面。
    NoteComponent polyline;
    polyline.m_type              = MMM::NoteType::POLYLINE;
    polyline.m_customColors.head = glm::vec4{ 0.5F };
    polyline.m_metadata.note_properties[mmm]["color.hold_flick_head"] =
        "0.5,0.5,0.5,0.5";
    NoteComponent::SubNote sub;
    sub.type                                             = MMM::NoteType::HOLD;
    sub.duration                                         = 1.0;
    sub.customColors.hold                                = glm::vec4{ 0.6F };
    sub.metadata.note_properties[mmm]["note_hold"]       = "0.6,0.6,0.6,0.6";
    sub.metadata.note_properties[mmm]["color.hold_body"] = "invalid";
    polyline.m_subNotes.push_back(sub);
    // 父实体与子投影共享这一子段的逻辑位置，但拥有独立的 ECS 组件值。
    const auto parentEntity = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<NoteComponent>(parentEntity, polyline);
    auto       child = makeNoteComponentFromSubNote(sub, true, parentEntity, 0);
    const auto childEntity = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<NoteComponent>(childEntity, child);
    // 创建投影后不再改写父数组，避免测试夹具先天不一致。

    // 草稿颜色保留；它存于项目草稿区，不属于当前谱面的正式 Note 容器。
    // 使用普通物件副本生成草稿，保证草稿确实包含待清理颜色。
    NoteComponent draft    = tap;
    draft.m_isDraft        = true;
    const auto draftEntity = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<NoteComponent>(draftEntity, draft);

    // 先确认内存中的所有正式投影，再调用文件保存前的模型同步。
    actions.handleCommand(CmdClearAllNoteColorOverrides{});
    const auto& clearedTap = ctx.noteRegistry.get<NoteComponent>(tapEntity);
    const auto& clearedParent =
        ctx.noteRegistry.get<NoteComponent>(parentEntity);
    const auto& clearedChild = ctx.noteRegistry.get<NoteComponent>(childEntity);
    const auto& retainedDraft =
        ctx.noteRegistry.get<NoteComponent>(draftEntity);
    // 即时视图覆盖四种角色：独立根、折线父、折线投影和草稿根。
    // 颜色缓存缺失代表渲染回退皮肤；元数据缺失代表重启后仍回退皮肤。
    // 两个状态都需要验证，否则清理可能只在当前编辑会话内有效。
    bool ok =
        !hasAnyNoteColorOverride(clearedTap.m_customColors) &&
        !hasAnyNoteColorOverride(clearedParent.m_customColors) &&
        !hasAnyNoteColorOverride(clearedParent.m_subNotes[0].customColors) &&
        !hasAnyNoteColorOverride(clearedChild.m_customColors) &&
        clearedTap.m_metadata.note_properties.at(mmm).size() == 1 &&
        clearedTap.m_metadata.note_properties.at(mmm).contains("annotation") &&
        clearedTap.m_metadata.note_properties.at(MMM::NoteMetadataType::MALODY)
            .contains("color.note") &&
        clearedParent.m_subNotes[0].metadata.note_properties.find(mmm) ==
            clearedParent.m_subNotes[0].metadata.note_properties.end() &&
        hasAnyNoteColorOverride(retainedDraft.m_customColors);

    // 模型同步是保存入口的数据源，文件写出时不得重新带回已清除的颜色。
    // Hold 子段来自父内嵌数组，不能错误地以派生实体作为持久化真源。
    SessionUtils::syncBeatmap(ctx);
    // 同步会重建具体类型容器，子 Hold 也应该出现在正式 Hold 容器里。
    ok = ok && ctx.currentBeatmap->m_noteData.notes.size() == 1 &&
         ctx.currentBeatmap->m_noteData.polylines.size() == 1 &&
         ctx.currentBeatmap->m_noteData.holds.size() == 1 &&
         ctx.currentBeatmap->m_noteData.notes.front()
                 .m_metadata.note_properties.at(mmm)
                 .size() == 1 &&
         ctx.currentBeatmap->m_noteData.holds.front()
                 .m_metadata.note_properties.find(mmm) ==
             ctx.currentBeatmap->m_noteData.holds.front()
                 .m_metadata.note_properties.end();

    // Undo 应恢复缓存和旧字段，确保随后再保存能还原原始谱面属性。
    // 投影子实体若没有恢复，画布颜色会与父折线持久化状态分离。
    // 这里通过统一命令入口撤销，覆盖 ActionStack 而非直接复写旧副本。
    actions.handleCommand(CmdUndo{});
    ok = ok &&
         hasAnyNoteColorOverride(
             ctx.noteRegistry.get<NoteComponent>(tapEntity).m_customColors) &&
         ctx.noteRegistry.get<NoteComponent>(tapEntity)
             .m_metadata.note_properties.at(mmm)
             .contains("color.note") &&
         hasAnyNoteColorOverride(
             ctx.noteRegistry.get<NoteComponent>(parentEntity)
                 .m_subNotes[0]
                 .customColors) &&
         hasAnyNoteColorOverride(
             ctx.noteRegistry.get<NoteComponent>(childEntity).m_customColors);
    if ( !ok )
        XERROR("Clear all note colors did not preserve persistence or undo");
    return ok;
}

/// @brief 在会话长锁被占用时提交多次焦点，随后只应用最后一个相机身份。
/// @details 请求线程始终不触碰可变 SessionContext。测试线程先持有真实
/// 注册表互斥锁，逻辑线程稍后启动，分别验证提交和消费两个边界。
/// @return 请求在注册表不可用时仍完成，并且最后一个有效目标被激活。
bool testLatestCanvasFocusRequest()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 根据引擎返回的索引取得相机身份；测试不用窗口标题作为跨线程地址。
    // 三个真实登记项区分初始活动会话、被覆盖的目标和最终应激活的目标。
    const auto first  = engine.createSession(nullptr, "Focus first", false);
    const auto second = engine.createSession(nullptr, "Focus second", false);
    const auto last   = engine.createSession(nullptr, "Focus last", false);
    const auto secondCamera = engine.getSessionEntry(second)->cameraId;
    const auto lastCamera   = engine.getSessionEntry(last)->cameraId;
    engine.setActiveSessionIndex(first);

    // 用另一执行线程模拟 UI。旧的同步入口无法越过本线程持有的注册表锁。
    // 模拟跨整轮 Session::update 的长锁。不能用两个互不相关的 mutex
    // 推断焦点提交速度，也不能在持锁线程本身尝试递归获取来假装并发。
    std::unique_lock holdSessionLock(engine.getSessionMutex());
    auto             request = std::async(std::launch::async, [&] {
        engine.requestActiveCanvas(secondCamera);
        engine.requestActiveCanvas(lastCamera);
    });
    const bool       returnedWithoutRegistryLock =
        request.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    // 即使超时也先解除阻塞再收回异步调用，有问题时应报告失败而非死锁。
    // get 返回后两个请求都已发布，启动工作者即可确定地验证合并。
    holdSessionLock.unlock();
    request.get();

    // 使用生产逻辑循环消费邮箱；无需依赖真实 GPU、窗口焦点或音频设备。
    // 此处只验证焦点提交，PCM 准备与 transport 提交由独立音频集成用例覆盖。
    engine.start();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // 固定上限只用于防止失败测试挂起，生产请求由逻辑循环消费。
    while ( engine.getActiveSessionIndex() != last &&
            std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::yield();
    }
    // 只观察原子发布的活动身份，不跨线程读取可变上下文来证明切换完成。
    // 到期后仍要完整执行 stop，避免本场景留下并发任务进入下一场景。
    const bool latestApplied = engine.getActiveSessionIndex() == last;
    engine.stop();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }
    // 清理后统一检查进展条件和最终身份。单纯能排队仍不足以验证逻辑提交。
    // 保留独立布尔值可区分提交被阻塞和邮箱覆盖顺序错误。
    if ( !returnedWithoutRegistryLock || !latestApplied ) {
        XERROR(
            "Canvas focus must submit without the registry lock and apply the "
            "latest request");
        return false;
    }
    return true;
}

/// @brief 验证活动手势读取独立于会话长锁，关闭及切换时不会继承错误状态。
/// @details 在逻辑线程停止时设置夹具手势，使用正式活动会话切换路径发布状态。
/// 另一条线程模拟快捷键侧，在注册表锁不可用时也必须完成读取。
/// @return 发布内容、快速读取和活动身份切换都正确时返回 true。
/// 直接设置夹具输入使锁竞争顺序可重复，真实快捷键的按键语义由 UI 套件验证。
/// 该用例不构造渲染快照，避免一帧延迟或缓存更新偶然掩盖会话锁依赖。
bool testPublishedActiveGestureState()
{
    using namespace MMM;
    auto&      engine       = Logic::EditorEngine::instance();
    const auto originalTool = engine.getCurrentTool();
    // 正式会话切换恢复全局工具，因此夹具通过同一命令设置该值。
    engine.pushCommand(Logic::CmdChangeTool{ Logic::EditTool::Draw });
    const auto first  = engine.createSession(nullptr, "Gesture source", false);
    const auto second = engine.createSession(nullptr, "Gesture target", false);
    auto&      firstContext =
        engine.getSessionEntry(first)->session->getContextMutable();
    // 无真实音频或 GPU，本用例仅模拟已发生的画布手势。
    // 活动画布必须带着一个非零状态完成跨线程读取；只检查空值会漏掉恒零错误。
    firstContext.currentTool         = Logic::EditTool::Draw;
    firstContext.brushState.isActive = true;
    engine.setActiveSessionIndex(first);

    // 异步读取必须在锁释放以前完成；故障路径也释放锁再回收任务。
    std::unique_lock sessionLock(engine.getSessionMutex());
    auto             reader = std::async(std::launch::async,
                             [&] { return engine.activeGestureState(); });
    const bool       ready =
        reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    sessionLock.unlock();
    // 结果使用普通值，测试不在两个线程间分享 SessionContext 中的引用。
    const auto brush = reader.get();

    auto& secondContext =
        engine.getSessionEntry(second)->session->getContextMutable();
    // 第二个目标的旧工具与全局配置故意不同，确保发布在切换完成之后。
    engine.pushCommand(Logic::CmdChangeTool{ Logic::EditTool::Marquee });
    secondContext.currentTool = Logic::EditTool::Draw;
    secondContext.isSelecting = true;
    secondContext.isDragging  = true;
    // 没有实体的拖动不成立。一次读应同时保留选区并排除上一画笔。
    engine.setActiveSessionIndex(second);
    const auto marquee = engine.activeGestureState();
    // 新建真实实体后再次发布同一画布，单独区分有命中与空白拖动。
    // 在独占测试线程分配实体，跳过真正拖拽所需的输入设备。
    secondContext.draggedEntity = secondContext.noteRegistry.create();
    engine.setActiveSessionIndex(second);
    // 两个 Marquee 位可以同代出现，不能用互斥枚举覆盖其中一个。
    const auto drag = engine.activeGestureState();
    // 删除活动槽时可能回到第一个会话，必须依照真实目标重新取样。
    engine.closeSession(second, false);
    // 回退到原画布时，该会话的手势恢复；只清零也不能通过该断言。
    const auto restored = engine.activeGestureState();
    engine.closeSession(first, false);
    // 最后一个槽关闭之后，发布值必须完全脱离此前的注册表条目。
    const auto empty = engine.activeGestureState();
    engine.pushCommand(Logic::CmdChangeTool{ originalTool });
    // 每组分别验证读取是否进展、工具切换是否及时及销毁时的失效规则。
    // 对只剩最后一个会话的清理也有断言，防止后续用例误读已销毁状态。
    // 发布位在渲染开始前已经可读，因此本回归无需推进 ImGui 或渲染快照。
    // 取样值跨越会话关闭保持独立，断言不借用已经释放的谱面对象。
    const bool lockBypassed = ready && brush.m_drawingBrush &&
                              !brush.m_selectingMarquee &&
                              !brush.m_draggingNote;
    const bool switchPublished =
        marquee.m_selectingMarquee && !marquee.m_draggingNote &&
        !marquee.m_drawingBrush && drag.m_selectingMarquee &&
        drag.m_draggingNote && !drag.m_drawingBrush;
    const bool closePublished =
        restored.m_drawingBrush && !empty.m_drawingBrush &&
        !empty.m_selectingMarquee && !empty.m_draggingNote;
    // 逐个报告阶段，避免多重断言合并后找不到发布边界。
    if ( !lockBypassed || !switchPublished || !closePublished ) {
        XERROR("gesture state regression: lock={}, switch={}, close={}",
               lockBypassed,
               switchPublished,
               closePublished);
    }
    return lockBypassed && switchPublished && closePublished;
}

/// @brief 验证工具栏布局及键数命令经实际引擎路由，替换会话时不混用旧布局。
/// @details 两份内存谱面都通过正式加载命令创建，无文件写入与音频设备依赖。
/// 独立读取和命令入队在长锁持有期间完成，再由生产循环消费两格滚轮。
/// @return 布局、变更顺序、尾随保存及会话回退同时正确时返回 true。
bool testToolbarTrackLayoutPublication()
{
    using namespace MMM;
    auto& engine = Logic::EditorEngine::instance();
    // 两份布局的三个区域都取不同值，便于发现只刷新玩家键数的半代发布。
    auto firstMap                               = std::make_shared<BeatMap>();
    firstMap->m_baseMapMetadata.track_count     = 5;
    firstMap->m_baseMapMetadata.bgm_track_count = 2;
    firstMap->m_baseMapMetadata.name            = "before queued edit";
    // 采样放在 BGM 第二轨，初始绝对位置为 5 + 1。
    // 虚拟资源没有 PCM；它足以覆盖 ECS 迁移以及协作采样权限的分类。
    firstMap->m_audioSamples.push_back({ .m_timestamp       = 500.0,
                                         .m_track           = 6,
                                         .m_audioResourceId = "virtual-bgm" });
    const auto first = engine.createSession(firstMap, "Toolbar source", false);
    const auto firstSession = engine.getActiveSession();
    // 仅用直接 update 准备两个固定谱面，其后的并发路由通过正式引擎循环验证。
    firstSession->update(0, engine.getEditorConfig(), true);
    // 夹具只设置草稿区宽度；玩家和 BGM 的布局必须来自正式模型加载。
    // 逻辑线程尚未启动，直接设置测试模型不会造成跨线程读写。
    firstSession->getContextMutable().draftTrackCount = 11;
    engine.setActiveSessionIndex(first);
    const auto firstGeneration = engine.activePlaybackState().generation;

    // 排队中的模型修改不能被旧 UI 副本覆盖。先把改标题载荷准备好，
    // 交给同一个生产者顺序入队，模拟元数据窗口与工具栏共享 UI 线程。
    // 入队时尚未消费标题，要求单字段命令在消费时读取，而非在提交线程复制元数据。
    // 不把不同生产者在并发队列中的相对顺序当作产品保证。
    auto renamed = firstMap->m_baseMapMetadata;
    renamed.name = "latest title";

    // 同一线程保有会话锁，另一个线程完成一次实际布局读取及两次正常输入路由。
    // 超时时先解除锁再回收 future，因此回归会报告失败，不会让整个套件挂起。
    std::unique_lock holdSessionLock(engine.getSessionMutex());
    // 先持锁再创建线程，确保读和两次输入都确实发生在受测的临界区间。
    auto reader = std::async(std::launch::async, [&] {
        const auto layout = engine.activeTrackLayout();
        // 设置步进器与工具栏读同一发布槽，两者都必须绕过这里持有的会话锁。
        const auto settings = Logic::activeBeatmapSettingsState();
        // 夹具已经持有会话句柄，直接走它的正常队列提交前置元数据；
        // 两个工具栏事件仍从公开引擎入口路由，用同一线程保留先后顺序。
        firstSession->pushCommand(
            Logic::CmdUpdateBeatmapMetadata{ .baseMeta = renamed });
        for ( int i = 0; i < 2; ++i ) {
            engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{
                .trackCountEdit = Logic::ToolbarTrackCountEdit{ 1, true } });
        }
        return std::pair{ layout, settings };
    });
    // 等待 future 而不是采样会话锁位；若读或入队任何一处发生等待，都会超时。
    const bool returned =
        reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    holdSessionLock.unlock();
    // future 完成说明并发调用已结束，以下布局副本无需再与读线程同步。
    const auto [readWhileLocked, settingsWhileLocked] = reader.get();
    // 用非零的积累状态作阳性对照，恒返回空布局不能通过。
    const auto expectedFirst = Common::EditorTrackLayout{ true, 5, 11, 2 };

    // UI 只等最终发布值，运行期间不借用会话模型。截止时间只约束测试故障路径。
    engine.start();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ( engine.activeTrackLayout().playerTracks != 7 &&
            std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();
    // 停止后才能检查模型及采样 Registry，否则测试本身会制造跨线程读风险。
    engine.stop();
    // 同步模型在逻辑线程停止后读取，两次滚轮应从 5 逐步累加至 7。
    // 草稿和 BGM 在本次操作中独立于玩家区，必须保留其原有值。
    const auto applied = engine.activeTrackLayout();
    // 此时样本应位于 7 + 1；验证保留的是 BGM 区内索引而非旧绝对编号。
    // 仅检查布局和计数会漏掉声音仍停在原轨的错误。
    const auto samples =
        firstSession->getContext()
            .sampleRegistry.view<const Logic::SampleComponent>();
    const bool sampleMigrated =
        samples.size() == 1 &&
        samples.get<const Logic::SampleComponent>(*samples.begin()).m_track ==
            8;
    // 保存请求要与两次布局修改同时存在，确认字段级路由也走元数据的尾随保存。
    // 不给内存谱面配置落盘目的地，因此在这里核对请求而不触发实际保存。
    const bool editsApplied =
        applied == Common::EditorTrackLayout{ true, 7, 11, 2 } &&
        firstMap->m_baseMapMetadata.track_count == 7 &&
        firstMap->m_baseMapMetadata.name == "latest title" &&
        firstSession->hasPendingMetadataAutoSave() && sampleMigrated;

    // 建立新会话后的短暂空窗不能继续显示旧谱面。等待载入后才发布新组合。
    auto secondMap                               = std::make_shared<BeatMap>();
    secondMap->m_baseMapMetadata.track_count     = 8;
    secondMap->m_baseMapMetadata.bgm_track_count = 3;
    const auto second =
        engine.createSession(secondMap, "Toolbar target", false);
    // 新会话默认字段尚未绑定谱面，不应提前按默认四键显示。
    const auto pending       = engine.activeTrackLayout();
    const auto secondSession = engine.getActiveSession();
    secondSession->update(0, engine.getEditorConfig(), true);
    secondSession->getContextMutable().draftTrackCount = 13;
    engine.setActiveSessionIndex(second);
    // 不打开 ImGui 或请求渲染快照。工具栏布局在逻辑侧切换完成时即可消费。
    const auto switched = engine.activeTrackLayout();

    // 正式关闭路径同时覆盖恢复剩余会话以及撤回最后一份布局。
    engine.closeSession(second, false);
    // 关闭后必须保留剩余谱面的草稿区与 BGM 区，不能靠全部复位通过。
    const auto restored = engine.activeTrackLayout();
    // 先关闭第二个索引，使回退发生在正常清理路径；最后关闭才应得到空布局。
    engine.closeSession(first, false);
    // 数值份额与会话都已释放，独占最后一份布局的测试读者仍可持值做比较。
    const auto empty = engine.activeTrackLayout();

    // 分阶段报告，以便区分长锁回归、相对步长丢失与索引重排后的失效处理。
    // 失败阶段各自独立计算，避免单个失败吞掉其他场景的结果。
    // 比较设置页所见代次及两组轨道数，不能只让半组状态可读就判为通过。
    const bool usableWhileLocked =
        returned && readWhileLocked == expectedFirst && settingsWhileLocked &&
        settingsWhileLocked->generation == firstGeneration &&
        settingsWhileLocked->draftTracks == 11 &&
        settingsWhileLocked->bgmTracks == 2;
    // 比较整组而不是单个计数，三个来自不同会话的值不能在同一轮混合。
    const bool switchedCleanly =
        pending == Common::EditorTrackLayout{} &&
        switched == Common::EditorTrackLayout{ true, 8, 13, 3 } &&
        restored == applied && empty == Common::EditorTrackLayout{};
    if ( !usableWhileLocked || !editsApplied || !switchedCleanly )
        XERROR("Toolbar track layout regression: lock={}, edits={}, switch={}",
               usableWhileLocked,
               editsApplied,
               switchedCleanly);
    return usableWhileLocked && editsApplied && switchedCleanly;
}

/// @brief 核对播放展示值与命令路由，以及同索引替换后的归属。
/// @return 长锁期间仍能读取与投递，旧代次不能改变新会话。
/// @note 作用域内逐个关闭会话，避免把活动状态留给随后使用引擎单例的测试。
/// @note 使用正式的逻辑事件订阅路由，覆盖 UI 载荷中的代次跨层传递。
/// @note 不启动逻辑线程，分别观察提交是否完成以及会话消费后的状态。
bool testActivePlaybackStatePublication()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 前面场景已逐个关闭会话。本夹具使用两张内存谱面复现零号索引被复用，
    // 不依赖真实音频设备或工作区写盘。
    auto firstMap                            = std::make_shared<MMM::BeatMap>();
    auto secondMap                           = std::make_shared<MMM::BeatMap>();
    firstMap->m_baseMapMetadata.track_count  = 5;
    secondMap->m_baseMapMetadata.track_count = 8;
    // 两张谱面的键数不同，错误返回某个固定默认值不能通过读模型检查。
    const auto first = engine.createSession(firstMap, "Mobile source", false);
    const auto firstSession = engine.getActiveSession();
    firstSession->update(0, engine.getEditorConfig(), true);
    engine.setActiveSessionIndex(first);

    // 把首次载入后的快照保留下来，后面用它区分创建和切回原谱面的两次发布。
    const auto original = engine.activePlaybackState();
    const auto second = engine.createSession(secondMap, "Mobile target", false);
    // 新活动谱面的加载命令还未消费，按钮应同时撤回旧轨数和旧播放位。
    const auto loading       = engine.activePlaybackState();
    const auto secondSession = engine.getActiveSession();
    // 先让邻接谱面载入，关闭活动首项后应立即显示邻接谱面的真实键数。
    secondSession->update(0, engine.getEditorConfig(), true);
    engine.setActiveSessionIndex(first);
    // 移动弹层和逻辑事件都以这个代次为基准，之前的初始快照只用于比较载入过程。
    // 此刻引擎已经切回首项，所见状态必须属于首项，即使第二张谱面已经可以播放。
    const auto openedPopup = engine.activePlaybackState();

    auto&        audio         = MMM::Audio::AudioManager::instance();
    const double originalSpeed = audio.getPlaybackSpeed();
    const double originalPitch = audio.getPlaybackPitch();
    // 使用与当前播放方案不同的值，验证实际消费效果而非队列是否非空。
    const double testSpeed = originalSpeed == .75 ? .5 : .75;
    const double testPitch = originalPitch == 5.0 ? 3.0 : 5.0;
    auto&        events    = MMM::Event::EventBus::instance();
    // 同一线程持有会话锁，另一个线程模拟 UI。这里不启动逻辑循环，
    // 因此读状态和四类命令都必须在解锁前结束，实际命令由后面的 update 消费。
    std::unique_lock holdSessionLock(engine.getSessionMutex());
    auto             reader = std::async(std::launch::async, [&] {
        // 带代次的播放事件覆盖移动入口，直接定位与音高覆盖桌面入口。
        // 先暂停再进入拖动，避免上一场景的实际播放进度影响本轮检查。
        events.publish(MMM::Event::LogicCommandEvent(
            MMM::Logic::CmdSetPlayState{ false }, openedPopup.generation));
        engine.pushCommand(
            MMM::Logic::CmdSeek{ .time = 0.0, .isScrubbing = true });
        events.publish(MMM::Event::LogicCommandEvent(
            MMM::Logic::CmdSetPlaybackSpeed{ testSpeed }));
        engine.pushCommand(MMM::Logic::CmdSetPlaybackPitch{ testPitch });
        return engine.activePlaybackState();
    });
    // 完成条件在持锁期间观察；解锁后读取结果不能掩盖错误的长锁等待。
    const bool returned =
        reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    // 超时也先解锁，故障路径可以回收异步任务，不会留下永久等待。
    holdSessionLock.unlock();
    const auto whileLocked = reader.get();
    firstSession->update(0, engine.getEditorConfig(), true);
    // 检查常规状态机确实消费了定位，并且倍速和变调已到达全局音频控制器。
    // 即使暂停中的谱面没有启用音频后端，仍应保留拖动阶段和试听参数。
    const bool playbackApplied = firstSession->getContext().isSeekScrubbing &&
                                 audio.getPlaybackSpeed() == testSpeed &&
                                 audio.getPlaybackPitch() == testPitch;

    // 删除活动首项后，相邻谱面成为零号项。如果继续比较索引就会误投旧命令。
    engine.closeSession(first, false);
    const auto replacement = engine.activePlaybackState();
    const bool reusedIndex = engine.getActiveSessionIndex() == first &&
                             engine.getActiveSession() == secondSession &&
                             second != first;
    // 分别使用旧代次发送播放控制和写谱命令，覆盖两条路由各自的校验。
    events.publish(MMM::Event::LogicCommandEvent(
        MMM::Logic::CmdSetPlaybackSpeed{ originalSpeed },
        openedPopup.generation));
    events.publish(MMM::Event::LogicCommandEvent(
        MMM::Logic::CmdUpdateTrackCount{ 3 }, openedPopup.generation));
    // 旧代次只应拒绝本次触屏命令，不能影响新会话队列及原有可撤销迁移。
    secondSession->update(0, engine.getEditorConfig(), true);
    // 速度读取全局试听参数，轨数读取替换会话。两个作用域应同时保持原值。
    const bool ignoredOld = secondSession->getContext().trackCount == 8 &&
                            audio.getPlaybackSpeed() == testSpeed;
    // 用同一个替换会话验证两条路由都接受当前代次。
    // 恢复原始候选倍速与上一条过期请求相同，只有代次发生变化。
    events.publish(MMM::Event::LogicCommandEvent(
        MMM::Logic::CmdSetPlaybackSpeed{ originalSpeed },
        replacement.generation));
    events.publish(MMM::Event::LogicCommandEvent(
        MMM::Logic::CmdUpdateTrackCount{ 7 }, replacement.generation));
    secondSession->update(0, engine.getEditorConfig(), true);
    // 检查消费后的权威轨数。队列中有请求与会话已实际执行编辑是两个条件。
    const bool acceptedNew = secondSession->getContext().trackCount == 7 &&
                             audio.getPlaybackSpeed() == originalSpeed;
    // 最后关闭仍沿用被复用的零号槽，结果必须撤回谱面而非恢复到旧对象。
    engine.closeSession(first, false);
    // 关闭会话是同步入口，完整撤回应在它返回时可见，无需等一次逻辑轮询。
    const auto empty = engine.activePlaybackState();
    // 最后一项已关闭，但测试仍持有原会话。无目标命令不能沿用这份旧弱引用。
    engine.pushCommand(MMM::Logic::CmdSetPlaybackSpeed{ testSpeed });
    // 这里检查队列而不手动更新已关闭会话，避免把旧对象重新标成活动项。
    const bool noClosedTarget = !secondSession->hasPendingCommands();
    // 显式归还全局速度和音高，保证回归失败时也能清理测试状态。
    // 关闭不会恢复用户试听参数；清理必须独立于是否收到前面的调速事件。
    audio.setPlaybackSpeed(originalSpeed);
    audio.setPlaybackPitch(originalPitch);

    // 查验代次与状态一起切换，后台重索引时不能混入先前的谱面键数。
    const bool published =
        original.playerTracks == 5 && loading.playerTracks == 0 &&
        !loading.playing && loading.generation != original.generation &&
        openedPopup.playerTracks == 5 && !openedPopup.playing &&
        replacement.playerTracks == 8 &&
        replacement.generation != openedPopup.generation &&
        empty.playerTracks == 0 && !empty.playing &&
        empty.generation != replacement.generation;
    // 及时返回和返回正确值独立检查，提前返回全零快照也算失败。
    // 回读仍应对应首项，因为另一线程只生产命令，尚无逻辑线程消费。
    const bool readable = returned && whileLocked == openedPopup;
    // 分开打印失败阶段，便于辨别读发布错误和代次路由错误。
    if ( !published || !readable || !playbackApplied || !reusedIndex ||
         !ignoredOld || !acceptedNew || !noClosedTarget )
        XERROR(
            "Playback state regression: state={}, read={}, applied={}, "
            "reuse={}, stale={}, "
            "current={}, closed={}",
            published,
            readable,
            playbackApplied,
            reusedIndex,
            ignoredOld,
            acceptedNew,
            noClosedTarget);
    return published && readable && playbackApplied && reusedIndex &&
           ignoredOld && acceptedNew && noClosedTarget;
}

/// @brief 验证活动时间点表按需发布、跨关闭保活及无变化时复用。
/// @return 完整行、排序、更新边界及活动目标归属均正确时返回 true。
/// @note 逻辑线程未启动。夹具用正常队列载入内存谱面，再手动推进 update。
/// @note 第一张谱面的旧快照跨编辑与会话关闭留存，确保没有原地修改共享行。
/// @note 测试不创建时间线相机，表格不能只靠相机可见区域组织输入。
/// @note 事件时间采用精确可表示的小整数秒，断言无需给排序容差。
/// @note 读接口应将没有会话与已有会话但尚未生成第一份表格区分开。
/// @note 自动备份和项目磁盘写入在此用例均未配置，清理由正式关闭入口完成。
/// @note 夹具的编辑器只消费本线程预先排队的命令，不从异步查询访问模型。
bool testTimingTablePublication()
{
    using namespace MMM;
    auto& engine = Logic::EditorEngine::instance();
    auto  map    = std::make_shared<BeatMap>();
    // 固定轨道数只用于建立真实谱面，会话不需要音轨、目录或屏幕控制器。
    map->m_baseMapMetadata.track_count = 4;
    const auto first   = engine.createSession(map, "Timing source", false);
    auto       session = engine.getActiveSession();
    const auto config  = engine.getEditorConfig();

    // 先准备真实事件。两项倒序提交，用不同效果和时间验证并非视口裁剪列表。
    session->pushCommand(Logic::CmdCreateTimelineEvent{
        .time = 6.0, .type = TimingEffect::SCROLL, .value = 1.5 });
    session->pushCommand(Logic::CmdCreateTimelineEvent{
        .time = 2.0, .type = TimingEffect::HS, .value = 1.25 });
    // 主视口进入空闲前的首个查询只提出请求。它既不阻塞，也不展示默认空表。
    const auto pending = Logic::readActiveTimingTable();
    session->update(0, config, true);
    // 首次更新消费加载与两个创建事件，读模型必须在事件提交后形成同一代。
    const auto initial = Logic::readActiveTimingTable();

    // 阻塞旧的直接读取路径会使异步调用在持锁期间超时。
    // 解锁后回收 future，故障场景也不会让测试进程挂死。
    std::unique_lock sessionLock(engine.getSessionMutex());
    auto             reader = std::async(std::launch::async,
                             [] { return Logic::readActiveTimingTable(); });
    const bool       independent =
        reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    sessionLock.unlock();
    const auto concurrent = reader.get();
    session->update(0, config, true);
    // 普通轮询不能发布内容相同的新对象，使用具体句柄身份验证分配复用。
    const auto unchanged = Logic::readActiveTimingTable();

    // 下一次 Timing 编辑才替换共享数据；旧行必须保有已呈现给 UI 的原始值。
    session->pushCommand(Logic::CmdCreateTimelineEvent{
        .time = 4.0, .type = TimingEffect::STOP, .value = 0.5 });
    session->update(0, config, true);
    const auto edited = Logic::readActiveTimingTable();
    // 对实际时间查找，允许谱面载入时补默认 BPM 行，不把默认行当作测试事件。
    const auto contains = [](const Logic::TimingTableSnapshot& table,
                             double                            time) {
        return std::ranges::any_of(
            table.rows, [time](const auto& row) { return row.time == time; });
    };
    // 以期待的两个事件确认列表完整，再单独检查所有行都按真实时间排列。
    const bool sorted = initial && *initial && contains(**initial, 2) &&
                        contains(**initial, 6) &&
                        std::ranges::is_sorted(
                            (*initial)->rows,
                            {},
                            &Common::Render::TimelineInteractiveElement::time);
    // 修订变化和实例变化独立。采纳编辑后仍属于第一图，新增事件只在新句柄中。
    const bool revised =
        initial && *initial && edited && *edited && *edited != *initial &&
        (*edited)->beatmapInstanceId == (*initial)->beatmapInstanceId &&
        contains(**edited, 4) && !contains(**initial, 4);

    // 新建另一会话后应首次等待它自己的数据，不能返回第一份实例的旧行。
    auto other                           = std::make_shared<BeatMap>();
    other->m_baseMapMetadata.track_count = 4;
    const auto second   = engine.createSession(other, "Timing target", false);
    const auto switched = Logic::readActiveTimingTable();
    // 第二图尚未要求逻辑线程推进即关闭，未完成的投影不能影响回退的第一图。
    engine.closeSession(second, false);
    // 关闭活动项后退回第一张图，可以复用已经准备好的完整快照。
    const auto returned = Logic::readActiveTimingTable();
    // 同一个会话内载入另一份模型，即使新模型可能从同一滚动修订开始，
    // 也必须撤回旧行。只靠活动索引比较无法识别这次实例改变。
    session->pushCommand(Logic::CmdLoadBeatmap{ other });
    session->update(0, config, true);
    // 刷新请求在装载前已经存在，完成这次 update 后应直接读到新实例。
    const auto reloaded = Logic::readActiveTimingTable();
    // 新谱面没有已编辑的 STOP。旧时间点若泄漏到同索引的新实例，绝不能通过。
    const bool replaced =
        edited && *edited && reloaded && *reloaded &&
        (*edited)->beatmapInstanceId != (*reloaded)->beatmapInstanceId &&
        !contains(**reloaded, 4);
    // 显式关闭测试用会话，避免污染调用链中之后读取引擎单例的用例。
    engine.closeSession(first, false);
    // 释放夹具持有的最后一份会话引用，让后面的读访问仅依赖快照所有权。
    session.reset();

    // 关闭真实谱面即停止路由，测试所持的旧首代仍能独立读取。
    // 保留原始句柄到最终断言，检验行内实体值不会借用已销毁的源容器。
    const bool closed = !Logic::readActiveTimingTable() && initial &&
                        *initial && contains(**initial, 2);
    // 联合检查挂起、调度和数据条件，提前返回空值不能掩盖等待或误路由。
    const bool ok = pending && !*pending && independent &&
                    concurrent == initial && unchanged == initial && sorted &&
                    revised && switched && !*switched && returned == edited &&
                    replaced && closed;
    // 分别输出锁读取、数据和销毁阶段，便于在平台上有针对性地排查。
    if ( !ok )
        XERROR(
            "Timing table publication regression: lock={}, sort={}, edit={}, "
            "reload={}, closed={}",
            independent,
            sorted,
            revised,
            replaced,
            closed);
    return ok;
}

/// @brief 确认后台谱面未保存时也要求退出确认，并独立保存活动谱面名称。
/// @return 是否正确区分全局脏状态、活动标签和经过关闭的会话。
///
/// 构造两张内存谱面，无需模拟平台窗口或写入工程文件。退出提示由普通关闭
/// 入口在同一临界区读取脏状态和名称，名称随快照复制到界面，因此可在其
/// 对应的会话关闭后继续显示。此用例直接运行这一捕获入口。
/// @note 撤销栈保存基线通过内存状态改变，与真实的谱面保存命令路径独立。
bool testExitConfirmationMapName()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 两张模型谱面的名称与 tab 名刻意不同，验证弹窗使用原始显示内容。
    const auto nameMap = [](std::string name) {
        auto beatmap                    = std::make_shared<MMM::BeatMap>();
        beatmap->m_baseMapMetadata.name = std::move(name);
        // 四键是载入内存谱面的最小有效夹具，不需要音频和工程资源。
        beatmap->m_baseMapMetadata.track_count = 4;
        return beatmap;
    };
    const auto first = engine.createSession(
        nameMap("First raw name"), "First tab caption", false);
    const auto second = engine.createSession(
        nameMap("Second raw name"), "Second tab caption", false);
    // 创建只排队载入谱面。这里没有后台逻辑线程，须推进各会话消费载入命令。
    const auto config = engine.getEditorConfig();
    engine.setActiveSessionIndex(second);
    if ( auto secondSession = engine.getActiveSession() )
        secondSession->update(0, config, true);
    // 固定第一张为最初的活动谱面，再推进其载入。
    engine.setActiveSessionIndex(first);
    auto session = engine.getActiveSession();
    if ( session ) session->update(0, config, true);
    // 干净的会话不阻止普通退出；直接改变撤销基线隔离此处的确认逻辑。
    bool valid = session && !engine.captureUnsavedExitMapName();
    if ( session ) session->getContextMutable().actionStack.markDirty();

    // 切到干净谱面后，后台的未保存修改仍应触发确认。
    engine.setActiveSessionIndex(second);
    const auto captured = engine.captureUnsavedExitMapName();
    // 确认词使用当前窗口展示的谱面，不取决于首先检测到的脏撤销栈。
    valid &= captured && *captured == "Second raw name";

    // 活动谱面关闭后再次查询时应看到剩余谱面，已捕获的值仍供旧弹窗保留。
    engine.closeSession(second, false);
    engine.setActiveSessionIndex(first);
    const auto remaining = engine.captureUnsavedExitMapName();
    // 第二次捕获的名称与先前的返回值分别独立，不依赖缓存中的活动索引。
    valid &= remaining && *remaining == "First raw name" && captured &&
             *captured == "Second raw name";
    // 保存使活动谱面恢复干净；再次发起关闭时旧的提示值不能影响新查询。
    if ( session ) session->getContextMutable().actionStack.markSaved();
    valid &= !engine.captureUnsavedExitMapName();
    // 即便之前的断言已失败，也要关闭夹具会话，防止进程退出阶段保留对象。
    engine.closeSession(first, false);
    valid &= !engine.captureUnsavedExitMapName();
    if ( !valid )
        XERROR("Exit confirmation did not preserve active map identity");
    return valid;
}

/// @brief 通过常规会话载入，验证导出对话框的命名和各格式兼容性快照。
/// @return 四种目标只读到所需特征，且名称和标志可越过会话关闭时返回 true。
///
/// 夹具同时提供 Unicode 标题、扩展物件、每个格式要单独警告的 timing
/// 和元数据。用内存谱面检查读模型本身，不需要打开文件对话框或写谱面。
/// @note 在同一个加载版本中请求四种目标；某种格式的低频分析不能清除模型
/// 中其余格式需要的元数据。最后再次查询空会话，与旧快照的可用性分别断言。
/// @note 这里不检查界面翻译和警告文案，避免因文本调整掩盖格式判定的变化。
bool testBeatmapExportPresentation()
{
    using namespace MMM;
    using Logic::BeatmapExportFormat;
    auto& engine = Logic::EditorEngine::instance();
    auto  map    = std::make_shared<BeatMap>();
    // 命名使用 Unicode 标题；原始名称仍保留给使用原文件主体的格式。
    map->m_baseMapMetadata.name          = "Export name";
    map->m_baseMapMetadata.title         = "Fallback title";
    map->m_baseMapMetadata.title_unicode = "导出标题";
    map->m_baseMapMetadata.version       = "EX";
    map->m_baseMapMetadata.track_count   = 6;
    // Malody 支持首尾空白，严格整数字段仍解析为 Key 模式。
    map->m_metadata.map_properties[MapMetadataType::MALODY]["mode"] = " 0 ";
    // 独立 Scroll 事件触发负倍率警告，同时确认没有把它解释成 Jump 或 HS。
    map->m_timings.push_back(Timing{});
    auto& scroll                   = map->m_timings.back();
    scroll.m_timestamp             = 1000.0;
    scroll.m_timingEffect          = TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = -1.0;
    // BMS 将普通音符的独立增益视为退化；Flick 另有格式能力提示。
    map->m_noteData.notes.emplace_back().setSampleBinding(AudioSampleBinding{
        .m_audioResourceId = "test-sample", .m_volume = 0.5F });
    // 同一物件还应启用 Malody 扩展选项，两个格式对该物件的动作不同。
    map->m_noteData.flicks.emplace_back();
    // 建立完整对象引用视图后再载入，避免元数据检查读取尚未同步的派生索引。
    map->sync();
    const auto index   = engine.createSession(map, "Export capture", false);
    auto       session = engine.getActiveSession();
    // 逻辑线程在此未启动，手动消费载入命令，使查询走真实活动谱面入口。
    if ( session ) session->update(0, engine.getEditorConfig(), true);
    // 命名读取与目标兼容性读取分开，短路径不需要遍历整个谱面。
    const auto naming = Logic::captureBeatmapExportNaming();
    // 每种格式用各自的请求范围，防止单一全量结构掩盖缺失的映射。
    const auto bms =
        Logic::captureBeatmapExportCompatibility(BeatmapExportFormat::Bms);
    const auto osu =
        Logic::captureBeatmapExportCompatibility(BeatmapExportFormat::Osu);
    const auto imd =
        Logic::captureBeatmapExportCompatibility(BeatmapExportFormat::Imd);
    const auto mc =
        Logic::captureBeatmapExportCompatibility(BeatmapExportFormat::Malody);
    // 关闭真实注册的活动会话，不能仅仅释放夹具构造时的 shared_ptr。
    engine.closeSession(index, false);
    // 关闭后再比较，若读取结果依赖 SessionContext 存活，此处无法安全使用。
    // BMS 的音量、osu! 的负 Scroll、IMD 的丢字段与 Malody 的 mode 单独核对。
    const bool valid =
        naming && naming->name == "Export name" &&
        naming->preferredTitle == "导出标题" && naming->version == "EX" &&
        naming->trackCount == 6 && bms && bms->hasBmsTimingDifferences &&
        bms->hasNonDefaultSampleVolume && bms->hasFlick && osu &&
        osu->hasNegativeScroll && !osu->hasJumpOrHs && imd &&
        imd->hasImdTimingDifferences && imd->hasImdMapMetadataDifferences &&
        mc && mc->malodyMode == 0 && mc->hasFlick &&
        !Logic::captureBeatmapExportNaming();
    // 退出路径仍清理测试会话，让其它快照用例可以独立检查活动身份。
    if ( !valid ) XERROR("Active beatmap export presentation mismatch");
    return valid;
}

/// @brief 验证设置读取在会话锁占用时立即提交，且文件与模型身份独立保活。
/// @param outputRoot CTest 传入的构建输出目录，文件探测只访问其独立子目录。
/// @return
/// 候选按请求枚举、打开会话被替换时旧设置不生效、已有字段修改被保留时返回
/// true。
/// @note 逻辑线程由用例显式推进；真实文件线程负责路径检查和资源遍历。
/// 夹具创建空媒体文件，枚举只看类型和路径，不需要外部解码器或用户项目。
/// 标题与作者使用不同输入，防止全量改写在同字段上被覆盖顺序掩盖。
/// 在捕捉快照和提交之间切换只读状态，验证以消费时项目能力为准。
/// 换谱保留旧模型引用及相同路径，确保测试覆盖真实的身份拒绝。
/// 两个文件结果都经真实线程池完成，测试线程只手动推进逻辑捕获。
/// 文件夹位于 CTest 传入的输出目录，完成后撤销借用状态并删除夹具目录。
/// 轨道与标题两个过期写入都要被拦下，便于覆盖各自独立的队列分支。
/// 已扫描列表和普通快照分开断言，识别文件扫描误入定期刷新路径。
/// 保留旧结果直到模拟界面消费完毕，覆盖同一版本共享及失效后的独立所有权。
/// 工程内文件检查使用真正的后台池；仅在接收到通知后检查状态是否变化。
/// 回归将工程清单发布与目录通知分开触发，避免两种失效来源互相覆盖。
/// 等待文件结果前先撤掉测试锁，避免因夹具锁而掩盖文件阶段的正常进度。
/// 只读模式下修改实际可写的字段，保证断言确实来自项目门禁。
bool testBeatmapSettingsSnapshot(const std::filesystem::path& outputRoot)
{
    using namespace MMM;
    using namespace std::chrono_literals;
    auto& engine  = Logic::EditorEngine::instance();
    auto& project = Logic::ProjectController::instance();
    // 文件夹名只用于测试隔离，进程单例的真实当前工程由控制器建立。
    // 每次运行都用独立目录，失败残留不会被下一次扫描当作新的候选。
    // CTest 传入构建输出根，文件不会写入源码或资源目录。
    const auto root =
        outputRoot /
        ("beatmap-settings-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    // 不清理任何已有工程目录；成功创建的根只在本用例末尾删除。
    std::error_code error;
    std::filesystem::create_directories(root / "nested", error);
    if ( error ) return false;
    // 大写图片、嵌套视频和无关扩展名同时存在，验证两类候选只枚举一次且互不混入。
    // 大写扩展名让仅支持小写比较的实现产生可观察失败。
    std::ofstream(root / "cover.PNG").put('i');
    // 视频放到次级目录，顶层遍历无法碰巧通过候选断言。
    std::ofstream(root / "nested" / "bg.MP4").put('v');
    std::ofstream(root / "ignore.txt").put('x');
    // 由真实控制器装载工程上下文，显式推进逻辑循环可隔离外部渲染节拍。
    // 文件池仍正常运行，确保回归确实跨越逻辑捕获和后台计算两段。
    const auto opened = project.openProject(root);
    // 使用带历史目录前缀的主音频，使旧实现对每项至少尝试一次存在性检查。
    // 不创建大量波形文件。候选需要保留无效引用，让原实现的探测路径仍可达。
    constexpr int resourceCount = 1024;
    bool          published     = false;
    if ( auto* liveProject = project.currentProject() ) {
        for ( int i = 0; i < resourceCount; ++i ) {
            const auto id = "audio-" + std::to_string(i);
            liveProject->m_audioResources.push_back({
                .m_id     = id,
                .m_path   = Config::pathToUtf8(root.filename() / (id + ".wav")),
                .m_config = {},
            });
        }
        published = project.saveProject();
    }
    auto& pool = Runtime::AppThreadPool::instance();
    // 此用例只初始化文件阶段依赖的池，无需启动主线程或渲染器。
    // 测试套件运行本用例之前没有主应用的启动逻辑，因此显式初始化共享文件池。
    pool.init();
    auto map                     = std::make_shared<BeatMap>();
    map->m_baseMapMetadata.title = "original";
    // 长字符串越过小字符串优化阈值，让后续差量检查也覆盖完整字符串的保留。
    // 这两个字段独立于背景和标题；长内容无需参与设置页本次提交。
    const std::string album(4093, 'A');
    const std::string artist(4099, 'B');
    map->m_baseMapMetadata.album          = album;
    map->m_baseMapMetadata.artist_unicode = artist;
    // 重载前会冲刷尾随保存。提供同一条可写 .mmm 路径，避免把保存失败
    // 当成旧设置成功拦截，也不会在仓库根目录留下无名测试文件。
    map->m_baseMapMetadata.map_path    = root / "same-chart.mmm";
    map->m_baseMapMetadata.cover_path  = "cover.PNG";
    map->m_baseMapMetadata.track_count = 4;
    map->sync();
    const auto index   = engine.createSession(map, "Settings source", false);
    const auto session = engine.getActiveSession();
    // createSession 仅排载入命令；必须先实际消费，不能直接构造伪造的返回结果。
    session->update(0, engine.getEditorConfig(), true);

    // 模拟逻辑持有会话锁。界面应仍能提交请求，由下一次逻辑推进捕获。
    std::unique_lock held(engine.getSessionMutex());
    auto             enqueue = std::async(
        std::launch::async, [] { return Logic::requestBeatmapSettings(true); });
    const bool enqueuedWithoutSessionLock =
        enqueue.wait_for(1s) == std::future_status::ready;
    // 定时等待只检测提交是否被挡；释放夹具锁后再等待实际响应，
    // 即使回归失败也能退出，不会等待一个被测试自己占用的锁。
    held.unlock();
    // 等待分两段，返回 future 只代表 UI 已提交，文件结果此时仍未产生。
    auto read = enqueue.get();
    Logic::processBeatmapSettingsRequest();
    // 目录收集需要经过实际的文件池，未完成时不可能读出两个候选。
    const auto scanned = read.get();
    auto       refresh = Logic::requestBeatmapSettings(false);
    Logic::processBeatmapSettingsRequest();
    const auto normal = refresh.get();
    // 校验相对路径不仅确认扩展名，还防止后台错误拼接项目根或压平子目录。
    // 普通读取要保持 scanned=false，使 UI 可以保留已有资源弹层缓存。
    // 后台保留原生路径分隔符，用路径分量比较 Windows 和 POSIX 的视频结果。
    const bool fileResults =
        scanned && normal && scanned->scanned && scanned->projectRoot == root &&
        scanned->paths[2].exists && scanned->paths[2].display == "cover.PNG" &&
        scanned->images == std::vector<std::string>{ "cover.PNG" } &&
        scanned->videos.size() == 1 &&
        Config::utf8ToPath(scanned->videos.front()) ==
            std::filesystem::path("nested") / "bg.MP4" &&
        !normal->scanned && normal->images.empty() && normal->videos.empty();

    // 输入未变化时处理请求后必须立即就绪并返回原对象。这条分支在取文件池
    // 之前退出，因此同时验证 32 次静置刷新中没有新文件任务和路径探测。
    bool idleReused =
        published && normal && normal->audio.size() == resourceCount;
    // 不消耗墙钟时间等待 150 ms；直接推进逻辑处理，等价于界面的连续采样。
    // 同一对象身份也排除后台很快返回一个内容相同但重新计算的结果。
    for ( int i = 0; i < 32 && idleReused; ++i ) {
        auto idle = Logic::requestBeatmapSettings(false, normal);
        Logic::processBeatmapSettingsRequest();
        idleReused = idle.wait_for(0s) == std::future_status::ready &&
                     idle.get() == normal;
    }

    // 正常发布资源更新，即使活动谱面和目录代次相同，旧音频候选也必须失效。
    bool resourceInvalidation = false;
    std::shared_ptr<const Logic::BeatmapSettingsSnapshot> latest;
    if ( auto* liveProject = project.currentProject();
         liveProject && !liveProject->m_audioResources.empty() ) {
        // 定向减少一个音频条目，使输出数量直接区分新旧读模型。
        liveProject->m_audioResources.pop_back();
        const bool saved = project.saveProject();
        // 请求仍携带保存前的结果，模拟 UI 尚未见到资源列表改变。
        auto changed = Logic::requestBeatmapSettings(false, normal);
        Logic::processBeatmapSettingsRequest();
        latest               = changed.get();
        resourceInvalidation = saved && latest && latest != normal &&
                               latest->audio.size() == resourceCount - 1;
    }

    // 同步清单已经发布，移走封面后仅消费真实目录通知。快照必须在
    // 工程资源读模型仍相同的情况下更新存在性，不能依赖后续清单持久化。
    bool directoryInvalidation = false;
    if ( latest ) {
        // 只改动实际文件名，故活动谱面的引用仍指向原位置。
        // 监听既可通过系统通知交付，也可通过 POSIX 后台轮询交付。
        std::filesystem::rename(root / "cover.PNG", root / "moved.PNG", error);
        const auto until = std::chrono::steady_clock::now() + 4s;
        while ( !error && !directoryInvalidation &&
                std::chrono::steady_clock::now() < until ) {
            // 用生产逻辑的消费入口递增修订；测试不直接写计数或篡改快照。
            if ( project.consumeDirectoryChangePending() ) {
                auto changed = Logic::requestBeatmapSettings(false, latest);
                Logic::processBeatmapSettingsRequest();
                // 仅等待已经由逻辑接走的文件任务，文件线程无需测试线程再次推进。
                const auto after = changed.get();
                directoryInvalidation =
                    after && after != latest &&
                    after->directoryRevision != latest->directoryRevision &&
                    !after->paths[2].exists;
            } else {
                // 仅测试线程等待系统通知，不阻塞会话、绘制或文件工作者。
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    bool editsPreserved   = false;
    bool staleEditIgnored = false;
    bool readOnlyRejected = false;
    if ( normal && project.currentProject() ) {
        // 在捕获之后才切成只读，确保写入门禁以消费时工程为准，而非旧快照标记。
        // 临时标志复用控制器的真实读取口径，不需要压缩测试工程文件。
        auto& currentProject                = *project.currentProject();
        currentProject.m_isTemporaryProject = true;
        auto blockedEdit                    = normal->metadata;
        blockedEdit.title                   = "rejected while read-only";
        Logic::submitBeatmapSettingsCommand(
            *normal,
            Logic::CmdUpdateBeatmapMetadata{
                .baseMeta = blockedEdit,
                .settingsBaseline =
                    std::make_shared<const BaseMapMeta>(normal->metadata) });
        session->update(0, engine.getEditorConfig(), true);
        // 此时仍是同一个谱面控制块，旧身份保护不能代替临时工程写入检查。
        readOnlyRejected =
            session->getContext().currentBeatmap->m_baseMapMetadata.title ==
            "original";
        // 恢复工程能力后，后续合法设置与重载仍应成功，拒绝行为不应污染队列。
        currentProject.m_isTemporaryProject = false;

        // 先入队外部更新，再以同一帧基线提交多字段设置。
        // 作者和长专辑内容在草稿中都未编辑；消费时必须保留较新的外部值。
        // 新旧专辑长度不同，也能识别错误保留草稿容量但只回写一部分的实现。
        const std::string latestAlbum(8191, 'C');
        auto              external = normal->metadata;
        external.author            = "external-edit";
        external.album             = latestAlbum;
        session->pushCommand(
            Logic::CmdUpdateBeatmapMetadata{ .baseMeta = external });
        auto edit    = normal->metadata;
        edit.title   = "settings-edit";
        edit.version = "new difficulty";
        // 给两轴不同符号，避免同步时互换坐标仍通过回归。
        edit.bgxoffset = 23;
        edit.bgyoffset = -17;
        // 两个偏移和两段文本一起提交；每个字段都按同一份写入前初值比较。
        Logic::submitBeatmapSettingsCommand(
            *normal,
            Logic::CmdUpdateBeatmapMetadata{
                .baseMeta = edit,
                .settingsBaseline =
                    std::make_shared<const BaseMapMeta>(normal->metadata) });
        session->update(0, engine.getEditorConfig(), true);
        const auto& live =
            session->getContext().currentBeatmap->m_baseMapMetadata;
        // 第三个长字段既没有外部修改，也没有表单修改，仍须逐字完整。
        // 合并一次后立即读取会话状态，谱面序列化不会掩盖遗漏的字段。
        editsPreserved =
            live.title == "settings-edit" && live.version == "new difficulty" &&
            live.bgxoffset == 23 && live.bgyoffset == -17 &&
            live.author == "external-edit" && live.album == latestAlbum &&
            live.artist_unicode == artist;

        // 重载保持会话和谱面路径不变，必须靠模型身份丢弃旧命令；保留源引用
        // 让提交阶段无法借助 weak_ptr 过期提前返回，确实走到消费阶段检查。
        const auto oldMap      = normal->beatmap.lock();
        auto       replacement = std::make_shared<BeatMap>();
        // 两份谱面刻意重用完全相同的绝对路径，使路径判断无法代替实例校验。
        replacement->m_baseMapMetadata.map_path    = root / "same-chart.mmm";
        replacement->m_baseMapMetadata.track_count = 4;
        replacement->m_baseMapMetadata.title       = "replacement";
        // 新谱面没有项目数据差异，重载仍须清除旧快照的写入资格。
        replacement->sync();
        // 载图必须排在旧设置之前，而快照的弱引用仍然有效。
        // 若消费侧只检查会话索引或路径，两次旧编辑都会误写新载图。
        session->pushCommand(Logic::CmdLoadBeatmap{ replacement });
        Logic::submitBeatmapSettingsCommand(
            *normal,
            Logic::CmdUpdateBeatmapMetadata{
                .baseMeta = edit,
                .settingsBaseline =
                    std::make_shared<const BaseMapMeta>(normal->metadata) });
        Logic::submitBeatmapSettingsCommand(*normal,
                                            Logic::CmdUpdateBgmTrackCount{ 7 });
        session->update(0, engine.getEditorConfig(), true);
        // 先验证确实完成重载，随后验证标题及 BGM 数量都保持新目标的值。
        staleEditIgnored =
            oldMap && session->getContext().currentBeatmap != oldMap &&
            session->getContext().currentBeatmap->m_baseMapMetadata.title ==
                "replacement" &&
            session->getContext().bgmTrackCount != 7;
    }
    // 释放项目和会话后继续读取已完成的数据，旧快照未借用模型内部字符串。
    engine.closeSession(index, false);
    // 关闭项目同时停止目录监视，避免清理夹具文件时产生无关刷新。
    project.closeProject();
    // 正常分支已收取各次请求的结果。即使静置断言失败，关闭文件池也会
    // 清空已经接收的任务，再删除文件夹，避免夹具清理和后台访问竞争。
    pool.shutdown();
    // 只删除当前测试创建的根，路径不来自输入文件或项目配置。
    std::filesystem::remove_all(root, error);
    // 关闭之后只检查不可变值，不依赖旧会话或文件还在磁盘上。
    // 短路条件保护最后一项快照读取；文件阶段失败时 scanned 可能为空。
    const bool valid = opened.m_opened && enqueuedWithoutSessionLock &&
                       fileResults && editsPreserved && staleEditIgnored &&
                       readOnlyRejected && idleReused && resourceInvalidation &&
                       directoryInvalidation &&
                       scanned->metadata.title == "original";
    // 失败日志区分文件枚举、写入合并、身份、只读和请求锁五个独立故障点。
    if ( !valid )
        XERROR(
            "Beatmap settings snapshot mismatch: files={}, edits={}, stale={}, "
            "unlocked={}, readOnly={}, idleReused={}, resourceInvalidation={}, "
            "directoryInvalidation={}",
            fileResults,
            editsPreserved,
            staleEditIgnored,
            enqueuedWithoutSessionLock,
            readOnlyRejected,
            idleReused,
            resourceInvalidation,
            directoryInvalidation);
    return valid;
}

}  // namespace

/// @brief 运行编辑器会话状态与设置快照测试。
/// @param argc 测试注册入口传入的参数数量。
/// @param argv 第一项参数为本用例专用的文件输出目录。
/// @return 全部测试通过时返回 0。
/// @note 按场景短路执行；首个失败会阻止后续测试运行。
/// @note 单例共享仅限当前测试进程，结果不说明应用重启后的恢复行为。
int main(int argc, char** argv)
{
    // 文件夹由测试注册显式指定，避免没有参数时落到源码目录。
    if ( argc != 2 ) return 1;
    const auto outputRoot = MMM::Config::utf8ToPath(argv[1]);
    // 先验证跨会话共享状态，再验证目标路由隔离，最后执行配置并发读取回归。
    return testClearAllNoteColors() &&
                   testSkinSelectionSurvivesPaletteRefresh() &&
                   testObservationCameraSurvivesStaleConfig() &&
                   testPaletteRestoredAfterSessionClose() &&
                   testAudioResourceRestoredAfterSessionClose() &&
                   testBackgroundCanvasMousePositionRouting() &&
                   testLatestCanvasFocusRequest() &&
                   testPublishedActiveGestureState() &&
                   testToolbarTrackLayoutPublication() &&
                   testActivePlaybackStatePublication() &&
                   testTimingTablePublication() &&
                   testExitConfirmationMapName() &&
                   testBeatmapExportPresentation() &&
                   testBeatmapSettingsSnapshot(outputRoot) &&
                   testCameraConfigSnapshot() &&
                   testConcurrentEditorConfigSnapshots()
               ? 0
               : 1;
}
