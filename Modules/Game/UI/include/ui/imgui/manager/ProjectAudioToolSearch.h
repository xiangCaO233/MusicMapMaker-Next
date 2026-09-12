#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>

namespace MMM::UI::ProjectAudioToolSearch
{

/// @file ProjectAudioToolSearch.h
/// @brief 项目音频工具使用的无分配搜索评分与结果面板高度策略。
/// @details 辅助函数全部内联且不访问文件系统，调用方先建立资源名缓存，再在
/// 查询变化时评分和排序；UTF-8 非 ASCII 字节按原值比较。

/// @brief 计算可拖动搜索结果区域在当前窗口内的实际高度。
/// @param preferredHeight 用户上次拖动选择的高度；非正值表示使用默认五行。
/// @param rowHeight 单条搜索结果的高度。
/// @param resultCount 当前搜索结果数量。
/// @param verticalPadding 搜索结果子窗口的单侧垂直内边距。
/// @param availableHeight 当前光标到窗口内容底部的可用高度。
/// @param reservedHeight 分隔条、状态栏和主画布必须保留的总高度。
/// @return 至少容纳一行、且不超过内容和可用空间上限的面板高度。
/// @note 没有结果时返回零，使调用方可以完全省略结果子窗口。
[[nodiscard]] inline float calculateResultPaneHeight(
    float preferredHeight, float rowHeight, std::size_t resultCount,
    float verticalPadding, float availableHeight, float reservedHeight)
{
    // 空结果不需要保留 padding 或默认行高。
    if ( resultCount == 0 ) return 0.0F;

    // 首次打开时默认展示五行，更多结果通过滚动访问。
    constexpr std::size_t DEFAULT_VISIBLE_ROWS = 5;
    // 负 padding 没有布局意义，按零处理后计算上下两侧总和。
    const float paddingHeight = std::max(0.0F, verticalPadding) * 2.0F;
    // 行高至少一个像素，防止 clamp 上下界退化。
    const float safeRowHeight = std::max(1.0F, rowHeight);
    // 最小高度始终可以显示一行及其内边距。
    const float minimumHeight = safeRowHeight + paddingHeight;
    // 内容上限由实际结果数量决定，不制造空白滚动区域。
    const float contentHeight =
        safeRowHeight * static_cast<float>(resultCount) + paddingHeight;
    // 为主画布和工具状态保留空间，但极窄窗口仍保留一行。
    const float availablePaneHeight =
        std::max(minimumHeight, availableHeight - reservedHeight);
    // 可拖动上限同时受内容高度与窗口空间约束。
    const float maximumHeight = std::min(contentHeight, availablePaneHeight);
    // 未保存用户高度时，以实际结果数和五行中较小者作为默认值。
    const float defaultHeight =
        safeRowHeight *
            static_cast<float>(std::min(DEFAULT_VISIBLE_ROWS, resultCount)) +
        paddingHeight;
    // 用户高度、默认高度最终都约束在同一安全区间。
    return std::clamp(preferredHeight > 0.0F ? preferredHeight : defaultHeight,
                      minimumHeight,
                      maximumHeight);
}

/// @brief 去除搜索词首尾的 ASCII 空白且不产生新字符串。
/// @param value 原始搜索词视图。
/// @return 指向原字符存储内部的裁切视图。
/// @warning 返回值不拥有字符数据，源字符串必须在评分期间保持有效。
[[nodiscard]] inline std::string_view trimAsciiWhitespace(
    std::string_view value)
{
    // unsigned char 转换满足 cctype 对负 char 输入的前置条件。
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    // 尾部独立裁切，允许全空白输入逐步缩减为空视图。
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    return value;
}

/// @brief 将 ASCII 字母折叠为小写；UTF-8 字节保持原值以支持精确 CJK 匹配。
/// @param value 单个候选或查询字节。
/// @return 大写 ASCII 对应的小写字节，其他字节原样返回。
/// @note 不使用区域设置，搜索结果不会随系统 locale 改变。
[[nodiscard]] inline unsigned char foldAscii(unsigned char value)
{
    // 只对连续的 A-Z 范围应用固定偏移。
    if ( value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z') ) {
        return static_cast<unsigned char>(
            value + static_cast<unsigned char>('a' - 'A'));
    }
    // 数字、符号和 UTF-8 多字节序列保持原值。
    return value;
}

/// @brief 判断候选从指定位置起是否与搜索词进行 ASCII 不区分大小写匹配。
/// @param candidate 待匹配文件名或资源 ID。
/// @param query 已裁切搜索词。
/// @param offset 候选中的起始字节偏移。
/// @return 查询完整落在候选范围内且逐字节匹配时返回 true。
[[nodiscard]] inline bool matchesAt(std::string_view candidate,
                                    std::string_view query, std::size_t offset)
{
    // 先用减法形式验证范围，避免 offset + size 溢出。
    if ( offset > candidate.size() ||
         query.size() > candidate.size() - offset ) {
        return false;
    }
    // ASCII 字母折叠后比较，非 ASCII 字节需要完全一致。
    for ( std::size_t index = 0; index < query.size(); ++index ) {
        if ( foldAscii(static_cast<unsigned char>(candidate[offset + index])) !=
             foldAscii(static_cast<unsigned char>(query[index])) ) {
            return false;
        }
    }
    // 循环完成表示 query 的每个字节均匹配。
    return true;
}

/// @brief 查找 ASCII 不区分大小写的连续子串位置。
/// @param candidate 待搜索文件名或资源 ID。
/// @param query 已裁切搜索词。
/// @return 首个匹配字节偏移；不存在时返回空 optional。
/// @warning 查询变化时调用，最坏复杂度为候选长度乘查询长度。
[[nodiscard]] inline std::optional<std::size_t> findSubstring(
    std::string_view candidate, std::string_view query)
{
    // 空查询按标准字符串查找语义匹配起点。
    if ( query.empty() ) return 0;
    // 查询比候选更长时不进入偏移循环。
    if ( query.size() > candidate.size() ) return std::nullopt;
    // 最终偏移仍保证剩余候选长度足以容纳 query。
    const std::size_t finalOffset = candidate.size() - query.size();
    for ( std::size_t offset = 0; offset <= finalOffset; ++offset ) {
        if ( matchesAt(candidate, query, offset) ) return offset;
    }
    // 没有任何连续区间匹配。
    return std::nullopt;
}

/// @brief 对文件名或资源 ID 与搜索词的相似度评分。
/// @param candidate 待排序候选文本。
/// @param query 用户输入的原始搜索词。
/// @return 完全、前缀、子串或顺序模糊匹配的分数；不匹配时返回空。
/// @details 分数段按完全匹配、前缀、连续子串和顺序模糊匹配分为四档，档间
/// 保留足够间隔，使长度和位置惩罚不会颠倒主要匹配质量。
/// @warning 只在搜索词或资源缓存变化时批量调用；函数自身不分配内存。
[[nodiscard]] inline std::optional<int> scoreCandidate(
    std::string_view candidate, std::string_view query)
{
    // 搜索词只裁切 ASCII 首尾空白，候选名称保持原样。
    query = trimAsciiWhitespace(query);
    // 空候选和纯空白查询不参与结果列表。
    if ( candidate.empty() || query.empty() ) return std::nullopt;

    // 连续匹配优先于后续顺序模糊扫描。
    const auto substring = findSubstring(candidate, query);
    if ( substring ) {
        // 候选额外长度作为轻量惩罚，并设置上限防止 int 溢出。
        const int lengthPenalty = static_cast<int>(
            std::min<std::size_t>(candidate.size() - query.size(), 50'000));
        if ( *substring == 0 && candidate.size() == query.size() ) {
            // 大小写折叠后的完整等长匹配占最高固定分数段。
            return 400'000;
        }
        if ( *substring == 0 ) {
            // 前缀匹配只扣除候选冗余长度。
            return 300'000 - lengthPenalty;
        }
        // 普通子串还按出现位置降低分数，使靠前命中优先。
        const int positionPenalty =
            static_cast<int>(std::min<std::size_t>(*substring, 1'000)) * 32;
        return 200'000 - positionPenalty - lengthPenalty;
    }

    // 连续匹配失败后，按顺序寻找查询中的每个字节。
    std::size_t queryIndex = 0;
    // firstMatch 初始为候选末尾，仅在首个字节命中时改写。
    std::size_t firstMatch    = candidate.size();
    std::size_t previousMatch = 0;
    std::size_t gapCount      = 0;
    // 相邻命中对用于奖励较紧凑的模糊匹配。
    std::size_t consecutivePairs = 0;
    for ( std::size_t candidateIndex = 0;
          candidateIndex < candidate.size() && queryIndex < query.size();
          ++candidateIndex ) {
        if ( foldAscii(static_cast<unsigned char>(candidate[candidateIndex])) !=
             foldAscii(static_cast<unsigned char>(query[queryIndex])) ) {
            // 当前候选字节不匹配时继续向后扫描，不回退查询位置。
            continue;
        }
        if ( queryIndex == 0 ) {
            // 首次命中位置用于惩罚远离文件名开头的结果。
            firstMatch = candidateIndex;
        } else {
            // 与前次命中的距离区分连续奖励和间隔惩罚。
            const std::size_t distance = candidateIndex - previousMatch;
            if ( distance == 1 ) {
                ++consecutivePairs;
            } else {
                // distance 减一只统计两个命中字节之间跳过的字符。
                gapCount += distance - 1;
            }
        }
        previousMatch = candidateIndex;
        ++queryIndex;
    }
    // 查询仍有未命中字节时不存在顺序模糊匹配。
    if ( queryIndex != query.size() ) return std::nullopt;

    // 查询越长，成功的顺序匹配提供越高基础奖励。
    const int queryReward =
        static_cast<int>(std::min<std::size_t>(query.size(), 1'000)) * 96;
    // 连续命中对减少分散字符意外匹配的排序优势。
    const int consecutiveReward =
        static_cast<int>(std::min<std::size_t>(consecutivePairs, 1'000)) * 24;
    // 起点越靠后，候选排序越低。
    const int startPenalty =
        static_cast<int>(std::min<std::size_t>(firstMatch, 1'000)) * 16;
    // 命中之间跳过的字符越多，模糊匹配越弱。
    const int gapPenalty =
        static_cast<int>(std::min<std::size_t>(gapCount, 1'000)) * 20;
    // 最后仍对候选整体冗余长度施加有界惩罚。
    const int lengthPenalty = static_cast<int>(
        std::min<std::size_t>(candidate.size() - query.size(), 50'000));
    // 模糊匹配固定在十万分数段，低于任意连续子串档。
    return 100'000 + queryReward + consecutiveReward - startPenalty -
           gapPenalty - lengthPenalty;
}

}  // namespace MMM::UI::ProjectAudioToolSearch
