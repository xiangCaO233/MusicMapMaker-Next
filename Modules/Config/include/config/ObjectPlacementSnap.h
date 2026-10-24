#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

/// @file ObjectPlacementSnap.h
/// @brief 编辑设置与工作区共用的分拍吸附取值及位掩码契约。
/// @details 分母对应固定位序，旧项目与软件默认配置必须使用相同编码。
namespace MMM::Config
{

/// @brief 物件放置磁吸使用的分拍线来源。
enum class ObjectPlacementSnapMode {
    CurrentBeatDivisor,  ///< 仅使用当前分拍策略生成的分拍线。
    CommonBeatDivisors   ///< 使用用户选中的常用分拍线集合。
};

/// @brief 将物件放置磁吸模式序列化为稳定文本。
void to_json(nlohmann::json& json, const ObjectPlacementSnapMode& mode);
/// @brief 从稳定文本读取物件放置磁吸模式。
void from_json(const nlohmann::json& json, ObjectPlacementSnapMode& mode);

/// @brief 常用分拍选择允许的最小分母。
inline constexpr int COMMON_BEAT_DIVISOR_MIN = 2;

/// @brief 常用分拍选择允许的最大分母。
inline constexpr int COMMON_BEAT_DIVISOR_MAX = 24;

/// @brief 常用分拍选择的有效位数量。
inline constexpr int COMMON_BEAT_DIVISOR_COUNT =
    COMMON_BEAT_DIVISOR_MAX - COMMON_BEAT_DIVISOR_MIN + 1;

/// @brief 1/2 至 1/24 常用分拍线的全部有效位。
inline constexpr std::uint32_t COMMON_BEAT_DIVISOR_MASK_ALL =
    (std::uint32_t{ 1 } << COMMON_BEAT_DIVISOR_COUNT) - 1U;

/// @brief 默认启用的常用分拍集合。
inline constexpr std::uint32_t COMMON_BEAT_DIVISOR_MASK_DEFAULT =
    (std::uint32_t{ 1 } << (2 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (3 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (4 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (6 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (8 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (12 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (16 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (24 - COMMON_BEAT_DIVISOR_MIN));

/// @brief 判断常用分拍选择中是否启用了指定分母。
/// @param mask 常用分拍选择位掩码。
/// @param divisor 待查询的分母。
/// @return 分母处于 2 至 24 且对应选择位开启时返回 true。
inline constexpr bool isCommonBeatDivisorEnabled(std::uint32_t mask,
                                                 int           divisor)
{
    // 先约束分母，避免负数或超出位宽的移位产生未定义行为。
    if ( divisor < COMMON_BEAT_DIVISOR_MIN ||
         divisor > COMMON_BEAT_DIVISOR_MAX ) {
        return false;
    }
    // 下界对应最低位，后续分母连续映射；不得按分母值直接移位。
    const auto bit = static_cast<unsigned>(divisor - COMMON_BEAT_DIVISOR_MIN);
    return (mask & (std::uint32_t{ 1 } << bit)) != 0U;
}

/// @brief 修改常用分拍选择中的指定分母状态。
/// @param mask 待修改的常用分拍选择位掩码。
/// @param divisor 待修改的分母。
/// @param enabled 是否启用该分母。
inline constexpr void setCommonBeatDivisorEnabled(std::uint32_t& mask,
                                                  int divisor, bool enabled)
{
    // 越界输入不改变已有掩码，读取旧配置和控件编辑共用此约束。
    if ( divisor < COMMON_BEAT_DIVISOR_MIN ||
         divisor > COMMON_BEAT_DIVISOR_MAX ) {
        return;
    }
    const auto bit  = static_cast<unsigned>(divisor - COMMON_BEAT_DIVISOR_MIN);
    const auto flag = std::uint32_t{ 1 } << bit;
    // 仅修改目标位，保留其他分拍选择及调用方尚未处理的位。
    if ( enabled ) {
        mask |= flag;
    } else {
        mask &= ~flag;
    }
}

}  // namespace MMM::Config
