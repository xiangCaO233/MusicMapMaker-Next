#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace MMM::Config
{
/// @brief Creator 身份在线协议允许的最大 UTF-8 字节数。
inline constexpr std::size_t MAX_CREATOR_IDENTITY_BYTES = 192;

/// @brief 规范化用于谱面作者和联机展示的 Creator 身份。
/// @param creator 用户输入或配置文件读取的 Creator。
/// @return 去除首尾 ASCII 空白后的身份；含控制字符、为空或超长时返回空。
[[nodiscard]] std::string normalizeCreatorIdentity(std::string_view creator);

/// @brief 判断 Creator 是否可以作为联机展示身份。
/// @param creator 待校验 Creator。
/// @return 规范化后非空时返回 true。
[[nodiscard]] bool isCreatorIdentityValid(std::string_view creator);
}  // namespace MMM::Config
