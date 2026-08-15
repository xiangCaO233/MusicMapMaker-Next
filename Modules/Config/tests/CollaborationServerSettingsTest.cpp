#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 验证协作服务器地址、端口和 TLS 开关能够完整往返。
/// @return 三个字段保持原值且写入独立对象时返回 true。
bool testRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.collaborationServer.address       = "collaboration.example.com";
    source.collaborationServer.signalingPort = 9443;
    source.collaborationServer.useTls        = false;

    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);

    const auto& server = restored.collaborationServer;
    if ( server.address != source.collaborationServer.address ||
         server.signalingPort != source.collaborationServer.signalingPort ||
         server.useTls != source.collaborationServer.useTls ||
         !encoded.contains("collaborationServer") ) {
        XERROR("Collaboration server settings did not round trip");
        return false;
    }
    return true;
}

/// @brief 验证旧配置缺少协作服务器字段时使用公开服务默认值。
/// @return 默认地址、443 端口和 TLS 均恢复时返回 true。
bool testLegacyDefaults()
{
    MMM::Config::EditorSettings restored;
    from_json(nlohmann::json::object(), restored);
    const auto& server = restored.collaborationServer;
    if ( server.address != "xiang233.top" || server.signalingPort != 443 ||
         !server.useTls ) {
        XERROR("Legacy settings did not use collaboration server defaults");
        return false;
    }
    return true;
}

/// @brief 验证非法持久化字段不会进入后续网络连接配置。
/// @return 空地址、越界端口和错误类型均回退到安全默认值时返回 true。
bool testInvalidValuesUseDefaults()
{
    const nlohmann::json        encoded{ { "collaborationServer",
                                           { { "address", "" },
                                             { "signalingPort", 70000U },
                                             { "useTls", "yes" } } } };
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);
    const auto& server = restored.collaborationServer;
    if ( server.address != "xiang233.top" || server.signalingPort != 443 ||
         !server.useTls ) {
        XERROR("Invalid collaboration server settings escaped validation");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行协作服务器配置持久化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testRoundTrip() && testLegacyDefaults() &&
                   testInvalidValuesUseDefaults()
               ? 0
               : 1;
}
