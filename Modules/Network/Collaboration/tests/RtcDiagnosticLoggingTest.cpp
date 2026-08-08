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
    using MMM::Network::Collaboration::Detail::redactRtcDiagnosticMessage;

    const std::string original =
        "servers=[turn:user:secret@xiang233.top:3478?transport=udp, "
        "TURNS://alice:token@xiang233.top:5349]";
    const std::string expected =
        "servers=[turn:[redacted]@xiang233.top:3478?transport=udp, "
        "TURNS://[redacted]@xiang233.top:5349]";
    if ( redactRtcDiagnosticMessage(original) != expected ||
         redactRtcDiagnosticMessage("stun:xiang233.top:3478") !=
             "stun:xiang233.top:3478" ||
         redactRtcDiagnosticMessage("turn:xiang233.top:3478") !=
             "turn:xiang233.top:3478" ) {
        XERROR("RTC diagnostic log TURN credential redaction was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证底层日志开关能够持久化且旧配置默认关闭。
/// @return 当前配置往返和旧配置缺省值均符合预期时返回 true。
[[nodiscard]] bool testRtcDiagnosticLoggingConfigRoundTrip()
{
    MMM::Config::EditorSettings settings;
    settings.rtcDiagnosticLogging = true;

    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( !encoded.value("rtcDiagnosticLogging", false) ||
         !restored.rtcDiagnosticLogging || legacy.rtcDiagnosticLogging ) {
        XERROR("RTC diagnostic logging config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证应用级 RTC 诊断开关不会写入或恢复为项目覆盖配置。
/// @return 项目 JSON 不包含开关且读取后保持关闭时返回 true。
[[nodiscard]] bool testRtcDiagnosticLoggingExcludedFromProjectSettings()
{
    MMM::ProjectSettings projectSettings;
    projectSettings.m_editorOverride.emplace();
    projectSettings.m_editorOverride->rtcDiagnosticLogging = true;

    const nlohmann::json encoded        = projectSettings;
    const auto&          editorOverride = encoded.at("m_editorOverride");
    const auto           restored       = encoded.get<MMM::ProjectSettings>();
    if ( editorOverride.contains("rtcDiagnosticLogging") ||
         !restored.m_editorOverride ||
         restored.m_editorOverride->rtcDiagnosticLogging ) {
        XERROR("Project settings retained the global RTC diagnostic switch");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 WebRTC/ICE 诊断日志脱敏与配置持久化测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testTurnCredentialRedaction() &&
                   testRtcDiagnosticLoggingConfigRoundTrip() &&
                   testRtcDiagnosticLoggingExcludedFromProjectSettings()
               ? 0
               : 1;
}
