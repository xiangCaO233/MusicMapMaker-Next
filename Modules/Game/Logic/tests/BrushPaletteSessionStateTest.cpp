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

namespace
{

/// @brief 使用小容差比较颜色通道。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个通道足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 比较可选颜色与预期颜色。
/// @param actual 实际颜色。
/// @param expected 预期颜色。
/// @return 实际颜色存在且全部通道匹配时返回 true。
bool colorMatches(const std::optional<glm::vec4>& actual,
                  const glm::vec4&                expected)
{
    return actual && near(actual->r, expected.r) &&
           near(actual->g, expected.g) && near(actual->b, expected.b) &&
           near(actual->a, expected.a);
}

/// @brief 验证新会话恢复关闭前的画笔调色盘。
/// @return 调色盘跨会话保留时返回 true。
bool testPaletteRestoredAfterSessionClose()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    engine.createSession(nullptr, "Palette Source", false);
    const std::array<glm::vec4, MMM::Logic::NOTE_COLOR_SLOT_COUNT> colors{
        glm::vec4{ 0.11F, 0.12F, 0.13F, 1.0F },
        glm::vec4{ 0.21F, 0.22F, 0.23F, 1.0F },
        glm::vec4{ 0.31F, 0.32F, 0.33F, 1.0F },
        glm::vec4{ 0.41F, 0.42F, 0.43F, 1.0F },
        glm::vec4{ 0.51F, 0.52F, 0.53F, 1.0F },
        glm::vec4{ 0.61F, 0.62F, 0.63F, 1.0F },
    };
    engine.pushCommand(MMM::Logic::CmdSetBrushNotePalette{ colors });

    engine.closeSession(0, false);
    engine.createSession(nullptr, "Palette Target", true);

    auto session = engine.getActiveSession();
    if ( !session ) {
        XERROR("Palette target session was not created");
        return false;
    }
    session->update(0.0, engine.getEditorConfig(), true);

    const auto& restored = session->getContext().brushState.customColors;
    const bool  matches  = colorMatches(restored.tap, colors[0]) &&
                           colorMatches(restored.head, colors[1]) &&
                           colorMatches(restored.hold, colors[2]) &&
                           colorMatches(restored.end, colors[3]) &&
                           colorMatches(restored.flickArrow, colors[4]) &&
                           colorMatches(restored.node, colors[5]);
    engine.closeSession(0, false);
    if ( !matches ) {
        XERROR("New session did not restore the editor brush palette");
    }
    return matches;
}

/// @brief 验证新会话恢复编辑器级项目音频画笔选择。
/// @return 稳定资源 ID 与资源类型均跨会话保留时返回 true。
bool testAudioResourceRestoredAfterSessionClose()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    engine.createSession(nullptr, "Audio Source", false);
    engine.pushCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "main-track",
        .audioTrackType  = MMM::AudioTrackType::Main,
        .volume          = 0.65F,
    });
    engine.closeSession(0, false);
    engine.createSession(nullptr, "Audio Target", true);

    auto session = engine.getActiveSession();
    if ( !session ) {
        XERROR("Audio target session was not created");
        return false;
    }
    session->update(0.0, engine.getEditorConfig(), true);

    const auto& brush = session->getContext().brushState;
    const bool  matches =
        brush.selectedAudioResourceId == "main-track" &&
        brush.selectedAudioTrackType == MMM::AudioTrackType::Main &&
        std::abs(brush.selectedAudioVolume - 0.65F) < 1e-6F;
    engine.closeSession(0, false);
    if ( !matches ) {
        XERROR("New session did not restore the editor audio selection");
    }
    return matches;
}

