#include "config/EditorSettings.h"
#include <nlohmann/json.hpp>

/// @brief 验证新偏好可持久化，旧配置或异常枚举保持原有判定线行为。
int main()
{
    // 保留完整 EditorSettings JSON
    // 反序列化入口，避免只测枚举而漏掉字段没有接入配置。
    using namespace MMM::Config;
    // 升级前保存的配置没有新字段，不能意外改变用户现有粘贴方式。
    const auto legacy = nlohmann::json::object().get<EditorSettings>();
    // 空对象模拟老版本省略字段；判断默认值之前不写入任何现代设置。
    if ( legacy.pastePosition != PastePosition::JudgmentLine ) return 1;
    // 覆盖两种合法值，确保其中一个没有因意外落到默认值而漏报。
    for ( auto position :
          { PastePosition::JudgmentLine, PastePosition::MouseCursor } ) {
        // 每次构造独立配置，测试值不能继承上一轮留下的字段。
        EditorSettings settings;
        settings.pastePosition      = position;
        settings.copyPasteTimeBasis = CopyPasteTimeBasis::Beat;
        // 以公开 ADL 转换走真实配置路径，不手工组装特定枚举的中间表示。
        const nlohmann::json json = settings;
        // 往返结果与原始值对照，保护配置保存后短时间重载的稳定性。
        const auto restored = json.get<EditorSettings>();
        // 锚点位置与批内相对时间基准分别保存，不能互相覆盖。
        if ( restored.pastePosition != position ||
             restored.copyPasteTimeBasis != CopyPasteTimeBasis::Beat )
            return 2;
    }
    // 未知字符串覆盖新版本降级；其他 JSON 类型覆盖手工编辑造成的结构错误。
    for ( const auto& value : { nlohmann::json("future"),
                                nlohmann::json(42),
                                nlohmann::json(false),
                                nlohmann::json(nullptr) } ) {
        // 直接转换枚举可区分字段级兜底与外层设置对象的类型检查。
        if ( value.get<PastePosition>() != PastePosition::JudgmentLine )
            return 3;
    }
    // 成功退出需要旧配置、两个当前取值和异常类型均符合兼容约定。
    return 0;
}
