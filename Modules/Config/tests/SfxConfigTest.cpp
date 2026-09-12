#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace
{

/// @brief 验证新版立体打击音效开关使用明确的新字段持久化。
/// @return 新字段往返且不再写出旧字段时返回 true。
/// @details false 输入覆盖默认开启值，并检查旧键不会继续污染新配置。
bool testStereoHitEffectsRoundTrip()
{
    // 使用 false 覆盖默认开启值，证明新字段确实控制恢复结果。
    MMM::Config::SfxConfig source;
    source.enableStereoHitEffects = false;

    // 显式 ADL 调用同时检查写出键集合和读回值。
    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);

    // 新格式必须写 enableStereoHitEffects，且不再继续产生旧 Flick 字段。
    if ( restored.enableStereoHitEffects ||
         !encoded.contains("enableStereoHitEffects") ||
         encoded.contains("enableDirectionalFlickChannels") ) {
        XERROR("Stereo hit effect setting did not use the new JSON field");
        return false;
    }
    // false 成功往返排除默认值偶然通过的情况。
    return true;
}

/// @brief 验证旧版 Flick 定向声道开关能够迁移到立体打击音效开关。
/// @return 旧字段的关闭状态被保留时返回 true。
/// @details 只提供旧键，确保兼容分支不依赖新字段同时存在。
bool testLegacyDirectionalFlickMigration()
{
    // 历史配置只有旧字段，解析器需把其语义迁移到统一立体声开关。
    const nlohmann::json legacy{
        { "enableDirectionalFlickChannels", false },
    };
    // 默认对象原本开启，恢复为 false 才能证明迁移读取生效。
    MMM::Config::SfxConfig restored;
    from_json(legacy, restored);
    // 旧 false 值必须覆盖新成员的默认 true。
    if ( restored.enableStereoHitEffects ) {
        XERROR("Legacy directional Flick setting was not migrated");
        return false;
    }
    // 兼容读取不要求先修改或重新保存 JSON 对象。
    return true;
}

/// @brief 验证新版字段存在时优先于旧版兼容字段。
/// @return 新字段的开启状态生效时返回 true。
/// @details 新旧键使用相反值，直接证明字段优先级。
bool testNewStereoSettingTakesPriority()
{
    // 新旧字段给出相反值，明确验证优先级而非单字段解析。
    const nlohmann::json mixed{
        { "enableStereoHitEffects", true },
        { "enableDirectionalFlickChannels", false },
    };
    // 恢复结果必须采用新字段 true，旧 false 仅服务缺失新字段的配置。
    MMM::Config::SfxConfig restored;
    from_json(mixed, restored);
    // mixed 对象保留旧键只为兼容测试，不应影响新键读取。
    if ( !restored.enableStereoHitEffects ) {
        XERROR("New stereo hit effect setting did not take priority");
        return false;
    }
    // 优先级稳定可避免升级后用户设置被旧残留字段反向覆盖。
    return true;
}

/// @brief 验证绑定与未绑定打击音效控制可以完整持久化。
/// @return 四个字段写出并按原值恢复时返回 true。
/// @details 两组开关均关闭，增益则分别覆盖低于和高于单位值的合法范围。
bool testHitSoundGroupControlsRoundTrip()
{
    // 两组开关均设为 false，增益使用不同非默认值以检测字段串位。
    MMM::Config::SfxConfig source;
    source.enableUnboundHitSfx = false;
    source.unboundHitSfxGain   = 0.35F;
    source.enableBoundHitSfx   = false;
    source.boundHitSfxGain     = 1.75F;

    // 当前格式应独立写出绑定和未绑定两组控制，而非合并旧总开关。
    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);

    // 同时检查恢复值和四个 JSON 键，防止只在内存默认值上偶然通过。
    if ( restored.enableUnboundHitSfx || restored.enableBoundHitSfx ||
         restored.unboundHitSfxGain != source.unboundHitSfxGain ||
         restored.boundHitSfxGain != source.boundHitSfxGain ||
         !encoded.contains("enableUnboundHitSfx") ||
         !encoded.contains("unboundHitSfxGain") ||
         !encoded.contains("enableBoundHitSfx") ||
         !encoded.contains("boundHitSfxGain") ) {
        // 键缺失和恢复值不符都说明新分组契约没有完整往返。
        XERROR("Hit sound group controls did not round trip");
        return false;
    }
    // 线性增益允许高于一，1.75 必须原样保存而不是提前钳制到单位值。
    return true;
}

