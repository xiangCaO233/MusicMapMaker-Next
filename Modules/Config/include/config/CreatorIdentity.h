#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace MMM::Config
{
/// @brief Creator 身份在线协议允许的最大 UTF-8 字节数。
inline constexpr std::size_t MAX_CREATOR_IDENTITY_BYTES = 192;
/// @brief 协作稳定标识的固定十六进制字符数。
inline constexpr std::size_t COLLABORATION_STABLE_ID_CHARACTERS = 32;

/// @brief 规范化用于谱面作者和联机展示的 Creator 身份。
/// @param creator 用户输入或配置文件读取的 Creator。
/// @return 去除首尾 ASCII 空白后的身份；含控制字符、为空或超长时返回空。
[[nodiscard]] std::string normalizeCreatorIdentity(std::string_view creator);

/// @brief 判断 Creator 是否可以作为联机展示身份。
/// @param creator 待校验 Creator。
/// @return 规范化后非空时返回 true。
[[nodiscard]] bool isCreatorIdentityValid(std::string_view creator);

/// @brief 生成跨进程极低碰撞概率的协作稳定标识。
/// @return 由两个 64 位十六进制段组成的稳定标识。
/// @warning 仅允许在应用配置初始化或协作连接建立时低频调用；内部原子计数器
/// 使用 relaxed 顺序，只承担唯一编号而不参与线程同步。
[[nodiscard]] std::string makeCollaborationStableId();

/// @brief 规范化协作稳定标识。
/// @param identity 待校验的 128 位十六进制文本。
/// @return 合法时返回小写形式，长度或字符不合法时返回空字符串。
[[nodiscard]] std::string normalizeCollaborationStableId(
    std::string_view identity);

/// @brief 判断文本是否可作为协作稳定标识。
/// @param identity 待校验文本。
/// @return 可规范化为 128 位十六进制标识时返回 true。
[[nodiscard]] bool isCollaborationStableIdValid(std::string_view identity);
}  // namespace MMM::Config
