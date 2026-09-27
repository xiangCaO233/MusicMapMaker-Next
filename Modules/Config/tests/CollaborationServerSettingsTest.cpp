#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 验证协作服务器地址、端口和 TLS 开关能够完整往返。
/// @return 三个字段保持原值且写入独立对象时返回 true。
/// @details 使用非默认值确保每个字段确实经过 JSON 序列化。
bool testRoundTrip()
{
    // 使用非默认地址、端口和 TLS 值，避免默认值掩盖字段遗漏。
    MMM::Config::EditorSettings source;
    source.collaborationServer.address       = "collaboration.example.com";
    source.collaborationServer.signalingPort = 9443;
    source.collaborationServer.useTls        = false;

    // 显式调用 ADL 序列化入口，分别验证写入对象和恢复对象。
    nlohmann::json encoded;
    to_json(encoded, source);
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);

    // 引用只在 restored 生命周期内使用，便于保持断言表达紧凑。
    const auto& server = restored.collaborationServer;
    // 除字段值外还检查嵌套对象存在，防止数据被错误写到顶层。
    if ( server.address != source.collaborationServer.address ||
         server.signalingPort != source.collaborationServer.signalingPort ||
         server.useTls != source.collaborationServer.useTls ||
         !encoded.contains("collaborationServer") ) {
        XERROR("Collaboration server settings did not round trip");
        return false;
    }
    // 全部自定义字段精确往返才算成功。
    return true;
}

/// @brief 验证旧配置缺少协作服务器字段时使用公开服务默认值。
/// @return 默认地址、443 端口和 TLS 均恢复时返回 true。
/// @details 空 JSON 对象代表该嵌套字段尚未加入的历史配置。
bool testLegacyDefaults()
{
    // 空对象模拟协作服务器配置尚未加入时保存的旧版用户文件。
    MMM::Config::EditorSettings restored;
    from_json(nlohmann::json::object(), restored);
    // 默认服务必须同时恢复地址、标准 HTTPS 端口和 TLS 开关。
    const auto& server = restored.collaborationServer;
    if ( server.address != "xiang233.top" || server.signalingPort != 443 ||
         !server.useTls ) {
        XERROR("Legacy settings did not use collaboration server defaults");
        return false;
    }
    // 兼容路径不应要求旧配置先经过迁移写回。
    return true;
}

/// @brief 验证非法持久化字段不会进入后续网络连接配置。
/// @return 空地址、越界端口和错误类型均回退到安全默认值时返回 true。
/// @details 三类错误在同一个嵌套对象中组合，验证逐字段防御解析。
bool testInvalidValuesUseDefaults()
{
    // 三个字段分别使用空文本、超出 uint16 范围和错误 JSON 类型。
    const nlohmann::json encoded{ { "collaborationServer",
                                    { { "address", "" },
                                      { "signalingPort", 70000U },
                                      { "useTls", "yes" } } } };
    // 输入对象保留原始错误类型，from_json 负责执行严格类型校验。
    // 从默认构造对象开始，解析器应保留安全默认值而不是部分接受输入。
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);
    const auto& server = restored.collaborationServer;
    // 非法组整体回退公开服务，不能把错误地址与默认端口混合进连接参数。
    if ( server.address != "xiang233.top" || server.signalingPort != 443 ||
         !server.useTls ) {
        XERROR("Invalid collaboration server settings escaped validation");
        return false;
    }
    // 成功表示所有无效字段都被限制在配置解析边界。
    return true;
}

}  // namespace

/// @brief 运行协作服务器配置持久化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 失败日志由具体子测试输出，main 只负责聚合 CTest 返回码。
    // 先验证当前格式，再验证缺失字段和非法字段两类兼容输入。
    // 短路失败时对应子测试已经输出明确日志，不再重复打印。
    return testRoundTrip() && testLegacyDefaults() &&
                   testInvalidValuesUseDefaults()
               ? 0
               : 1;
}
