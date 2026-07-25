#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

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

}  // namespace

/// @brief 运行立体打击音效设置兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testStereoHitEffectsRoundTrip() &&
                   testLegacyDirectionalFlickMigration() &&
                   testNewStereoSettingTakesPriority()
               ? 0
               : 1;
}
