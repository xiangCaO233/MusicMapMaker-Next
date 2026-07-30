#include "logic/EditorEngine.h"

#include "log/colorful-log.h"

#include <array>
#include <cmath>
#include <optional>

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

}  // namespace

/// @brief 运行画笔调色盘跨会话恢复测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testPaletteRestoredAfterSessionClose() &&
                   testAudioResourceRestoredAfterSessionClose()
               ? 0
               : 1;
}
