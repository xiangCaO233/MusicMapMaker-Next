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
/// @details 同时覆盖中文 UTF-8、全空白、嵌入换行和最大字节边界。
[[nodiscard]] bool testCreatorNormalization()
{
    // 使用公开常量构造边界字符串，测试不会复制实现中的具体上限数值。
    using MMM::Config::MAX_CREATOR_IDENTITY_BYTES;
    using MMM::Config::isCreatorIdentityValid;
    // 校验入口与规范化入口在同一组表达式中交叉验证。
    using MMM::Config::normalizeCreatorIdentity;

    // 同一条件组覆盖多字节中文保留、ASCII 空白裁剪和控制字符拒绝。
    if ( normalizeCreatorIdentity("  高高的翔 \t") != "高高的翔" ||
         isCreatorIdentityValid(" \r\n") ||
         isCreatorIdentityValid("invalid\ncreator") ||
         !isCreatorIdentityValid(
             std::string(MAX_CREATOR_IDENTITY_BYTES, 'a')) ||
         isCreatorIdentityValid(
             std::string(MAX_CREATOR_IDENTITY_BYTES + 1, 'a')) ) {
        // 错误日志聚合到 Creator 规则，返回值仍允许 main 继续组合其他用例。
        XERROR("Creator identity normalization boundary was incorrect");
        return false;
    }
    // 没有错误分支命中说明所有边界同时满足协议约束。
    return true;
}

/// @brief 验证默认 Creator 持久化并兼容缺失字段的旧配置。
/// @return 当前值规范化往返且旧配置默认未设置时返回 true。
/// @details 直接测试 EditorSettings 的 JSON 契约，不经过 AppConfig 文件层。
[[nodiscard]] bool testCreatorConfigRoundTrip()
{
    // 输入刻意包含首尾空格，验证序列化入口写出规范化值而非原文。
    MMM::Config::EditorSettings settings;
    settings.defaultCreator = "  Mapper  ";

    // 同时执行编码、当前格式解码和空旧对象解码三种兼容路径。
    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 新格式必须往返 Mapper，旧配置缺字段则保持未设置状态。
    if ( encoded.value("defaultCreator", std::string()) != "Mapper" ||
         restored.defaultCreator != "Mapper" ||
         !legacy.defaultCreator.empty() ) {
        XERROR("Default Creator config did not preserve compatibility");
        return false;
    }
    // 成功路径证明写出与读回使用相同规范化语义。
    return true;
}

/// @brief 验证协作稳定标识生成、大小写规范化和非法值拒绝规则。
/// @return 连续生成值不同且均能稳定规范化时返回 true。
/// @details 不假设生成值可排序或具有加密随机性，只验证公开格式契约。
[[nodiscard]] bool testCollaborationStableIdentity()
{
    // 所有断言引用公开长度和校验入口，避免测试依赖内部混合算法。
    using MMM::Config::COLLABORATION_STABLE_ID_CHARACTERS;
    using MMM::Config::isCollaborationStableIdValid;
    using MMM::Config::makeCollaborationStableId;
    // normalize 单独参与大小写断言，valid 则覆盖拒绝分支。
    using MMM::Config::normalizeCollaborationStableId;

    // 连续生成验证进程内序号参与混合，不能只依赖低分辨率时钟。
    const auto first  = makeCollaborationStableId();
    const auto second = makeCollaborationStableId();
    // 固定大写输入验证协议接受大小写但存储统一为小写。
    const std::string upper = "0123456789ABCDEF0123456789ABCDEF";
    // 短字符串与含 g 字符分别覆盖长度和字符集拒绝分支。
    if ( first.size() != COLLABORATION_STABLE_ID_CHARACTERS ||
         first == second || !isCollaborationStableIdValid(first) ||
         normalizeCollaborationStableId(upper) !=
             "0123456789abcdef0123456789abcdef" ||
         isCollaborationStableIdValid("0123") ||
         isCollaborationStableIdValid("0123456789abcdef0123456789abcdeg") ) {
        XERROR("Collaboration stable identity boundary was incorrect");
        return false;
    }
    // 两次生成均合法且不同，满足进程内低碰撞测试目标。
    return true;
}

/// @brief 验证应用配置重置和磁盘往返都不会更换协作者稳定标识。
/// @return 同一份用户配置始终恢复同一个 ParticipantId 时返回 true。
/// @warning 使用显式临时文件，不调用默认路径，避免修改个人配置。
[[nodiscard]] bool testCollaborationStableIdentityPersistence()
{
    // AppConfig 单例已经在构造时生成身份，本测试记录它作为稳定基准。
    auto& appConfig = MMM::Config::AppConfig::instance();
    // 基准身份来自当前测试进程，不硬编码生成算法的具体输出。
    const auto identity = appConfig.getCollaborationParticipantId();
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // 临时文件名按本进程时间区分，不触碰用户默认配置位置。
    const auto path =
        std::filesystem::temp_directory_path() /
        ("mmm-collaboration-identity-" + std::to_string(suffix) + ".json");
    // 首次保存验证身份非空，并建立后续磁盘加载输入。
    if ( identity.empty() || !appConfig.save(path) ) return false;

    // reset 只恢复编辑器设置，协作者稳定标识必须继续保持不变。
    appConfig.reset();
    // 显式路径 load 不应生成新身份，内存值和磁盘值都回到同一基准。
    if ( appConfig.getCollaborationParticipantId() != identity ||
         !appConfig.load(path) ||
         appConfig.getCollaborationParticipantId() != identity ) {
        // 失败分支也清理测试文件，避免连续运行读取上轮残留。
        std::error_code error;
        std::filesystem::remove(path, error);
        XERROR("Collaboration stable identity did not survive config reload");
        return false;
    }

    // 最后直接解析磁盘 JSON，避免只验证同一 AppConfig 读写实现的自洽性。
    std::ifstream input(path);
    // parse 的 allow_exceptions=false 让损坏文件以 discarded 值进入断言。
    const auto      serialized = nlohmann::json::parse(input, nullptr, false);
    std::error_code error;
    // 读取完成后立即清理临时文件，断言只使用已加载的 JSON 值。
    std::filesystem::remove(path, error);
    // 配置必须是对象且明确持久化原始稳定身份字段。
    if ( serialized.is_discarded() || !serialized.is_object() ||
         serialized.value("collaborationParticipantId", std::string{}) !=
             identity ) {
        XERROR("Collaboration stable identity was not persisted");
        return false;
    }
    // 内存往返和独立磁盘解析均通过后才确认稳定身份持久化契约。
    // 通过磁盘字段检查后，测试不再依赖 AppConfig 单例的当前编辑器设置。
    return true;
}
}  // namespace

/// @brief 运行默认 Creator 配置与身份约束测试。
/// @return 全部断言通过时返回 0。
int main()
{
    // 短路顺序从纯字符串规则到磁盘持久化，基础失败时避免无关文件操作。
    // 单个返回码交给 CTest，具体失败阶段由各子测试的日志标识。
    return testCreatorNormalization() && testCreatorConfigRoundTrip() &&
                   testCollaborationStableIdentity() &&
                   // 文件持久化放在最后，基础失败不会留下临时文件。
                   testCollaborationStableIdentityPersistence()
               ? 0
               : 1;
}