/// @brief 验证旧配置缺少分组控制字段时保持全部启用和单位增益。
/// @return 四个新增字段均恢复兼容默认值时返回 true。
/// @details 保留旧总开关但省略新字段，模拟升级前用户文件。
bool testLegacyHitSoundGroupDefaults()
{
    // 旧配置仅含总开关，新增分组字段缺失时都应采用兼容默认值。
    const nlohmann::json legacy{
        { "enableHitSfx", true },
    };
    // 恢复对象从类型默认值开始，from_json 只覆盖实际存在的旧字段。
    MMM::Config::SfxConfig restored;
    from_json(legacy, restored);
    // 缺失新增字段时由成员默认值恢复两组单位增益。
    if ( !restored.enableUnboundHitSfx || !restored.enableBoundHitSfx ||
         restored.unboundHitSfxGain != 1.0F ||
         restored.boundHitSfxGain != 1.0F ) {
        XERROR("Legacy hit sound group controls did not use safe defaults");
        return false;
    }
    // 两组均启用且增益为一可保持升级前的听感和行为。
    return true;
}

/// @brief 验证持久化增益不会越过约定的线性范围。
/// @return 低于零和高于二的增益分别收敛到边界时返回 true。
/// @details 两个分组分别覆盖下界和上界，无需重复构造四种对象。
bool testHitSoundGroupGainBounds()
{
    // 输入分别越过线性增益的下界和上界，覆盖两侧钳制。
    const nlohmann::json encoded{
        { "unboundHitSfxGain", -0.5F },
        { "boundHitSfxGain", 2.5F },
    };
    // JSON 数值本身不修改，安全范围在配置恢复边界实施。
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);
    // 精确边界比较确认解析器执行钳制而非简单回退单位值。
    if ( restored.unboundHitSfxGain != 0.0F ||
         restored.boundHitSfxGain != 2.0F ) {
        XERROR("Hit sound group gains escaped the supported range");
        return false;
    }
    // 恢复结果落在 0 到 2，后续实时音频路径无需再次处理越界配置。
    return true;
}

/// @brief 验证非有限持久化增益不会传播到实时音频路径。
/// @return NaN 和正无穷在写出和读取时均收敛为零则返回 true。
/// @details 同时经过 to_json 与 from_json，覆盖配置输出和输入两道边界。
bool testNonFiniteHitSoundGroupGains()
{
    // 构造 NaN 与正无穷覆盖无法进入有效 JSON 数值域的两类输入。
    MMM::Config::SfxConfig source;
    source.unboundHitSfxGain = std::numeric_limits<float>::quiet_NaN();
    source.boundHitSfxGain   = std::numeric_limits<float>::infinity();
    // 两个特殊值分别覆盖安静 NaN 与无穷，均不得进入 JSON 数字结果。
    // 先验证写出阶段清洗，再验证读回阶段仍保持安全零值。
    nlohmann::json encoded;
    to_json(encoded, source);
    // 再从已经清洗的 JSON 恢复，模拟保存后下次启动的实际路径。
    MMM::Config::SfxConfig restored;
    from_json(encoded, restored);
    // 非有限值若传播到混音乘法会污染整帧样本，因此四项都必须为零。
    if ( encoded.at("unboundHitSfxGain").get<float>() != 0.0F ||
         encoded.at("boundHitSfxGain").get<float>() != 0.0F ||
         restored.unboundHitSfxGain != 0.0F ||
         restored.boundHitSfxGain != 0.0F ) {
        XERROR("Non-finite hit sound group gains were not sanitized");
        return false;
    }
    // 写入和读取双重防御通过后才确认配置不会污染实时音频状态。
    return true;
}

}  // namespace

/// @brief 运行打击音效设置序列化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 先检查字段迁移，再覆盖分组控制、范围和非有限数值防御。
    // 各子测试日志提供具体失败类别，main 只返回聚合状态。
    return testStereoHitEffectsRoundTrip() &&
                   testLegacyDirectionalFlickMigration() &&
                   testNewStereoSettingTakesPriority() &&
                   testHitSoundGroupControlsRoundTrip() &&
                   testLegacyHitSoundGroupDefaults() &&
                   testHitSoundGroupGainBounds() &&
                   // 非有限值场景放在最后，验证最严格的数值清洗边界。
                   testNonFiniteHitSoundGroupGains()
               ? 0
               : 1;
}
