#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"

#include "log/colorful-log.h"

#include <array>
#include <atomic>
#include <cmath>
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

}  // namespace

/// @brief 运行画笔调色盘跨会话恢复测试。
/// @return 全部测试通过时返回 0。
/// @note 按场景短路执行；首个失败会阻止后续测试运行。
/// @note 单例共享仅限当前测试进程，结果不说明应用重启后的恢复行为。
int main()
{
    // 先验证跨会话共享状态，再验证目标路由隔离，最后执行配置并发读取回归。
    return testPaletteRestoredAfterSessionClose() &&
                   testAudioResourceRestoredAfterSessionClose() &&
                   testBackgroundCanvasMousePositionRouting() &&
                   testConcurrentEditorConfigSnapshots()
               ? 0
               : 1;
}
