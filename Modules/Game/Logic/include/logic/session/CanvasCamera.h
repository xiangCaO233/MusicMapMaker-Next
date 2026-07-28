#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace MMM::Logic
{

/// @brief 画布轨道所属区域。
enum class CanvasLaneKind : std::uint8_t {
    Player = 0,  ///< 玩家可操作的主轨道区。
    Bgm,         ///< 自动采样使用的 BGM 轨道区。
};

/// @brief 统一描述玩家轨道或 BGM 轨道的区域内地址。
struct CanvasLaneAddress {
    /// @brief 轨道所属区域。
    CanvasLaneKind kind{ CanvasLaneKind::Player };

    /// @brief 所属区域内从零开始的轨道索引。
    std::uint32_t index{ 0 };

    /// @brief 将区域内地址换算为统一画布绝对轨道。
    /// @param playerTrackCount 玩家轨道数量。
    /// @return 玩家轨道直接返回 index，BGM 轨道返回 K + index。
    [[nodiscard]] std::uint32_t absoluteTrack(
        std::uint32_t playerTrackCount) const
    {
        return kind == CanvasLaneKind::Player ? index
                                              : playerTrackCount + index;
    }

    /// @brief 将统一画布绝对轨道转换为区域内地址。
    /// @param absoluteTrack 统一画布绝对轨道。
    /// @param playerTrackCount 玩家轨道数量。
    /// @return 玩家区返回 Player 地址，其余返回 Bgm 地址。
    [[nodiscard]] static CanvasLaneAddress fromAbsoluteTrack(
        std::uint32_t absoluteTrack, std::uint32_t playerTrackCount)
    {
        if ( absoluteTrack < playerTrackCount ) {
            return { CanvasLaneKind::Player, absoluteTrack };
        }
        return { CanvasLaneKind::Bgm, absoluteTrack - playerTrackCount };
    }

    /// @brief 判断两个轨道地址是否相同。
    bool operator==(const CanvasLaneAddress&) const = default;
};

/// @brief 单条统一画布轨道的逻辑像素边界。
struct CanvasLaneBounds {
    /// @brief 左边界。
    float leftX{ 0.0F };

    /// @brief 右边界。
    float rightX{ 0.0F };

    /// @brief 判断逻辑横坐标是否落在该轨道内。
    /// @param x 待判断横坐标。
    /// @return 位于半开区间内时返回 true。
    [[nodiscard]] bool contains(float x) const
    {
        return std::isfinite(x) && x >= leftX && x < rightX;
    }
};

/// @brief 主画布玩家轨道在逻辑像素空间中的横向投影。
struct CanvasTrackProjection {
    /// @brief 玩家轨道区左边界。
    float leftX{ 0.0F };

    /// @brief 玩家轨道区右边界。
    float rightX{ 0.0F };

    /// @brief 单条玩家轨道宽度。
    float singleTrackWidth{ 0.0F };

    /// @brief 投影参数是否可用于坐标换算。
    bool valid{ false };

    /// @brief 判断逻辑像素横坐标是否位于玩家轨道区。
    /// @param x 待判断的横坐标。
    /// @return 位于轨道区闭区间内时返回 true。
    [[nodiscard]] bool contains(float x) const
    {
        return valid && std::isfinite(x) && x >= leftX && x <= rightX;
    }

    /// @brief 将逻辑像素横坐标换算为从零开始的玩家轨道索引。
    /// @param x 待换算的横坐标。
    /// @param trackCount 玩家轨道数量。
    /// @return 限制在有效范围内的轨道索引；投影无效时返回 0。
    [[nodiscard]] std::int32_t trackAt(float x, std::int32_t trackCount) const
    {
        if ( !valid || trackCount <= 0 || !std::isfinite(x) ) {
            return 0;
        }
        const auto track = static_cast<std::int32_t>(
            std::floor((x - leftX) / singleTrackWidth));
        return std::clamp(track, std::int32_t{ 0 }, trackCount - 1);
    }
};

/// @brief 玩家区与 BGM 区共享的主画布横向投影。
struct CanvasLaneProjection {
    /// @brief 玩家轨道区投影。
    CanvasTrackProjection player;

    /// @brief 玩家轨道数量。
    std::uint32_t playerLaneCount{ 0 };

    /// @brief 当前可访问的 BGM 轨道数量，包含末尾运行时追加轨。
    std::uint32_t bgmLaneCount{ 0 };

    /// @brief BGM 轨道区左边界。
    float bgmLeftX{ 0.0F };

    /// @brief BGM 轨道区右边界。
    float bgmRightX{ 0.0F };

    /// @brief 投影是否有效。
    bool valid{ false };

    /// @brief 获取指定统一轨道地址的横向边界。
    /// @param address 区域内轨道地址。
    /// @return 有效轨道边界；地址越界或投影无效时为空。
    [[nodiscard]] std::optional<CanvasLaneBounds> bounds(
        CanvasLaneAddress address) const
    {
        if ( !valid ) return std::nullopt;
        if ( address.kind == CanvasLaneKind::Player ) {
            if ( address.index >= playerLaneCount ) return std::nullopt;
            const float left =
                player.leftX +
                static_cast<float>(address.index) * player.singleTrackWidth;
            return CanvasLaneBounds{ left, left + player.singleTrackWidth };
        }
        if ( address.index >= bgmLaneCount ) return std::nullopt;
        const float left = bgmLeftX + static_cast<float>(address.index) *
                                          player.singleTrackWidth;
        return CanvasLaneBounds{ left, left + player.singleTrackWidth };
    }

    /// @brief 将横坐标换算为统一轨道地址。
    /// @param x 画布局部逻辑横坐标。
    /// @return 位于玩家区或当前 BGM 区时返回对应地址。
    [[nodiscard]] std::optional<CanvasLaneAddress> laneAt(float x) const
    {
        if ( !valid || !std::isfinite(x) || x < player.leftX ||
             x >= bgmRightX ) {
            return std::nullopt;
        }
        if ( x < player.rightX ) {
            const auto index = static_cast<std::uint32_t>(
                std::floor((x - player.leftX) / player.singleTrackWidth));
            if ( index < playerLaneCount ) {
                return CanvasLaneAddress{ CanvasLaneKind::Player, index };
            }
            return std::nullopt;
        }

        const auto index = static_cast<std::uint32_t>(
            std::floor((x - bgmLeftX) / player.singleTrackWidth));
        if ( index < bgmLaneCount ) {
            return CanvasLaneAddress{ CanvasLaneKind::Bgm, index };
        }
        return std::nullopt;
    }

    /// @brief 计算视口内可见的 BGM 轨道索引半开区间。
    /// @param viewportLeft 视口左边界。
    /// @param viewportRight 视口右边界。
    /// @return `[begin,end)`；没有可见 BGM 轨时返回空。
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
    visibleBgmRange(float viewportLeft, float viewportRight) const
    {
        if ( !valid || bgmLaneCount == 0 || !std::isfinite(viewportLeft) ||
             !std::isfinite(viewportRight) ) {
            return std::nullopt;
        }
        if ( viewportLeft > viewportRight ) {
            std::swap(viewportLeft, viewportRight);
        }
        if ( viewportRight <= bgmLeftX || viewportLeft >= bgmRightX ) {
            return std::nullopt;
        }

        const auto begin = static_cast<std::uint32_t>(std::clamp(
            std::floor((viewportLeft - bgmLeftX) / player.singleTrackWidth),
            0.0F,
            static_cast<float>(bgmLaneCount)));
        const auto end   = static_cast<std::uint32_t>(std::clamp(
            std::ceil((viewportRight - bgmLeftX) / player.singleTrackWidth),
            0.0F,
            static_cast<float>(bgmLaneCount)));
        if ( begin >= end ) return std::nullopt;
        return std::pair{ begin, end };
    }
};

/// @brief 计算应用横向相机偏移后的玩家轨道投影。
/// @param viewportWidth 视口逻辑宽度。
/// @param trackCount 玩家轨道数量。
/// @param layoutLeft 玩家轨道区左边界比例。
/// @param layoutRight 玩家轨道区右边界比例。
/// @param horizontalOffsetX 相机产生的内容横向逻辑像素偏移。
/// @return 可供渲染、拾取和工具坐标换算共用的轨道投影。
/// @warning 逻辑与渲染热路径可能每帧调用；只允许常量级数值运算。
[[nodiscard]] inline CanvasTrackProjection calculatePlayerTrackProjection(
    float viewportWidth, std::int32_t trackCount, float layoutLeft,
    float layoutRight, float horizontalOffsetX)
{
    CanvasTrackProjection result;
    if ( !std::isfinite(viewportWidth) || viewportWidth <= 0.0F ||
         trackCount <= 0 ) {
        return result;
    }

    if ( !std::isfinite(layoutLeft) ) {
        layoutLeft = 0.0F;
    }
    if ( !std::isfinite(layoutRight) ) {
        layoutRight = 1.0F;
    }
    if ( layoutLeft >= layoutRight ) {
        layoutRight = layoutLeft + 0.01F;
    }
    if ( !std::isfinite(horizontalOffsetX) ) {
        horizontalOffsetX = 0.0F;
    }

    result.leftX  = viewportWidth * layoutLeft + horizontalOffsetX;
    result.rightX = viewportWidth * layoutRight + horizontalOffsetX;
    result.singleTrackWidth =
        (result.rightX - result.leftX) / static_cast<float>(trackCount);
    result.valid = std::isfinite(result.leftX) &&
                   std::isfinite(result.rightX) &&
                   std::isfinite(result.singleTrackWidth) &&
                   result.singleTrackWidth > 0.0F;
    return result;
}

/// @brief 计算玩家区及其右侧 BGM 区的统一轨道投影。
/// @param viewportWidth 视口逻辑宽度。
/// @param playerTrackCount 玩家轨道数量 K。
/// @param persistentBgmTrackCount 持久化 BGM 轨道数量。
/// @param layoutLeft 玩家轨道区左边界比例。
/// @param layoutRight 玩家轨道区右边界比例。
/// @param horizontalOffsetX 相机产生的内容横向逻辑像素偏移。
/// @param includeAppendLane 是否在持久轨道后显示一条运行时追加轨。
/// @return 可供渲染、拾取、框选和拖动共用的统一投影。
/// @warning 逻辑与渲染热路径可能每帧调用；只允许常量级数值运算。
[[nodiscard]] inline CanvasLaneProjection calculateCanvasLaneProjection(
    float viewportWidth, std::int32_t playerTrackCount,
    std::int32_t persistentBgmTrackCount, float layoutLeft, float layoutRight,
    float horizontalOffsetX, bool includeAppendLane = true)
{
    CanvasLaneProjection result;
    result.player = calculatePlayerTrackProjection(viewportWidth,
                                                   playerTrackCount,
                                                   layoutLeft,
                                                   layoutRight,
                                                   horizontalOffsetX);
    if ( !result.player.valid ) return result;

    result.playerLaneCount     = static_cast<std::uint32_t>(playerTrackCount);
    const auto persistentCount = static_cast<std::uint32_t>(
        std::max(std::int32_t{ 0 }, persistentBgmTrackCount));
    result.bgmLaneCount =
        persistentCount + static_cast<std::uint32_t>(includeAppendLane);
    result.bgmLeftX = result.player.rightX;
    result.bgmRightX =
        result.bgmLeftX + static_cast<float>(result.bgmLaneCount) *
                              result.player.singleTrackWidth;
    result.valid = std::isfinite(result.bgmLeftX) &&
                   std::isfinite(result.bgmRightX) &&
                   result.bgmRightX >= result.bgmLeftX;
    return result;
}

/// @brief 在逻辑视口宽度变化时等比例换算横向相机偏移。
/// @param horizontalOffsetX 旧视口下的内容横向偏移。
/// @param oldViewportWidth 旧视口逻辑宽度。
/// @param newViewportWidth 新视口逻辑宽度。
/// @return 新视口下保持相同比例位移的横向偏移。
/// @warning 视口更新路径调用；只允许常量级数值运算。
[[nodiscard]] inline float resizeCanvasHorizontalOffset(float horizontalOffsetX,
                                                        float oldViewportWidth,
                                                        float newViewportWidth)
{
    if ( !std::isfinite(horizontalOffsetX) ) {
        return 0.0F;
    }
    if ( !std::isfinite(oldViewportWidth) || oldViewportWidth <= 0.0F ||
         !std::isfinite(newViewportWidth) || newViewportWidth <= 0.0F ) {
        return horizontalOffsetX;
    }
    return horizontalOffsetX * newViewportWidth / oldViewportWidth;
}

}  // namespace MMM::Logic
