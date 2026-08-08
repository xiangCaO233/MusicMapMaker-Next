#include "config/CreatorIdentity.h"
#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
/// @brief 验证 Creator 首尾空白裁剪及非法值拒绝规则。
/// @return 规范化边界符合联机身份约束时返回 true。
[[nodiscard]] bool testCreatorNormalization()
{
    using MMM::Config::MAX_CREATOR_IDENTITY_BYTES;
    using MMM::Config::isCreatorIdentityValid;
    using MMM::Config::normalizeCreatorIdentity;

    if ( normalizeCreatorIdentity("  高高的翔 \t") != "高高的翔" ||
         isCreatorIdentityValid(" \r\n") ||
         isCreatorIdentityValid("invalid\ncreator") ||
         !isCreatorIdentityValid(
             std::string(MAX_CREATOR_IDENTITY_BYTES, 'a')) ||
         isCreatorIdentityValid(
             std::string(MAX_CREATOR_IDENTITY_BYTES + 1, 'a')) ) {
        XERROR("Creator identity normalization boundary was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证默认 Creator 持久化并兼容缺失字段的旧配置。
/// @return 当前值规范化往返且旧配置默认未设置时返回 true。
[[nodiscard]] bool testCreatorConfigRoundTrip()
{
    MMM::Config::EditorSettings settings;
    settings.defaultCreator = "  Mapper  ";

    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( encoded.value("defaultCreator", std::string()) != "Mapper" ||
         restored.defaultCreator != "Mapper" ||
         !legacy.defaultCreator.empty() ) {
        XERROR("Default Creator config did not preserve compatibility");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行默认 Creator 配置与身份约束测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testCreatorNormalization() && testCreatorConfigRoundTrip() ? 0 : 1;
}
