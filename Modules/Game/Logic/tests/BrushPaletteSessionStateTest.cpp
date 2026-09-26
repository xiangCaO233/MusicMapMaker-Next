#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/session/ActionController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"

#include "config/AppConfig.h"
#include "log/colorful-log.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>

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
                           colorMatches(restored.node, colors[5]);
    // 在关闭前完成所有引用读取，后面只保留布尔结果用于报告。
    engine.closeSession(0, false);
    if ( !matches ) {
        XERROR("New session did not restore the editor brush palette");
    }
    return matches;
}

/// @brief 验证新会话恢复编辑器级项目音频画笔选择。
/// @return 稳定资源 ID 与资源类型均跨会话保留时返回 true。
/// @note 不验证 main-track 指向真实资源，只验证选择状态的传递。
/// @note 未覆盖跨项目资源 ID 的有效性判断，不能据此宣称跨项目资源可用。
bool testAudioResourceRestoredAfterSessionClose()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    engine.createSession(nullptr, "Audio Source", false);
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
        return false;
    }
    session->update(0.0, engine.getEditorConfig(), true);

    const auto& brush = session->getContext().brushState;
    // 三个字段构成同一次音频选择，不能仅凭资源字符串相同判定恢复成功。
    const bool matches =
        brush.selectedAudioResourceId == "main-track" &&
        brush.selectedAudioTrackType == MMM::AudioTrackType::Main &&
        std::abs(brush.selectedAudioVolume - 0.65F) < 1e-6F;
    // 只读选择字段，不触发实际采样播放，测试不要求音频设备可用。
    engine.closeSession(0, false);
    if ( !matches ) {
        XERROR("New session did not restore the editor audio selection");
    }
    return matches;
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
    const bool routed = activeContext.mouseCameraId.empty() &&
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
    app.getEditorSettings().selectedSkinDirectory    = "rm";
    stalePaletteConfig.visual.overrideBeatLineColors = true;
    engine.setEditorConfig(stalePaletteConfig);
    // 同时断言新配色仍生效，禁止通过丢弃整份配置来保住皮肤。
    // 两侧都必须一致，否则下一次引擎快照仍会继续传播旧值。
    bool ok = app.getEditorSettings().selectedSkinDirectory == "rm" &&
              engine.getEditorConfig().settings.selectedSkinDirectory == "rm" &&
              engine.getEditorConfig().visual.overrideBeatLineColors;
    // 完整走生产保存和读取路径，防止只修复当前窗口却仍保存旧选择。
    if ( ok ) {
        // 写入后清除内存选择，读取结果必须真正来自持久化文件。
        ok                                            = app.save();
        app.getEditorSettings().selectedSkinDirectory = "mmm-default";
        ok = app.load() && ok &&
             app.getEditorSettings().selectedSkinDirectory == "rm";
    }
    // 测试结束恢复单例，后续会话及并发测试不继承本场景状态。
    // 即使断言失败也执行恢复，避免污染同进程内后续测试。
    app.getEditorConfig() = originalApp;
    engine.setEditorConfig(originalEngine);
    if ( !ok ) XERROR("Palette refresh overwrote persisted skin selection");
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

}  // namespace

/// @brief 运行画笔调色盘跨会话恢复测试。
/// @return 全部测试通过时返回 0。
/// @note 按场景短路执行；首个失败会阻止后续测试运行。
/// @note 单例共享仅限当前测试进程，结果不说明应用重启后的恢复行为。
int main()
{
    // 先验证跨会话共享状态，再验证目标路由隔离，最后执行配置并发读取回归。
    return testClearAllNoteColors() &&
                   testSkinSelectionSurvivesPaletteRefresh() &&
                   testPaletteRestoredAfterSessionClose() &&
                   testAudioResourceRestoredAfterSessionClose() &&
                   testBackgroundCanvasMousePositionRouting() &&
                   testConcurrentEditorConfigSnapshots()
               ? 0
               : 1;
}
