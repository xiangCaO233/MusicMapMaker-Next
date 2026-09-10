#pragma once

#include "config/visual/TrackLayoutConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace MMM::Logic
{

/// @brief 画布轨道所属区域。
enum class CanvasLaneKind : std::uint8_t {
    Draft = 0,  ///< 当前谱面的草稿轨道区。
    Player,     ///< 玩家可操作的主轨道区。
    Bgm,        ///< 自动采样使用的 BGM 轨道区。
};

/// @brief 统一描述草稿、玩家或 BGM 轨道的区域内地址。
struct CanvasLaneAddress {
    /// @brief 轨道所属区域。
    CanvasLaneKind kind{ CanvasLaneKind::Player };

    /// @brief 所属区域内从零开始的轨道索引。
    std::uint32_t index{ 0 };

    /// @brief 将区域内地址换算为统一画布绝对轨道。
    /// @param playerTrackCount 玩家轨道数量。
    /// @param draftTrackCount 当前可访问的草稿轨道数量；零时兼容为玩家轨道数。
    /// @return 草稿轨按当前草稿区宽度返回负轨，玩家轨返回 index，BGM 轨返回
    /// K+index。
    /// @pre 索引与数量处于有符号轨道类型可表示范围内，地址所属区域合法。
    /// @note 区域地址不保存当时的轨道数量，转换时必须使用与该地址匹配的布局。
    /// @warning 交互热路径仅作常量换算，不验证地址是否已持久化。
    [[nodiscard]] std::int32_t absoluteTrack(
        std::uint32_t playerTrackCount, std::uint32_t draftTrackCount = 0) const
    {
        if ( kind == CanvasLaneKind::Draft ) {
            // 草稿从负 count 排到 -1，区域内索引零对应最左侧可访问轨道。
            const auto count =
                draftTrackCount > 0 ? draftTrackCount : playerTrackCount;
            return static_cast<std::int32_t>(index) -
                   static_cast<std::int32_t>(count);
        }
        if ( kind == CanvasLaneKind::Player ) {
            return static_cast<std::int32_t>(index);
        }
        return static_cast<std::int32_t>(playerTrackCount + index);
    }

    /// @brief 将统一画布绝对轨道转换为区域内地址。
    /// @param absoluteTrack 统一画布有符号轨道；负值表示草稿轨。
    /// @param playerTrackCount 玩家轨道数量。
    /// @param draftTrackCount 当前可访问的草稿轨道数量；零时兼容为玩家轨道数。
    /// @return 负轨返回 Draft 地址，玩家区返回 Player 地址，其余返回 Bgm
    /// 地址。
    /// @note 超出草稿左边界的负轨映射到首轨，不保证非法输入可逆。
    /// @note BGM 上限不在该接口检查，实际访问须再调用投影 bounds 校验。
    /// @warning 交互热路径只做数值分类，不按像素位置判断区域。
    [[nodiscard]] static CanvasLaneAddress fromAbsoluteTrack(
        std::int32_t absoluteTrack, std::uint32_t playerTrackCount,
        std::uint32_t draftTrackCount = 0)
    {
        if ( absoluteTrack < 0 ) {
            const auto count =
                draftTrackCount > 0 ? draftTrackCount : playerTrackCount;
            const auto index = absoluteTrack + static_cast<std::int32_t>(count);
            return { CanvasLaneKind::Draft,
                     static_cast<std::uint32_t>(std::max(0, index)) };
        }
        if ( absoluteTrack < playerTrackCount ) {
            return { CanvasLaneKind::Player,
                     static_cast<std::uint32_t>(absoluteTrack) };
        }
        return { CanvasLaneKind::Bgm,
                 static_cast<std::uint32_t>(absoluteTrack) - playerTrackCount };
    }

    /// @brief 判断两个轨道地址是否相同。
    bool operator==(const CanvasLaneAddress&) const = default;
};

/// @brief 将草稿负轨道换算为面向用户的一基 DRAFT 轨道编号。
/// @param absoluteTrack 草稿区统一画布负轨道。
/// @param persistentDraftTrackCount 持久化草稿轨道数量。
/// @return 当前持久轨编号；运行时追加轨返回确认扩充后将使用的 1。
/// @warning 逻辑与 UI 热路径可能每帧调用；只允许常量级整数运算。
[[nodiscard]] inline std::int32_t draftTrackDisplayNumber(
    std::int32_t absoluteTrack, std::int32_t persistentDraftTrackCount)
{
    const auto count = std::max(0, persistentDraftTrackCount);
    // 左侧运行时追加轨尚无持久编号，显示其确认扩充后对应的一号轨。
    if ( absoluteTrack < -count ) return 1;
    return std::max(1, absoluteTrack + count + 1);
}

/// @brief 单条统一画布轨道的逻辑像素边界。
struct CanvasLaneBounds {
    /// @brief 左边界。
    float leftX{ 0.0F };

    /// @brief 右边界。
    float rightX{ 0.0F };

    /// @brief 判断逻辑横坐标是否落在该轨道内。
    /// @param x 待判断横坐标。
    /// @return 位于半开区间内时返回 true。
    /// @warning 拾取热路径常量判断，右边界归相邻轨道，避免双重命中。
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
    /// @note 这是区域测试，与单轨道的半开边界约定不同。
    /// @warning 交互热路径不更新投影状态。
    [[nodiscard]] bool contains(float x) const
    {
        return valid && std::isfinite(x) && x >= leftX && x <= rightX;
    }

    /// @brief 将逻辑像素横坐标换算为从零开始的玩家轨道索引。
    /// @param x 待换算的横坐标。
    /// @param trackCount 玩家轨道数量。
    /// @return 限制在有效范围内的轨道索引；投影无效时返回 0。
    /// @pre valid 时 singleTrackWidth 为有限正数，由投影生成函数保证。
    /// @warning 拾取热路径不遍历轨道，使用等宽区间直接换算。
    [[nodiscard]] std::int32_t trackAt(float x, std::int32_t trackCount) const
    {
        if ( !valid || trackCount <= 0 || !std::isfinite(x) ) {
            return 0;
        }
        const auto track = static_cast<std::int32_t>(
            // 向下取整决定所属槽位，再将区域外坐标夹到两侧端轨。
            std::floor((x - leftX) / singleTrackWidth));
        return std::clamp(track, std::int32_t{ 0 }, trackCount - 1);
    }
};

/// @brief 草稿区、玩家区与 BGM 区共享的主画布横向投影。
struct CanvasLaneProjection {
    /// @brief 批注时间戳标记区固定逻辑宽度。
    static constexpr float ANNOTATION_GUTTER_WIDTH = 26.0F;

    /// @brief 当前可访问的草稿轨道数量，包含最左侧运行时追加轨。
    std::uint32_t draftLaneCount{ 0 };

    /// @brief 草稿轨道区左边界。
    float draftLeftX{ 0.0F };

    /// @brief 草稿轨道区右边界。
    float draftRightX{ 0.0F };

    /// @brief 单条草稿轨道宽度。
    float draftLaneWidth{ 0.0F };

    /// @brief 玩家轨道区投影。
    CanvasTrackProjection player;

    /// @brief 玩家轨道数量。
    std::uint32_t playerLaneCount{ 0 };

    /// @brief 当前可访问的 BGM 轨道数量，包含末尾运行时追加轨。
    std::uint32_t bgmLaneCount{ 0 };

    /// @brief 批注标记区左边界。
    float annotationLeftX{ 0.0F };

    /// @brief 批注标记区右边界。
    float annotationRightX{ 0.0F };

    /// @brief BGM 轨道区左边界。
    float bgmLeftX{ 0.0F };

    /// @brief BGM 轨道区右边界。
    float bgmRightX{ 0.0F };

    /// @brief 单条 BGM 轨道宽度。
    float bgmLaneWidth{ 0.0F };

    /// @brief 投影是否有效。
    bool valid{ false };

    /// @brief 获取指定统一轨道地址的横向边界。
    /// @param address 区域内轨道地址。
    /// @return 有效轨道边界；地址越界或投影无效时为空。
    /// @warning 渲染与拾取热路径只读取当前投影，不能在此重新计算布局。
    [[nodiscard]] std::optional<CanvasLaneBounds> bounds(
        CanvasLaneAddress address) const
    {
        if ( !valid ) return std::nullopt;
        // 地址索引始终是区域局部索引，不能直接当成绝对轨道乘单轨宽度。
        if ( address.kind == CanvasLaneKind::Draft ) {
            if ( address.index >= draftLaneCount ) return std::nullopt;
            // 草稿轨独立宽度用于渲染、拾取与拖动的同一边界。
            const float left =
                draftLeftX + static_cast<float>(address.index) * draftLaneWidth;
            return CanvasLaneBounds{ left, left + draftLaneWidth };
        }
        if ( address.kind == CanvasLaneKind::Player ) {
            if ( address.index >= playerLaneCount ) return std::nullopt;
            const float left =
                player.leftX +
                static_cast<float>(address.index) * player.singleTrackWidth;
            return CanvasLaneBounds{ left, left + player.singleTrackWidth };
        }
        if ( address.index >= bgmLaneCount ) return std::nullopt;
        // BGM 轨道不再借用玩家单轨宽度。
        const float left =
            bgmLeftX + static_cast<float>(address.index) * bgmLaneWidth;
        return CanvasLaneBounds{ left, left + bgmLaneWidth };
    }

    /// @brief 获取玩家、草稿、批注与 BGM 区域共同覆盖的横向范围。
    /// @return 不依赖各区域左右顺序的最小外包边界。
    /// @note 包围范围可包含区域间空隙，不等同于可拾取轨道集合。
    /// @pre 调用方先检查 valid；本函数不为无效投影提供错误返回值。
    /// @warning 布局热路径固定比较四个区域，不枚举所有轨道。
    [[nodiscard]] CanvasLaneBounds contentBounds() const
    {
        // 独立布局允许任意重排，不能再把 Draft/BGM 固定视为左右端点。
        return {
            std::min({ draftLeftX, player.leftX, annotationLeftX, bgmLeftX }),
            std::max(
                { draftRightX, player.rightX, annotationRightX, bgmRightX })
        };
    }

    /// @brief 将横坐标换算为统一轨道地址。
    /// @param x 画布局部逻辑横坐标。
    /// @return 位于可访问的玩家、草稿或 BGM 区时返回对应地址。
    /// @note 空隙和批注区返回空，不隐式吸附到相邻轨道。
    /// @warning 拾取热路径仅按区域和等宽轨道计算。
    [[nodiscard]] std::optional<CanvasLaneAddress> laneAt(float x) const
    {
        if ( !valid || !std::isfinite(x) ) return std::nullopt;
        // 批注区优先排除，独立移动后也不会误判为重叠轨道。
        if ( x >= annotationLeftX && x < annotationRightX ) {
            return std::nullopt;
        }
        // 玩家区优先于辅助轨，保证错误重叠配置下主编辑区仍可操作。
        if ( player.contains(x) ) {
            const auto index = static_cast<std::uint32_t>(
                std::floor((x - player.leftX) / player.singleTrackWidth));
            if ( index < playerLaneCount ) {
                // 区域 contains 包含右边界，但末端计算出 K 时不能生成越界地址。
                return CanvasLaneAddress{ CanvasLaneKind::Player, index };
            }
        }
        // 草稿区与 BGM 区分别按自身宽度换算，允许区域之间存在空隙。
        if ( x >= draftLeftX && x < draftRightX ) {
            const auto index = static_cast<std::uint32_t>(
                std::floor((x - draftLeftX) / draftLaneWidth));
            if ( index < draftLaneCount ) {
                return CanvasLaneAddress{ CanvasLaneKind::Draft, index };
            }
        }
        if ( x >= bgmLeftX && x < bgmRightX ) {
            const auto index = static_cast<std::uint32_t>(
                std::floor((x - bgmLeftX) / bgmLaneWidth));
            if ( index < bgmLaneCount ) {
                return CanvasLaneAddress{ CanvasLaneKind::Bgm, index };
            }
        }
        return std::nullopt;
    }

    /// @brief 将空隙或区域外横坐标吸附到最近的合法轨道。
    /// @param x 画布局部逻辑横坐标。
    /// @return 最近轨道；批注区、非有限坐标或无轨道时返回空。
    /// @note 距离按区域边缘而非轨道中心计算，适合拖动越界后的延续定位。
    /// @note 批注区始终禁止吸附，即使它与轨道区域发生重叠。
    /// @warning 拖动热路径固定检查三个区域，不搜索全部轨道。
    [[nodiscard]] std::optional<CanvasLaneAddress> nearestLane(float x) const
    {
        if ( const auto direct = laneAt(x) ) return direct;
        // 已命中轨道时沿用精确结果，仅为空隙或越界坐标寻找最近端轨。
        if ( !valid || !std::isfinite(x) ||
             (x >= annotationLeftX && x < annotationRightX) ) {
            return std::nullopt;
        }

        std::optional<CanvasLaneAddress> nearest;
        // 首个合法候选替代无穷远距离，后续只有严格更近时才覆盖。
        float      distance = std::numeric_limits<float>::infinity();
        const auto consider = [&](CanvasLaneKind kind,
                                  std::uint32_t  count,
                                  float          left,
                                  float          right) {
            if ( count == 0U || right <= left ) return;
            const float candidateDistance =
                x < left ? left - x : (x >= right ? x - right : 0.0F);
            if ( candidateDistance >= distance ) return;
            // 区域内部空隙理论上不存在；这里仍按最近端点做防御性处理。
            const std::uint32_t index =
                x < left ? 0U : (x >= right ? count - 1U : 0U);
            nearest  = CanvasLaneAddress{ kind, index };
            distance = candidateDistance;
        };
        // 平距时沿用拾取优先级：玩家、草稿、BGM。
        consider(CanvasLaneKind::Player,
                 playerLaneCount,
                 player.leftX,
                 player.rightX);
        consider(
            CanvasLaneKind::Draft, draftLaneCount, draftLeftX, draftRightX);
        consider(CanvasLaneKind::Bgm, bgmLaneCount, bgmLeftX, bgmRightX);
        return nearest;
    }

    /// @brief 计算视口内可见的草稿轨道索引半开区间。
    /// @param viewportLeft 视口左边界。
    /// @param viewportRight 视口右边界。
    /// @return `[begin,end)`；没有可见草稿轨时返回空。
    /// @note 接受反向视口边界，内部统一规整为从左到右。
    /// @warning 渲染裁剪热路径直接计算索引范围，不逐轨测试相交。
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
    visibleDraftRange(float viewportLeft, float viewportRight) const
    {
        if ( !valid || draftLaneCount == 0 || !std::isfinite(viewportLeft) ||
             !std::isfinite(viewportRight) ) {
            return std::nullopt;
        }
        if ( viewportLeft > viewportRight ) {
            std::swap(viewportLeft, viewportRight);
        }
        if ( viewportRight <= draftLeftX || viewportLeft >= draftRightX ) {
            return std::nullopt;
        }

        const auto begin = static_cast<std::uint32_t>(
            // 左侧向下、右侧向上取整，将仅部分露出的轨道也纳入绘制范围。
            std::clamp(std::floor((viewportLeft - draftLeftX) / draftLaneWidth),
                       0.0F,
                       static_cast<float>(draftLaneCount)));
        const auto end = static_cast<std::uint32_t>(
            std::clamp(std::ceil((viewportRight - draftLeftX) / draftLaneWidth),
                       0.0F,
                       static_cast<float>(draftLaneCount)));
        if ( begin >= end ) return std::nullopt;
        return std::pair{ begin, end };
    }

    /// @brief 计算视口内可见的 BGM 轨道索引半开区间。
    /// @param viewportLeft 视口左边界。
    /// @param viewportRight 视口右边界。
    /// @return `[begin,end)`；没有可见 BGM 轨时返回空。
    /// @note 结果使用 BGM 局部索引，需要绝对索引时再加玩家轨道数。
    /// @note 运行时追加轨若已包含在 bgmLaneCount 中，也按普通可见轨道参与计算。
    /// @warning 渲染裁剪热路径常量换算，结果上界可等于轨道总数。
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

        const auto begin = static_cast<std::uint32_t>(
            std::clamp(std::floor((viewportLeft - bgmLeftX) / bgmLaneWidth),
                       0.0F,
                       static_cast<float>(bgmLaneCount)));
        const auto end = static_cast<std::uint32_t>(
            std::clamp(std::ceil((viewportRight - bgmLeftX) / bgmLaneWidth),
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
/// @note 布局比例可以超出 0～1，不把画布外轨道强制压回视口。
/// @note 输入有限仍可能在乘法后溢出，因此 valid 必须以最终坐标为准。
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
        // 退化布局保留左端锚点并提供最小比例跨度，不交换用户配置方向。
        layoutRight = layoutLeft + 0.01F;
    }
    if ( !std::isfinite(horizontalOffsetX) ) {
        horizontalOffsetX = 0.0F;
    }

    result.leftX = viewportWidth * layoutLeft + horizontalOffsetX;
    // 内容偏移只平移整个区域，不参与单轨宽度的比例计算。
    result.rightX = viewportWidth * layoutRight + horizontalOffsetX;
    result.singleTrackWidth =
        (result.rightX - result.leftX) / static_cast<float>(trackCount);
    result.valid = std::isfinite(result.leftX) &&
                   std::isfinite(result.rightX) &&
                   std::isfinite(result.singleTrackWidth) &&
                   result.singleTrackWidth > 0.0F;
    return result;
}

/// @brief 计算可独立布局的草稿、玩家、批注与 BGM 区横向投影。
/// @param viewportWidth 视口逻辑宽度。
/// @param playerTrackCount 玩家轨道数量 K。
/// @param persistentDraftTrackCount 持久化草稿轨道数量。
/// @param persistentBgmTrackCount 持久化 BGM 轨道数量。
/// @param layout 玩家边界及辅助区域的可选位置与宽度配置。
/// @param horizontalOffsetX 相机产生的内容横向逻辑像素偏移。
/// @param includeAppendLane 是否在 BGM 持久轨道后显示一条运行时追加轨。
/// @param includeBgmLanes 是否显示并允许访问 BGM 轨道区。
/// @param includeDraftLanes 是否显示并允许访问当前谱面的草稿轨道区。
/// @param includeDraftAppendLane 是否在草稿持久轨道前显示一条运行时追加轨。
/// @return 可供渲染、拾取、框选和拖动共用的统一投影。
/// @par 草稿扩展方向
/// 布局编辑器写入右锚点后，草稿区随轨道数量增加只向左扩展。缺少右锚点的
/// 旧配置继续按左边界解析，避免加载时改变既有自定义布局；再次编辑该区域时会
/// 由布局编辑器物化右锚点。
/// @warning 逻辑与渲染热路径可能每帧调用；只允许常量级数值运算。
[[nodiscard]] inline CanvasLaneProjection calculateCanvasLaneProjection(
    float viewportWidth, std::int32_t playerTrackCount,
    std::int32_t persistentBgmTrackCount, const Config::TrackLayout& layout,
    float horizontalOffsetX, bool includeAppendLane = true,
    bool includeBgmLanes = true, bool includeDraftLanes = false,
    std::int32_t persistentDraftTrackCount = -1,
    bool         includeDraftAppendLane    = false)
{
    CanvasLaneProjection result;
    // 玩家矩形继续由 TrackLayout 四边控制，辅助区只复用其纵向范围。
    result.player = calculatePlayerTrackProjection(viewportWidth,
                                                   playerTrackCount,
                                                   layout.left,
                                                   layout.right,
                                                   horizontalOffsetX);
    if ( !result.player.valid ) return result;
    // 玩家投影是统一布局基础，基础无效时不构造看似可用的辅助区域。

    // 自定义位置使用归一化世界坐标，并与玩家区应用同一个相机偏移。
    const auto resolveLeft = [viewportWidth, horizontalOffsetX](
                                 const std::optional<float>& value,
                                 float                       fallback) {
        return value && std::isfinite(*value)
                   ? viewportWidth * *value + horizontalOffsetX
                   : fallback;
    };
    // 草稿区可保存右锚点，使最左追加轨增长时不侵入玩家区。
    const auto resolveRight = [viewportWidth, horizontalOffsetX](
                                  const std::optional<float>& value,
                                  float                       fallback) {
        return value && std::isfinite(*value)
                   ? viewportWidth * *value + horizontalOffsetX
                   : fallback;
    };
    // 自定义宽度按视口等比例缩放；非法或非正数仍保持旧版推导值。
    // 左边界回退值已经包含相机偏移，不能在回退路径重复加偏移。
    const auto resolveWidth = [viewportWidth](const std::optional<float>& value,
                                              float fallback) {
        const float width =
            value && std::isfinite(*value) ? viewportWidth * *value : fallback;
        return std::isfinite(width) && width > 0.0F ? width : fallback;
    };

    result.playerLaneCount = static_cast<std::uint32_t>(playerTrackCount);
    const auto persistentDraftCount = static_cast<std::uint32_t>(
        // 负持久数量表示兼容旧数据，以玩家轨道数提供草稿区默认宽度。
        std::max(std::int32_t{ 0 },
                 persistentDraftTrackCount >= 0 ? persistentDraftTrackCount
                                                : playerTrackCount));
    result.draftLaneCount =
        // 隐藏区域计数为零，即使启用追加轨也不能通过拾取访问。
        includeDraftLanes
            ? persistentDraftCount +
                  static_cast<std::uint32_t>(includeDraftAppendLane)
            : std::uint32_t{ 0 };
    // 草稿区优先以右边界为锚点向左增长；旧配置没有右锚点时仍沿用左边界。
    result.draftLaneWidth =
        resolveWidth(layout.draftLanes.width, result.player.singleTrackWidth);
    const float legacyDraftLeft =
        result.player.leftX -
        static_cast<float>(result.draftLaneCount) * result.draftLaneWidth;
    const float leftAnchoredDraftLeft =
        resolveLeft(layout.draftLanes.left, legacyDraftLeft);
    const float leftAnchoredDraftRight =
        leftAnchoredDraftLeft +
        static_cast<float>(result.draftLaneCount) * result.draftLaneWidth;
    result.draftRightX =
        resolveRight(layout.draftLanes.right, leftAnchoredDraftRight);
    result.draftLeftX =
        result.draftRightX -
        static_cast<float>(result.draftLaneCount) * result.draftLaneWidth;

    const auto persistentCount =
        includeBgmLanes ? static_cast<std::uint32_t>(std::max(
                              std::int32_t{ 0 }, persistentBgmTrackCount))
                        : std::uint32_t{ 0 };
    result.bgmLaneCount =
        includeBgmLanes
            ? persistentCount + static_cast<std::uint32_t>(includeAppendLane)
            : std::uint32_t{ 0 };
    // 批注区宽度表示整个沟槽，默认值继续保持固定 26 逻辑像素。
    result.annotationLeftX =
        resolveLeft(layout.annotation.left, result.player.rightX);
    const float annotationWidth = resolveWidth(
        layout.annotation.width, CanvasLaneProjection::ANNOTATION_GUTTER_WIDTH);
    result.annotationRightX = result.annotationLeftX + annotationWidth;
    // 批注沟槽独立存在，不随 BGM 区可见性消失，也不属于任何轨道地址。
    // BGM 区按自身单轨宽度排列，默认位置仍从批注区右侧开始。
    result.bgmLaneWidth =
        resolveWidth(layout.bgmLanes.width, result.player.singleTrackWidth);
    result.bgmLeftX =
        resolveLeft(layout.bgmLanes.left, result.annotationRightX);
    result.bgmRightX =
        result.bgmLeftX +
        static_cast<float>(result.bgmLaneCount) * result.bgmLaneWidth;

    // 独立区域允许空隙和重叠，但每个区间自身必须有限且非负向。
    // 区域顺序由布局决定，命中冲突的优先级由 laneAt 统一处理。
    result.valid =
        std::isfinite(result.draftLeftX) && std::isfinite(result.draftRightX) &&
        result.draftLaneWidth > 0.0F && std::isfinite(result.annotationLeftX) &&
        std::isfinite(result.annotationRightX) &&
        result.annotationRightX >= result.annotationLeftX &&
        std::isfinite(result.bgmLeftX) && std::isfinite(result.bgmRightX) &&
        result.bgmLaneWidth > 0.0F && result.bgmRightX >= result.bgmLeftX;
    return result;
}

/// @brief 使用旧版左右边界参数计算兼容投影。
/// @note 只构造布局值再委托完整接口，辅助区域继续使用默认位置和宽度。
/// @warning 仅供未迁移调用点和测试使用；新代码应传入完整 TrackLayout。
[[nodiscard]] inline CanvasLaneProjection calculateCanvasLaneProjection(
    float viewportWidth, std::int32_t playerTrackCount,
    std::int32_t persistentBgmTrackCount, float layoutLeft, float layoutRight,
    float horizontalOffsetX, bool includeAppendLane = true,
    bool includeBgmLanes = true, bool includeDraftLanes = false,
    std::int32_t persistentDraftTrackCount = -1,
    bool         includeDraftAppendLane    = false)
{
    Config::TrackLayout layout;
    // 保持新旧调用入口使用同一实现，避免两个投影算法产生拾取差异。
    layout.left  = layoutLeft;
    layout.right = layoutRight;
    return calculateCanvasLaneProjection(viewportWidth,
                                         playerTrackCount,
                                         persistentBgmTrackCount,
                                         layout,
                                         horizontalOffsetX,
                                         includeAppendLane,
                                         includeBgmLanes,
                                         includeDraftLanes,
                                         persistentDraftTrackCount,
                                         includeDraftAppendLane);
}

/// @brief 在逻辑视口宽度变化时等比例换算横向相机偏移。
/// @param horizontalOffsetX 旧视口下的内容横向偏移。
/// @param oldViewportWidth 旧视口逻辑宽度。
/// @param newViewportWidth 新视口逻辑宽度。
/// @return 新视口下保持相同比例位移的横向偏移。
/// @note 该缩放只维持归一化水平位移，不重新定位某个指定轨道中心。
/// @warning 视口更新路径调用；只允许常量级数值运算。
[[nodiscard]] inline float resizeCanvasHorizontalOffset(float horizontalOffsetX,
                                                        float oldViewportWidth,
                                                        float newViewportWidth)
{
    if ( !std::isfinite(horizontalOffsetX) ) {
        // 非法偏移不能传给后续投影，直接回到无平移状态。
        return 0.0F;
    }
    if ( !std::isfinite(oldViewportWidth) || oldViewportWidth <= 0.0F ||
         !std::isfinite(newViewportWidth) || newViewportWidth <= 0.0F ) {
        // 最小化或尚未取得有效视口时保留旧偏移，不按零宽度重新缩放。
        return horizontalOffsetX;
    }
    return horizontalOffsetX * newViewportWidth / oldViewportWidth;
}

}  // namespace MMM::Logic
