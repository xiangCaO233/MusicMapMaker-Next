/// @file MagnetDefaultsTest.cpp
/// @brief 检查桌面与移动共用的默认磁吸策略及配置读写兼容性。
/// @details 不启动交互外壳，避免依赖用户当前已打开的磁铁弹层。
/// 两个开关使用不同组合，保证迁移只补缺失字段而不重置完整偏好。
#include "config/EditorPlatformDefaults.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include <initializer_list>
#include <nlohmann/json.hpp>

/// @brief 验证统一磁吸默认和旧配置迁移，不覆盖用户明确保存的开关。
/// @details 新配置与缺字段 JSON 应一致，旧单开关仍控制缺失的物件吸附。
/// 所有输入为内存 JSON，不读取真实配置或修改模拟器个人数据。
/// @return 默认策略、显式开关或迁移行为不一致时返回非零。
int main()
{
    using namespace MMM::Config;
    // 构造与读取是两个独立入口，桌面和安卓首次安装均采用开启策略。
    // 这里不手动设置初值，否则会掩盖成员初始化仍为旧默认的遗漏。
    const EditorSettings defaults;
    const auto missing = nlohmann::json::object().get<EditorSettings>();
    // 缺字段不能退回写死的 false，否则新安装与升级缺字段的结果会不同。
    // 直接断言开启，不能只与同一个错误的默认函数比较而漏掉桌面回归。
    // JSON 中的显式开关在后面的用例中必须优先。
    if ( !defaultMagnetEnabled() || !defaults.scrollSnap ||
         defaults.objectPlacementSnap != defaults.scrollSnap ||
         missing.scrollSnap != defaults.scrollSnap ||
         missing.objectPlacementSnap != defaults.objectPlacementSnap ) {
        XERROR("Magnet defaults must be enabled on desktop and mobile");
        return 1;
    }
    // 显式保存的四种组合都要原样恢复，两个开关不能被错误地绑定。
    // 即使首次默认开启，用户关闭任一开关之后也不应被默认策略覆盖。
    // 外层复用两种旧字段取值，内层单独检查新格式的两开关独立性。
    for ( const bool scroll : { false, true } ) {
        for ( const bool placement : { false, true } ) {
            const nlohmann::json saved{ { "scrollSnap", scroll },
                                        { "objectPlacementSnap", placement } };
            const auto           loaded = saved.get<EditorSettings>();
            // 软件配置保存后再读入，显式关闭不能在序列化往返时重新变为开启。
            const auto restored = nlohmann::json(loaded).get<EditorSettings>();
            if ( restored.scrollSnap != scroll ||
                 restored.objectPlacementSnap != placement ||
                 loaded.scrollSnap != scroll ||
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
