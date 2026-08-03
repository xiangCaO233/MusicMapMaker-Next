#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>

namespace MMM::UI::ProjectAudioToolSearch
{

/// @brief 计算可拖动搜索结果区域在当前窗口内的实际高度。
/// @param preferredHeight 用户上次拖动选择的高度；非正值表示使用默认五行。
/// @param rowHeight 单条搜索结果的高度。
/// @param resultCount 当前搜索结果数量。
/// @param verticalPadding 搜索结果子窗口的单侧垂直内边距。
/// @param availableHeight 当前光标到窗口内容底部的可用高度。
/// @param reservedHeight 分隔条、状态栏和主画布必须保留的总高度。
[[nodiscard]] inline float calculateResultPaneHeight(
    float preferredHeight, float rowHeight, std::size_t resultCount,
    float verticalPadding, float availableHeight, float reservedHeight)
{
    if ( resultCount == 0 ) return 0.0F;

    constexpr std::size_t DEFAULT_VISIBLE_ROWS = 5;
    const float paddingHeight = std::max(0.0F, verticalPadding) * 2.0F;
    const float safeRowHeight = std::max(1.0F, rowHeight);
    const float minimumHeight = safeRowHeight + paddingHeight;
    const float contentHeight =
        safeRowHeight * static_cast<float>(resultCount) + paddingHeight;
    const float availablePaneHeight =
        std::max(minimumHeight, availableHeight - reservedHeight);
    const float maximumHeight = std::min(contentHeight, availablePaneHeight);
    const float defaultHeight =
        safeRowHeight *
            static_cast<float>(std::min(DEFAULT_VISIBLE_ROWS, resultCount)) +
        paddingHeight;
    return std::clamp(preferredHeight > 0.0F ? preferredHeight : defaultHeight,
                      minimumHeight,
                      maximumHeight);
}

/// @brief 去除搜索词首尾的 ASCII 空白且不产生新字符串。
[[nodiscard]] inline std::string_view trimAsciiWhitespace(
    std::string_view value)
{
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    return value;
}

/// @brief 将 ASCII 字母折叠为小写；UTF-8 字节保持原值以支持精确 CJK 匹配。
[[nodiscard]] inline unsigned char foldAscii(unsigned char value)
{
    if ( value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z') ) {
        return static_cast<unsigned char>(
            value + static_cast<unsigned char>('a' - 'A'));
    }
    return value;
}

/// @brief 判断候选从指定位置起是否与搜索词进行 ASCII 不区分大小写匹配。
[[nodiscard]] inline bool matchesAt(std::string_view candidate,
                                    std::string_view query, std::size_t offset)
{
    if ( offset > candidate.size() ||
         query.size() > candidate.size() - offset ) {
        return false;
    }
    for ( std::size_t index = 0; index < query.size(); ++index ) {
        if ( foldAscii(static_cast<unsigned char>(candidate[offset + index])) !=
             foldAscii(static_cast<unsigned char>(query[index])) ) {
            return false;
        }
    }
    return true;
}

/// @brief 查找 ASCII 不区分大小写的连续子串位置。
[[nodiscard]] inline std::optional<std::size_t> findSubstring(
    std::string_view candidate, std::string_view query)
{
    if ( query.empty() ) return 0;
    if ( query.size() > candidate.size() ) return std::nullopt;
    const std::size_t finalOffset = candidate.size() - query.size();
    for ( std::size_t offset = 0; offset <= finalOffset; ++offset ) {
        if ( matchesAt(candidate, query, offset) ) return offset;
    }
    return std::nullopt;
}

/// @brief 对文件名或资源 ID 与搜索词的相似度评分。
/// @return 完全、前缀、子串或顺序模糊匹配的分数；不匹配时返回空。
/// @warning 只在搜索词或资源缓存变化时批量调用；函数自身不分配内存。
[[nodiscard]] inline std::optional<int> scoreCandidate(
    std::string_view candidate, std::string_view query)
{
    query = trimAsciiWhitespace(query);
    if ( candidate.empty() || query.empty() ) return std::nullopt;

    const auto substring = findSubstring(candidate, query);
    if ( substring ) {
        const int lengthPenalty = static_cast<int>(
            std::min<std::size_t>(candidate.size() - query.size(), 50'000));
        if ( *substring == 0 && candidate.size() == query.size() ) {
            return 400'000;
        }
        if ( *substring == 0 ) {
            return 300'000 - lengthPenalty;
        }
        const int positionPenalty =
            static_cast<int>(std::min<std::size_t>(*substring, 1'000)) * 32;
        return 200'000 - positionPenalty - lengthPenalty;
    }

    std::size_t queryIndex       = 0;
    std::size_t firstMatch       = candidate.size();
    std::size_t previousMatch    = 0;
    std::size_t gapCount         = 0;
    std::size_t consecutivePairs = 0;
    for ( std::size_t candidateIndex = 0;
          candidateIndex < candidate.size() && queryIndex < query.size();
          ++candidateIndex ) {
        if ( foldAscii(static_cast<unsigned char>(candidate[candidateIndex])) !=
             foldAscii(static_cast<unsigned char>(query[queryIndex])) ) {
            continue;
        }
        if ( queryIndex == 0 ) {
            firstMatch = candidateIndex;
        } else {
            const std::size_t distance = candidateIndex - previousMatch;
            if ( distance == 1 ) {
                ++consecutivePairs;
            } else {
                gapCount += distance - 1;
            }
        }
        previousMatch = candidateIndex;
        ++queryIndex;
    }
    if ( queryIndex != query.size() ) return std::nullopt;

    const int queryReward =
        static_cast<int>(std::min<std::size_t>(query.size(), 1'000)) * 96;
    const int consecutiveReward =
        static_cast<int>(std::min<std::size_t>(consecutivePairs, 1'000)) * 24;
    const int startPenalty =
        static_cast<int>(std::min<std::size_t>(firstMatch, 1'000)) * 16;
    const int gapPenalty =
        static_cast<int>(std::min<std::size_t>(gapCount, 1'000)) * 20;
    const int lengthPenalty = static_cast<int>(
        std::min<std::size_t>(candidate.size() - query.size(), 50'000));
    return 100'000 + queryReward + consecutiveReward - startPenalty -
           gapPenalty - lengthPenalty;
}

}  // namespace MMM::UI::ProjectAudioToolSearch
