#pragma once

#include "canvas/ComposeTargetEligibility.h"
#include "common/render/RenderSnapshot.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace MMM::Canvas
{
/// @brief 长条练习的单轨起止拍位；起点时间始终早于终点。
struct ComposeHoldTarget {
    /// @brief 与上一颗单键不同的玩家轨号。
    int track{ 0 };
    /// @brief 按下位置的精确分拍时间。
    double startTime{ 0.0 };
    /// @brief 松开位置的精确分拍时间。
    double endTime{ 0.0 };
};

/// @brief 从实际拍线中选择紧邻上一颗单键、位于其他轨道的长条路径。
/// @param lines 当前玩家区真实绘制的分拍线，无需按时间排序。
/// @param noteTime 刚才成功放置的单键时间，不能用当前播放位置替代。
/// @param noteTrack 单键所在玩家轨道。
/// @param trackCount 当前玩家轨道数量。
/// @param noteHeight 当前 Note 真实高度，用于保证端点完整可见且可区分。
/// @param top 可见区域上界，已扣除 UI 补间位移。
/// @param bottom 可见区域下界，已扣除 UI 补间位移。
/// @param seed 仅影响另一个轨道的选择，不改变附近拍位的排序。
/// @param excludedEndTime 已绘制 Flick 头部所在拍位；长条尾部不得重合。
/// @param excludedTrack Flick 头部轨道；多轨谱面先改用另一条轨道。
/// @return 普通练习首选后继；同轨 Flick 附近改走其时间相反侧。
/// @note 多轨先换轨，双轨换轨无解时改选相反侧时间区间。
/// @note 无完整可见的安全区间时返回空，不让长条身体穿过已有 Flick。
/// @warning 教程目标失效时调用；只扫描可见拍线，不分配、排序或访问 ECS。
inline std::optional<ComposeHoldTarget> chooseComposeHoldTarget(
    std::span<const Common::Render::PlayerBeatLineSnapshot> lines,
    double noteTime, int noteTrack, int trackCount, float noteHeight, float top,
    float bottom, std::uint64_t seed,
    std::optional<double> excludedEndTime = std::nullopt,
    std::optional<int>    excludedTrack   = std::nullopt)
{
    // 单轨谱面无法满足异轨要求；缺少真实尺寸时也不能猜测提示框。
    if ( trackCount < 2 || noteTrack < 0 || noteTrack >= trackCount ||
         !isComposeTargetTime(noteTime) || !std::isfinite(noteHeight) ||
         noteHeight <= 0.0F )
        return std::nullopt;
    /// @brief 以完整端点高度判断可见性，防止框中心可见但边缘被裁剪。
    const auto visible = [&](const auto& line) {
        // Flick 也复用本候选集，不能单独放宽负时间或裁剪约束。
        return isComposeTargetBeatLine(
            line.time, line.y, noteHeight, top, bottom);
    };
    const Common::Render::PlayerBeatLineSnapshot* anchor = nullptr;
    // 借用输入中的实际拍位保留首拍偏移与变速段相位，不创建新的浮点网格。
    // 生命周期局限于本次调用，返回结果仅保存值，不向 UI 暴露观察指针。
    for ( const auto& line : lines ) {
        if ( visible(line) && std::abs(line.time - noteTime) < 1e-7 ) {
            anchor = &line;
            break;
        }
    }
    // 原拍位不再可见时等待用户返回，禁止重选到远处而丢失单键附近的约束。
    if ( !anchor ) return std::nullopt;
    int targetTrack =
        (noteTrack + 1 + static_cast<int>(seed % (trackCount - 1))) %
        trackCount;
    // 三轨以上优先避开 Flick 头所在轨，整个 Hold 身体也不会穿过它。
    if ( excludedTrack && targetTrack == *excludedTrack && trackCount > 2 ) {
        for ( int candidate = 0; candidate < trackCount; ++candidate ) {
            if ( candidate != noteTrack && candidate != *excludedTrack ) {
                targetTrack = candidate;
                break;
            }
        }
    }
    const bool sameFlickTrack =
        excludedEndTime && (!excludedTrack || targetTrack == *excludedTrack);
    // 只有仍在同一轨时才应用排除拍位，否则同拍但异轨并不重叠。
    double next     = std::numeric_limits<double>::infinity();
    double previous = -std::numeric_limits<double>::infinity();
    // 分段滚动可能让拍线乱序；线性选择最近时间即可，无需热路径完整排序。
    for ( const auto& line : lines ) {
        // 首尾至少相隔两个真实 Note 高度，避免提示框挤在一起。
        // 时间正负与屏幕上下独立判断，兼容反向 SV 下的向下拖动。
        if ( !visible(line) ||
             std::abs(line.y - anchor->y) < noteHeight * 2.0F )
            continue;
        // 同轨时禁用 Flick 头拍位；异轨时可照常选最近后继。
        if ( line.time > noteTime && line.time < next &&
             (!sameFlickTrack ||
              std::abs(line.time - *excludedEndTime) >= 1e-7) )
            next = line.time;
        if ( line.time < noteTime && line.time > previous )
            previous = line.time;
    }
    // 两轨时同轨不可避开，改用 Flick 头相反侧的时间区间。
    // 这样不仅尾部不重叠，Hold 身体也不会穿过已有 Flick 头。
    if ( sameFlickTrack ) {
        if ( *excludedEndTime > noteTime + 1e-7 && std::isfinite(previous) )
            return ComposeHoldTarget{ targetTrack, previous, noteTime };
        if ( *excludedEndTime < noteTime - 1e-7 && std::isfinite(next) )
            return ComposeHoldTarget{ targetTrack, noteTime, next };
        return std::nullopt;
    }
    // 普通路径仍优先选最近后继，靠近上沿时退到最近前驱。
    if ( !std::isfinite(next) && !std::isfinite(previous) ) return std::nullopt;
    return ComposeHoldTarget{
        .track     = targetTrack,
        .startTime = std::isfinite(next) ? noteTime : previous,
        .endTime   = std::isfinite(next) ? next : noteTime,
    };
}
}  // namespace MMM::Canvas
