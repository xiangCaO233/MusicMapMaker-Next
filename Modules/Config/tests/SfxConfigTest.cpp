#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace
{

/// @brief 验证新版立体打击音效开关使用明确的新字段持久化。
/// @return 新字段往返且不再写出旧字段时返回 true。
bool testStereoHitEffectsRoundTrip()
{
    MMM::Config::SfxConfig source;
    source.enableStereoHitEffects = false;

    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);

    if ( restored.enableStereoHitEffects ||
         !encoded.contains("enableStereoHitEffects") ||
         encoded.contains("enableDirectionalFlickChannels") ) {
        XERROR("Stereo hit effect setting did not use the new JSON field");
        return false;
    }
    return true;
}

/// @brief 验证旧版 Flick 定向声道开关能够迁移到立体打击音效开关。
/// @return 旧字段的关闭状态被保留时返回 true。
bool testLegacyDirectionalFlickMigration()
{
    const nlohmann::json legacy{
        { "enableDirectionalFlickChannels", false },
    };
    MMM::Config::SfxConfig restored;
    from_json(legacy, restored);
    if ( restored.enableStereoHitEffects ) {
        XERROR("Legacy directional Flick setting was not migrated");
        return false;
    }
    return true;
}

/// @brief 验证新版字段存在时优先于旧版兼容字段。
/// @return 新字段的开启状态生效时返回 true。
bool testNewStereoSettingTakesPriority()
{
    const nlohmann::json mixed{
        { "enableStereoHitEffects", true },
        { "enableDirectionalFlickChannels", false },
    };
    MMM::Config::SfxConfig restored;
    from_json(mixed, restored);
    if ( !restored.enableStereoHitEffects ) {
        XERROR("New stereo hit effect setting did not take priority");
        return false;
    }
    return true;
}

/// @brief 验证绑定与未绑定打击音效控制可以完整持久化。
/// @return 四个字段写出并按原值恢复时返回 true。
bool testHitSoundGroupControlsRoundTrip()
{
    MMM::Config::SfxConfig source;
    source.enableUnboundHitSfx = false;
    source.unboundHitSfxGain   = 0.35F;
    source.enableBoundHitSfx   = false;
    source.boundHitSfxGain     = 1.75F;

    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);

    if ( restored.enableUnboundHitSfx || restored.enableBoundHitSfx ||
         restored.unboundHitSfxGain != source.unboundHitSfxGain ||
         restored.boundHitSfxGain != source.boundHitSfxGain ||
         !encoded.contains("enableUnboundHitSfx") ||
         !encoded.contains("unboundHitSfxGain") ||
         !encoded.contains("enableBoundHitSfx") ||
         !encoded.contains("boundHitSfxGain") ) {
        XERROR("Hit sound group controls did not round trip");
        return false;
    }
    return true;
}

/// @brief 验证旧配置缺少分组控制字段时保持全部启用和单位增益。
/// @return 四个新增字段均恢复兼容默认值时返回 true。
bool testLegacyHitSoundGroupDefaults()
{
    const nlohmann::json legacy{
        { "enableHitSfx", true },
    };
    MMM::Config::SfxConfig restored;
    from_json(legacy, restored);
    if ( !restored.enableUnboundHitSfx || !restored.enableBoundHitSfx ||
         restored.unboundHitSfxGain != 1.0F ||
         restored.boundHitSfxGain != 1.0F ) {
        XERROR("Legacy hit sound group controls did not use safe defaults");
        return false;
    }
    return true;
}

/// @brief 验证持久化增益不会越过约定的线性范围。
/// @return 低于零和高于二的增益分别收敛到边界时返回 true。
bool testHitSoundGroupGainBounds()
{
    const nlohmann::json encoded{
        { "unboundHitSfxGain", -0.5F },
        { "boundHitSfxGain", 2.5F },
    };
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);
    if ( restored.unboundHitSfxGain != 0.0F ||
         restored.boundHitSfxGain != 2.0F ) {
        XERROR("Hit sound group gains escaped the supported range");
        return false;
    }
    return true;
}

/// @brief 验证非有限持久化增益不会传播到实时音频路径。
/// @return NaN 和正无穷在写出和读取时均收敛为零则返回 true。
bool testNonFiniteHitSoundGroupGains()
{
    MMM::Config::SfxConfig source;
    source.unboundHitSfxGain = std::numeric_limits<float>::quiet_NaN();
    source.boundHitSfxGain   = std::numeric_limits<float>::infinity();
    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);
    if ( encoded.at("unboundHitSfxGain").get<float>() != 0.0F ||
         encoded.at("boundHitSfxGain").get<float>() != 0.0F ||
         restored.unboundHitSfxGain != 0.0F ||
         restored.boundHitSfxGain != 0.0F ) {
        XERROR("Non-finite hit sound group gains were not sanitized");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行打击音效设置序列化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testStereoHitEffectsRoundTrip() &&
                   testLegacyDirectionalFlickMigration() &&
                   testNewStereoSettingTakesPriority() &&
                   testHitSoundGroupControlsRoundTrip() &&
                   testLegacyHitSoundGroupDefaults() &&
                   testHitSoundGroupGainBounds() &&
                   testNonFiniteHitSoundGroupGains()
               ? 0
               : 1;
}
