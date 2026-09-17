#pragma once

#include <span>
#include <string_view>

namespace MMM::UI
{

/// @brief 已打开谱面用于计算打包默认选择的轻量状态。
/// @details 状态由当前 Session 与停靠布局快照构造，不持有路径或画布对象。
struct PackageOpenBeatmapState {
    /// @brief 与候选文件使用相同规则生成的稳定绝对路径键。
    std::string_view beatmapPathKey{};

    /// @brief 对应主画布是否为所属停靠组当前显示的前台标签。
    bool isCanvasVisible{ false };

    /// @brief 对应 Session 是否为当前活动 Session。
    bool isActive{ false };

    /// @brief 是否为未加载谱面的 Logo 占位画布。
    bool isLogoPlaceholder{ false };
};

/// @brief 判断谱面候选是否应在打开打包窗口时默认选中。
/// @param candidatePathKey 候选谱面的稳定绝对路径键。
/// @param openBeatmaps 当前已打开谱面的前台、活动与占位状态。
/// @return 存在有效前台画布时只匹配全部前台画布；没有前台画布时只匹配活动画布。
/// @note 前台画布优先级高于活动 Session，避免隐藏标签被意外纳入默认包。
/// @note 同一路径若对应多个前台画布，任一匹配即可选中候选项。
constexpr bool shouldDefaultSelectPackageBeatmap(
    std::string_view                         candidatePathKey,
    std::span<const PackageOpenBeatmapState> openBeatmaps)
{
    // 空键无法与真实谱面建立稳定对应关系，必须直接排除。
    if ( candidatePathKey.empty() ) {
        return false;
    }

    bool hasVisibleBeatmap = false;
    // 第一轮只考察实际可见的谱面画布，并同时记录是否存在前台候选。
    for ( const auto& beatmap : openBeatmaps ) {
        // Logo 占位、无路径状态和后台标签均不能代表用户当前所见谱面。
        if ( beatmap.isLogoPlaceholder || beatmap.beatmapPathKey.empty() ||
             !beatmap.isCanvasVisible ) {
            continue;
        }
        hasVisibleBeatmap = true;
        // 发现前台精确匹配即可结束，避免继续遍历无关 Session。
        if ( beatmap.beatmapPathKey == candidatePathKey ) {
            return true;
        }
    }
    if ( hasVisibleBeatmap ) {
        // 已有前台谱面但未命中时，不再用活动 Session 覆盖用户可见选择。
        return false;
    }

    // 无前台谱面时回退到活动 Session，保持无停靠信息场景仍有默认项。
    for ( const auto& beatmap : openBeatmaps ) {
        if ( !beatmap.isLogoPlaceholder && beatmap.isActive &&
             beatmap.beatmapPathKey == candidatePathKey ) {
            return true;
        }
    }
    return false;
}

/// @brief 计算非 OGG 主音轨原点对齐选项在候选变化后的状态。
/// @param hasSelectedNonOggMainAudio 当前选择是否包含非 OGG 默认 Main 音轨。
/// @param userOverridden 用户是否已在本次打包流程中手动修改该选项。
/// @param userValue 用户最近一次手动选择的值。
/// @return 不可用时固定为 false；可用且未手动修改时默认返回 true。
/// @note 用户手动选择独立保存，选项短暂失效再恢复时不会丢失该选择。
constexpr bool resolveNonOggAudioAlignmentSelection(
    bool hasSelectedNonOggMainAudio, bool userOverridden, bool userValue)
{
    // 没有可对齐的谱面时清除残留值，命令层无需依赖 UI 可用状态。
    if ( !hasSelectedNonOggMainAudio ) {
        return false;
    }
    // 本次流程尚无用户选择时采用安全默认值；否则原样保留用户意图。
    return userOverridden ? userValue : true;
}

}  // namespace MMM::UI
