#include "RtcDiagnosticLoggingInternal.h"
#include "config/EditorSettings.h"
#include "mmm/project/ProjectSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{

/// @brief 验证 TURN URI 凭据脱敏且 STUN 和无凭据地址保持可诊断。
/// @return 所有 URI 变体均符合脱敏边界时返回 true。
[[nodiscard]] bool testTurnCredentialRedaction()
{
    // 测试直接调用纯文本脱敏帮助器，不启用全局 libdatachannel 回调。
    using MMM::Network::Collaboration::Detail::redactRtcDiagnosticMessage;

    // 同一日志同时包含小写 turn 与大写 TURNS，覆盖大小写无关协议匹配。
    const std::string original =
        "servers=[turn:user:secret@xiang233.top:3478?transport=udp, "
        "TURNS://alice:token@xiang233.top:5349]";
    // 只替换 user:credential 部分，保留主机、端口和查询参数用于诊断。
    // 方括号、逗号及 URI 大小写也必须保持，避免脱敏破坏日志上下文。
    const std::string expected =
        "servers=[turn:[redacted]@xiang233.top:3478?transport=udp, "
        "TURNS://[redacted]@xiang233.top:5349]";
    // STUN 和无用户信息的 TURN URI 不含秘密，应保持原文不变。
    if ( redactRtcDiagnosticMessage(original) != expected ||
         // STUN URI 没有 TURN 长期凭据语义，不能被过度脱敏。
         redactRtcDiagnosticMessage("stun:xiang233.top:3478") !=
             "stun:xiang233.top:3478" ||
         redactRtcDiagnosticMessage("turn:xiang233.top:3478") !=
             "turn:xiang233.top:3478" ) {
        XERROR("RTC diagnostic log TURN credential redaction was incorrect");
        return false;
    }
    // 三类 URI 均符合脱敏边界。
    return true;
}

/// @brief 验证底层日志开关能够持久化且旧配置默认关闭。
/// @return 当前配置往返和旧配置缺省值均符合预期时返回 true。
[[nodiscard]] bool testRtcDiagnosticLoggingConfigRoundTrip()
{
    // 新配置显式启用开关，验证序列化不会遗漏应用级字段。
    MMM::Config::EditorSettings settings;
    // 默认对象先由测试显式覆盖，排除“默认即为 true”造成的假阳性。
    settings.rtcDiagnosticLogging = true;

    // 先编码再按正式 ADL 入口恢复，覆盖双向配置契约。
    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空对象模拟字段尚不存在的旧版用户配置。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 当前 JSON 应保存 true，恢复后仍为 true，旧空 JSON 则必须默认关闭。
    if ( !encoded.value("rtcDiagnosticLogging", false) ||
         !restored.rtcDiagnosticLogging || legacy.rtcDiagnosticLogging ) {
        XERROR("RTC diagnostic logging config did not preserve compatibility");
        return false;
    }
    // 当前与旧版配置兼容性同时满足。
    return true;
}

/// @brief 验证应用级 RTC 诊断开关不会写入或恢复为项目覆盖配置。
/// @return 项目 JSON 不包含开关且读取后保持关闭时返回 true。
[[nodiscard]] bool testRtcDiagnosticLoggingExcludedFromProjectSettings()
{
    // 项目覆盖对象故意设置 true，验证序列化边界会主动排除全局开关。
    MMM::ProjectSettings projectSettings;
    // emplace 确保项目确实拥有编辑器覆盖对象，而不是测试缺失分支。
    projectSettings.m_editorOverride.emplace();
    projectSettings.m_editorOverride->rtcDiagnosticLogging = true;

    // 使用真实 ProjectSettings 编解码入口检查嵌套 editor override。
    const nlohmann::json encoded = projectSettings;
    // at 要求覆盖对象实际被序列化，避免 contains 在错误层级上静默返回 false。
    const auto& editorOverride = encoded.at("m_editorOverride");
    const auto  restored       = encoded.get<MMM::ProjectSettings>();
    // 项目 JSON 不得包含字段，恢复对象必须存在且采用安全默认 false。
    if ( editorOverride.contains("rtcDiagnosticLogging") ||
         !restored.m_editorOverride ||
         restored.m_editorOverride->rtcDiagnosticLogging ) {
        XERROR("Project settings retained the global RTC diagnostic switch");
        return false;
    }
    // 应用级诊断开关未泄漏到可共享项目配置。
    return true;
}

}  // namespace

/// @brief 运行 WebRTC/ICE 诊断日志脱敏与配置持久化测试。
/// @return 全部断言通过时返回 0。
int main()
{
    // 三组纯配置与文本检查短路执行，任一失败统一返回非零状态。
    // 测试不启用真实 RTC 日志回调，因此不会向进程输出第三方调试内容。
    return testTurnCredentialRedaction() &&
                   testRtcDiagnosticLoggingConfigRoundTrip() &&
                   testRtcDiagnosticLoggingExcludedFromProjectSettings()
               ? 0
               : 1;
}