/// @brief 验证主画布鼠标位置按 cameraId 路由到后台 Session。
/// @return 目标后台 Session 独立收到悬停坐标且活动 Session 未被污染时返回
/// true。
bool testBackgroundCanvasMousePositionRouting()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }

    const int32_t activeIndex =
        engine.createSession(nullptr, "Pointer Active", false);
    const int32_t backgroundIndex =
        engine.createSession(nullptr, "Pointer Background", false);
    const auto* activeEntry     = engine.getSessionEntry(activeIndex);
    const auto* backgroundEntry = engine.getSessionEntry(backgroundIndex);
    if ( !activeEntry || !activeEntry->session || !backgroundEntry ||
         !backgroundEntry->session ) {
        XERROR("Pointer routing sessions were not created");
        return false;
    }

    auto              activeSession      = activeEntry->session;
    auto              backgroundSession  = backgroundEntry->session;
    const std::string backgroundCameraId = backgroundEntry->cameraId;
    engine.setActiveSessionIndex(activeIndex);
    activeSession->update(0.0, engine.getEditorConfig(), true);
    backgroundSession->update(0.0, engine.getEditorConfig(), false);

    engine.pushCommand(MMM::Logic::CmdSetMousePosition{
        .cameraId       = backgroundCameraId,
        .mouseX         = 321.0F,
        .mouseY         = 234.0F,
        .viewportWidth  = 800.0F,
        .viewportHeight = 600.0F,
        .isHovering     = true,
    });
    activeSession->update(0.0, engine.getEditorConfig(), true);
    backgroundSession->update(0.0, engine.getEditorConfig(), false);

    const auto& activeContext     = activeSession->getContext();
    const auto& backgroundContext = backgroundSession->getContext();
    const bool  routed            = activeContext.mouseCameraId.empty() &&
                        backgroundContext.mouseCameraId == backgroundCameraId &&
                        backgroundContext.isMouseInCanvas &&
                        near(backgroundContext.lastMousePos.x, 321.0F) &&
                        near(backgroundContext.lastMousePos.y, 234.0F);

    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }
    if ( !routed ) {
        XERROR("Background canvas mouse position was routed to active session");
    }
    return routed;
}

/// @brief 验证布局拖拽式高频配置写入不会产生撕裂的容器和字符串快照。
/// @return 并发读取到的每份配置都保持内部字段一致时返回 true。
bool testConcurrentEditorConfigSnapshots()
{
    auto&             engine       = MMM::Logic::EditorEngine::instance();
    const auto        baseConfig   = engine.getEditorConfig();
    const std::string STRESS_KEY   = "layout-snapshot-stress";
    constexpr int     UPDATE_COUNT = 256;
    std::atomic<bool> writerDone{ false };

    std::thread writer([&]() {
        for ( int i = 0; i < UPDATE_COUNT; ++i ) {
            auto        updatedConfig = baseConfig;
            auto&       sfxConfig     = updatedConfig.settings.sfxConfig;
            const float marker        = static_cast<float>(i);
            sfxConfig.flickWidthVolumeMultiplier = marker;
            sfxConfig.permanentSfxVolumes.clear();
            sfxConfig.permanentSfxMutes.clear();
            sfxConfig.permanentSfxVolumes.emplace(STRESS_KEY, marker);
            sfxConfig.permanentSfxMutes.emplace(STRESS_KEY, (i % 2) == 0);
            engine.setEditorConfig(updatedConfig);
        }
        writerDone.store(true, std::memory_order_release);
    });

    bool consistent = true;
    while ( !writerDone.load(std::memory_order_acquire) ) {
        const auto  snapshot  = engine.getEditorConfig();
        const auto& sfxConfig = snapshot.settings.sfxConfig;
        const auto  marker    = sfxConfig.permanentSfxVolumes.find(STRESS_KEY);
        if ( marker != sfxConfig.permanentSfxVolumes.end() &&
             !near(marker->second, sfxConfig.flickWidthVolumeMultiplier) ) {
            consistent = false;
            break;
        }
    }

    writer.join();
    engine.setEditorConfig(baseConfig);
    if ( !consistent ) {
        XERROR("Concurrent editor config snapshot contained torn SFX fields");
    }
    return consistent;
}

}  // namespace

/// @brief 运行画笔调色盘跨会话恢复测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testPaletteRestoredAfterSessionClose() &&
                   testAudioResourceRestoredAfterSessionClose() &&
                   testBackgroundCanvasMousePositionRouting() &&
                   testConcurrentEditorConfigSnapshots()
               ? 0
               : 1;
}
