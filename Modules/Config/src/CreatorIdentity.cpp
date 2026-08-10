#include "config/CreatorIdentity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace MMM::Config
{
namespace
{
/// @brief 进程内协作标识序号；只用于避免同一时钟刻度内碰撞。
std::atomic<std::uint64_t> g_collaborationIdentitySequence{ 0U };

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

/// @brief 将 64 位数值追加为固定宽度十六进制文本。
/// @param output 接收文本。
/// @param value 待编码数值。
void appendHex64(std::string& output, std::uint64_t value)
{
    std::array<char, 16>       buffer{};
    constexpr std::string_view DIGITS = "0123456789abcdef";
    for ( std::size_t index = buffer.size(); index > 0U; --index ) {
        buffer[index - 1U] = DIGITS[value & 0xFU];
        value >>= 4U;
    }
    output.append(buffer.data(), buffer.size());
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

std::string makeCollaborationStableId()
{
    const auto wallClock = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto monotonic = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence = g_collaborationIdentitySequence.fetch_add(
        1U, std::memory_order_relaxed);
    const auto processSalt = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&g_collaborationIdentitySequence));

    std::string identity;
    identity.reserve(COLLABORATION_STABLE_ID_CHARACTERS);
    appendHex64(identity, wallClock ^ processSalt);
    appendHex64(identity, monotonic ^ (sequence * 0x9E3779B97F4A7C15ULL));
    return identity;
}

std::string normalizeCollaborationStableId(std::string_view identity)
{
    if ( identity.size() != COLLABORATION_STABLE_ID_CHARACTERS ) return {};

    std::string normalized;
    normalized.reserve(identity.size());
    for ( const char character : identity ) {
        if ( character >= '0' && character <= '9' ) {
            normalized.push_back(character);
        } else if ( character >= 'a' && character <= 'f' ) {
            normalized.push_back(character);
        } else if ( character >= 'A' && character <= 'F' ) {
            normalized.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            return {};
        }
    }
    return normalized;
}

bool isCollaborationStableIdValid(std::string_view identity)
{
    return !normalizeCollaborationStableId(identity).empty();
}
}  // namespace MMM::Config
