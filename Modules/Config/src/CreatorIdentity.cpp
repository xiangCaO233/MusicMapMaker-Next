#include "config/CreatorIdentity.h"

#include <algorithm>

namespace MMM::Config
{
namespace
{
/// @brief 判断字符是否为 Creator 首尾允许裁剪的 ASCII 空白。
/// @param character 待判断的无符号字符。
/// @return 属于 ASCII 空白时返回 true。
[[nodiscard]] bool isAsciiWhitespace(unsigned char character)
{
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

/// @brief 判断字符是否为不允许出现在单行 Creator 中的 ASCII 控制字符。
/// @param character 待判断的无符号字符。
/// @return 属于控制字符时返回 true。
[[nodiscard]] bool isAsciiControl(unsigned char character)
{
    return character < 0x20U || character == 0x7FU;
}
}  // namespace

std::string normalizeCreatorIdentity(std::string_view creator)
{
    while ( !creator.empty() &&
            isAsciiWhitespace(static_cast<unsigned char>(creator.front())) ) {
        creator.remove_prefix(1);
    }
    while ( !creator.empty() &&
            isAsciiWhitespace(static_cast<unsigned char>(creator.back())) ) {
        creator.remove_suffix(1);
    }

    if ( creator.empty() || creator.size() > MAX_CREATOR_IDENTITY_BYTES ||
         std::any_of(creator.begin(), creator.end(), [](char character) {
             return isAsciiControl(static_cast<unsigned char>(character));
         }) ) {
        return {};
    }
    return std::string(creator);
}

bool isCreatorIdentityValid(std::string_view creator)
{
    return !normalizeCreatorIdentity(creator).empty();
}
}  // namespace MMM::Config
