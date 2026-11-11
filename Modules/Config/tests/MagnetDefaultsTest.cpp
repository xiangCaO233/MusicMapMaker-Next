/// @file MagnetDefaultsTest.cpp
/// @brief 检查移动端默认磁吸策略对配置读写兼容性的影响。
/// @details 不启动交互外壳，避免依赖用户当前已打开的磁铁弹层。
/// 两个开关使用不同组合，保证迁移只补缺失字段而不重置完整偏好。
#include "config/EditorPlatformDefaults.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include <initializer_list>
#include <nlohmann/json.hpp>

/// @brief 验证平台磁吸默认和旧配置迁移，不覆盖用户明确保存的开关。
/// @details 新配置与缺字段 JSON 应一致，旧单开关仍控制缺失的物件吸附。
/// 所有输入为内存 JSON，不读取真实配置或修改模拟器个人数据。
/// @return 默认策略、显式开关或迁移行为不一致时返回非零。
int main()
{
    using namespace MMM::Config;
    // 构造与读取是两个独立入口，首次安装和旧版缺字段都要采用同一平台策略。
    // 这里不手动设置初值，否则会掩盖成员初始化仍为旧默认的遗漏。
    const EditorSettings defaults;
    const auto missing = nlohmann::json::object().get<EditorSettings>();
    // 缺字段不能退回写死的 false，否则新安装与升级缺字段的结果会不同。
    // 平台函数只提供初始策略，JSON 中的开关在后面的用例中必须优先。
    if ( defaults.scrollSnap != defaultMagnetEnabled() ||
         defaults.objectPlacementSnap != defaults.scrollSnap ||
         missing.scrollSnap != defaults.scrollSnap ||
         missing.objectPlacementSnap != defaults.objectPlacementSnap ) {
        XERROR("Magnet platform defaults and missing-field defaults differ");
        return 1;
    }
    // 显式保存的四种组合都要原样恢复，两个开关不能被错误地绑定。
    // 即使安卓默认开启，用户关闭任一开关之后也不应被默认策略覆盖。
    // 外层复用两种旧字段取值，内层单独检查新格式的两开关独立性。
    for ( const bool scroll : { false, true } ) {
        for ( const bool placement : { false, true } ) {
            const nlohmann::json saved{ { "scrollSnap", scroll },
                                        { "objectPlacementSnap", placement } };
            const auto           loaded = saved.get<EditorSettings>();
            if ( loaded.scrollSnap != scroll ||
                 loaded.objectPlacementSnap != placement ) {
                XERROR("Explicit magnet preferences were overwritten");
                return 2;
            }
        }
        // 旧格式只有 scrollSnap 时仍继承它，不能把关闭的旧配置迁移成开启。
        // 同时覆盖旧配置显式开启，避免用整体回退掩盖迁移遗漏。
        const auto legacy =
            nlohmann::json{ { "scrollSnap", scroll } }.get<EditorSettings>();
        if ( legacy.scrollSnap != scroll ||
             legacy.objectPlacementSnap != scroll ) {
            XERROR("Legacy magnet preference migration changed");
            return 3;
        }
    }
    return 0;
}
