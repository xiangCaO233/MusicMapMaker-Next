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
    // 只采用协议定义的 ASCII 集，不受进程区域设置影响。
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

/// @brief 判断字符是否为不允许出现在单行 Creator 中的 ASCII 控制字符。
/// @param character 待判断的无符号字符。
/// @return 属于控制字符时返回 true。
[[nodiscard]] bool isAsciiControl(unsigned char character)
{
    // DEL 与 C0 控制区都不能进入单行身份，UTF-8 高位字节保持原样。
    return character < 0x20U || character == 0x7FU;
}

/// @brief 将 64 位数值追加为固定宽度十六进制文本。
/// @param output 接收文本。
/// @param value 待编码数值。
void appendHex64(std::string& output, std::uint64_t value)
{
    // 固定 16 字节缓冲保证前导零被保留，两个分段始终组成 128 位文本。
    std::array<char, 16>       buffer{};
    constexpr std::string_view DIGITS = "0123456789abcdef";
    // 从低位向高位取半字节，但倒序写入得到通常阅读顺序。
    for ( std::size_t index = buffer.size(); index > 0U; --index ) {
        buffer[index - 1U] = DIGITS[value & 0xFU];
        value >>= 4U;
    }
    // 一次追加完整缓冲，避免把末尾零误当作 C 字符串终止符。
    output.append(buffer.data(), buffer.size());
}
}  // namespace

/// @brief 裁剪 Creator 首尾空白并拒绝控制字符、空值和超长文本。
/// @param creator 来自用户输入、环境覆盖或配置文件的 UTF-8 文本。
/// @return 合法时返回裁剪后的身份，否则返回空字符串。
std::string normalizeCreatorIdentity(std::string_view creator)
{
    // string_view 只移动视图边界，不为首尾裁剪分配中间字符串。
    while ( !creator.empty() &&
            isAsciiWhitespace(static_cast<unsigned char>(creator.front())) ) {
        creator.remove_prefix(1);
    }
    // 尾部独立处理，确保全空白输入在第二轮前已经安全变为空视图。
    while ( !creator.empty() &&
            isAsciiWhitespace(static_cast<unsigned char>(creator.back())) ) {
        creator.remove_suffix(1);
    }

    // 长度按 UTF-8 字节计数，与网络和持久化协议的上限口径一致。
    if ( creator.empty() || creator.size() > MAX_CREATOR_IDENTITY_BYTES ||
         std::any_of(creator.begin(), creator.end(), [](char character) {
             // 转为 unsigned char 避免高位 UTF-8 字节被符号扩展为负值。
             return isAsciiControl(static_cast<unsigned char>(character));
         }) ) {
        return {};
    }
    // 通过所有校验后才构造拥有字符串，返回值不依赖调用方缓冲区。
    return std::string(creator);
}

/// @brief 判断 Creator 输入能否规范化为非空单行身份。
/// @param creator 待检查的 UTF-8 文本。
/// @return 满足裁剪、长度和控制字符约束时返回 true。
bool isCreatorIdentityValid(std::string_view creator)
{
    // 复用规范化入口，确保校验与真正存储时不会出现规则漂移。
    return !normalizeCreatorIdentity(creator).empty();
}

/// @brief 生成由两个固定宽度十六进制分段组成的协作稳定标识。
/// @return 长度固定为 32 的小写十六进制文本。
/// @warning 低频身份初始化路径：原子序号使用 relaxed，仅降低本进程碰撞概率。
std::string makeCollaborationStableId()
{
    // 墙上时钟提供跨启动差异，不作为安全随机源或可排序协议字段。
    const auto wallClock = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    // 单调时钟降低系统时间回拨时重复生成同一首段的可能。
    const auto monotonic = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    // 原子序号区分同一时钟刻度中的并发调用，不承担其他数据同步。
    const auto sequence = g_collaborationIdentitySequence.fetch_add(
        1U, std::memory_order_relaxed);
    // 进程内地址作为额外盐值，降低不同进程拥有相同时钟读数的碰撞概率。
    const auto processSalt = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&g_collaborationIdentitySequence));

    // 预留完整长度，两个十六进制分段追加期间不会触发重新分配。
    std::string identity;
    identity.reserve(COLLABORATION_STABLE_ID_CHARACTERS);
    appendHex64(identity, wallClock ^ processSalt);
    // 黄金比例常数扩散连续序号的低位变化，再与单调时钟混合。
    appendHex64(identity, monotonic ^ (sequence * 0x9E3779B97F4A7C15ULL));
    return identity;
}

/// @brief 校验协作稳定标识并统一为小写十六进制文本。
/// @param identity 来自配置或协议的候选标识。
/// @return 长度和字符均合法时返回规范化文本，否则返回空字符串。
std::string normalizeCollaborationStableId(std::string_view identity)
{
    // 固定字符数直接对应 128 位，不接受缩写或附加分隔符。
    if ( identity.size() != COLLABORATION_STABLE_ID_CHARACTERS ) return {};

    // 结果长度已知，预留空间避免逐字符规范化时扩容。
    std::string normalized;
    normalized.reserve(identity.size());
    for ( const char character : identity ) {
        // 数字和小写十六进制字符无需转换。
        if ( character >= '0' && character <= '9' ) {
            normalized.push_back(character);
        } else if ( character >= 'a' && character <= 'f' ) {
            normalized.push_back(character);
        } else if ( character >= 'A' && character <= 'F' ) {
            // 手工 ASCII 转换保证结果不依赖区域设置。
            normalized.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            // 任一非十六进制字符使完整标识失效，不返回部分结果。
            return {};
        }
    }
    return normalized;
}

/// @brief 判断文本能否规范化为协作稳定标识。
/// @param identity 待检查的候选文本。
/// @return 可转换为 32 位小写十六进制字符串时返回 true。
bool isCollaborationStableIdValid(std::string_view identity)
{
    // 与 Creator 校验相同，复用规范化函数保证单一规则来源。
    return !normalizeCollaborationStableId(identity).empty();
}
}  // namespace MMM::Config
