#include "config/CreatorIdentity.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

/// @brief 验证协作稳定标识生成、大小写规范化和非法值拒绝规则。
/// @return 连续生成值不同且均能稳定规范化时返回 true。
[[nodiscard]] bool testCollaborationStableIdentity()
{
    using MMM::Config::COLLABORATION_STABLE_ID_CHARACTERS;
    using MMM::Config::isCollaborationStableIdValid;
    using MMM::Config::makeCollaborationStableId;
    using MMM::Config::normalizeCollaborationStableId;

    const auto        first  = makeCollaborationStableId();
    const auto        second = makeCollaborationStableId();
    const std::string upper  = "0123456789ABCDEF0123456789ABCDEF";
    if ( first.size() != COLLABORATION_STABLE_ID_CHARACTERS ||
         first == second || !isCollaborationStableIdValid(first) ||
         normalizeCollaborationStableId(upper) !=
             "0123456789abcdef0123456789abcdef" ||
         isCollaborationStableIdValid("0123") ||
         isCollaborationStableIdValid("0123456789abcdef0123456789abcdeg") ) {
        XERROR("Collaboration stable identity boundary was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证应用配置重置和磁盘往返都不会更换协作者稳定标识。
/// @return 同一份用户配置始终恢复同一个 ParticipantId 时返回 true。
[[nodiscard]] bool testCollaborationStableIdentityPersistence()
{
    auto&      appConfig = MMM::Config::AppConfig::instance();
    const auto identity  = appConfig.getCollaborationParticipantId();
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() /
        ("mmm-collaboration-identity-" + std::to_string(suffix) + ".json");
    if ( identity.empty() || !appConfig.save(path) ) return false;

    appConfig.reset();
    if ( appConfig.getCollaborationParticipantId() != identity ||
         !appConfig.load(path) ||
         appConfig.getCollaborationParticipantId() != identity ) {
        std::error_code error;
        std::filesystem::remove(path, error);
        XERROR("Collaboration stable identity did not survive config reload");
        return false;
    }

    std::ifstream   input(path);
    const auto      serialized = nlohmann::json::parse(input, nullptr, false);
    std::error_code error;
    std::filesystem::remove(path, error);
    if ( serialized.is_discarded() || !serialized.is_object() ||
         serialized.value("collaborationParticipantId", std::string{}) !=
             identity ) {
        XERROR("Collaboration stable identity was not persisted");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行默认 Creator 配置与身份约束测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testCreatorNormalization() && testCreatorConfigRoundTrip() &&
                   testCollaborationStableIdentity() &&
                   testCollaborationStableIdentityPersistence()
               ? 0
               : 1;
}
