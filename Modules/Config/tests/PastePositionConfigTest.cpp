#include "config/EditorSettings.h"
#include <nlohmann/json.hpp>

/// @brief 验证新偏好可持久化，旧配置或异常枚举保持原有判定线行为。
int main()
{
    using namespace MMM::Config;
    // 升级前保存的配置没有新字段，不能意外改变用户现有粘贴方式。
    const auto legacy = nlohmann::json::object().get<EditorSettings>();
    if ( legacy.pastePosition != PastePosition::JudgmentLine ) return 1;
    for ( auto position :
          { PastePosition::JudgmentLine, PastePosition::MouseCursor } ) {
        EditorSettings settings;
        settings.pastePosition        = position;
        settings.copyPasteTimeBasis   = CopyPasteTimeBasis::Beat;
        const nlohmann::json json     = settings;
        const auto           restored = json.get<EditorSettings>();
        // 锚点位置与批内相对时间基准分别保存，不能互相覆盖。
        if ( restored.pastePosition != position ||
             restored.copyPasteTimeBasis != CopyPasteTimeBasis::Beat )
            return 2;
    }
    for ( const auto& value : { nlohmann::json("future"),
                                nlohmann::json(42),
                                nlohmann::json(false),
                                nlohmann::json(nullptr) } ) {
        if ( value.get<PastePosition>() != PastePosition::JudgmentLine )
            return 3;
    }
    return 0;
}
