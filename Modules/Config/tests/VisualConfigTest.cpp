#include "config/VisualConfig.h"

#include "log/colorful-log.h"

#include <cmath>
#include <nlohmann/json.hpp>

namespace
{

/// @brief 使用小容差比较视觉配置中的单精度数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证当前分拍线显示模式与自动范围能够完整往返。
/// @return 当前格式往返无损时返回 true。
bool testBeatLineDisplayModeRoundTrip()
{
    MMM::Config::VisualConfig source;
    source.beatLineDisplayMode = MMM::Config::BeatLineDisplayMode::NearCursor;
    source.beatLineCursorVisibleRatio = 0.27F;
    source.beatLineCursorFadeRatio    = 0.31F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    if ( restored.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::NearCursor ||
         !near(restored.beatLineCursorVisibleRatio, 0.27F) ||
         !near(restored.beatLineCursorFadeRatio, 0.31F) ||
         !encoded.value("drawBeatLines", false) ) {
        XERROR("Beat line display mode did not survive JSON round trip");
        return false;
    }
    return true;
}

/// @brief 验证旧版 drawBeatLines 布尔值能够迁移为三态显示模式。
/// @return 旧版开关的开启与关闭语义均保持时返回 true。
bool testLegacyDrawBeatLinesMigration()
{
    const nlohmann::json hiddenJson{ { "drawBeatLines", false } };
    const nlohmann::json visibleJson{ { "drawBeatLines", true } };
    const auto           hidden  = hiddenJson.get<MMM::Config::VisualConfig>();
    const auto           visible = visibleJson.get<MMM::Config::VisualConfig>();
    if ( hidden.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Hidden ||
         visible.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Always ) {
        XERROR("Legacy drawBeatLines value was not migrated");
        return false;
    }
    return true;
}

/// @brief 验证自动显示比例在读取配置时被限制到工具栏允许范围。
/// @return 过小和过大的比例均被正确限制时返回 true。
bool testBeatLineAutoRatioClamping()
{
    const nlohmann::json json{
        { "beatLineDisplayMode", "NearCursor" },
        { "beatLineCursorVisibleRatio", 0.0F },
        { "beatLineCursorFadeRatio", 1.0F },
    };
    const auto config = json.get<MMM::Config::VisualConfig>();
    if ( !near(config.beatLineCursorVisibleRatio, 0.05F) ||
         !near(config.beatLineCursorFadeRatio, 0.40F) ) {
        XERROR("Beat line auto display ratios escaped supported bounds");
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置能够完整往返。
/// @return 当前格式往返无损时返回 true。
bool testBackgroundSpectrumRoundTrip()
{
    MMM::Config::VisualConfig source;
    auto&                     spectrum = source.background.spectrum;
    spectrum.enabled                   = true;
    spectrum.bandCount                 = 48;
    spectrum.widthRatio                = 0.72F;
    spectrum.heightRatio               = 0.44F;
    spectrum.baselineRatio             = 0.83F;
    spectrum.opacity                   = 0.27F;
    spectrum.includeHitEffects         = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto&          result   = restored.background.spectrum;
    if ( !result.enabled || result.bandCount != 48 ||
         !near(result.widthRatio, 0.72F) || !near(result.heightRatio, 0.44F) ||
         !near(result.baselineRatio, 0.83F) || !near(result.opacity, 0.27F) ||
         result.includeHitEffects ) {
        XERROR("Background spectrum config did not survive JSON round trip");
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置在读取时被限制到设置菜单允许范围。
/// @return 所有越界字段均被正确限制时返回 true。
bool testBackgroundSpectrumClamping()
{
    const nlohmann::json json{ { "background",
                                 { { "spectrum",
                                     { { "bandCount", 2 },
                                       { "widthRatio", 0.0F },
                                       { "heightRatio", 2.0F },
                                       { "baselineRatio", -1.0F },
                                       { "opacity", 3.0F } } } } } };
    const auto           config   = json.get<MMM::Config::VisualConfig>();
    const auto&          spectrum = config.background.spectrum;
    if ( spectrum.bandCount != MMM::Config::BACKGROUND_SPECTRUM_MIN_BANDS ||
         !near(spectrum.widthRatio, 0.10F) ||
         !near(spectrum.heightRatio, 1.0F) ||
         !near(spectrum.baselineRatio, 0.05F) ||
         !near(spectrum.opacity, 1.0F) ) {
        XERROR("Background spectrum config escaped supported bounds");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行分拍线显示模式配置兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBeatLineDisplayModeRoundTrip() &&
                   testLegacyDrawBeatLinesMigration() &&
                   testBeatLineAutoRatioClamping() &&
                   testBackgroundSpectrumRoundTrip() &&
                   testBackgroundSpectrumClamping()
               ? 0
               : 1;
}
