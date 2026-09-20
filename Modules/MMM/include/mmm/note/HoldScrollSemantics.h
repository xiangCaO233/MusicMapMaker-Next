#pragma once

#include "mmm/Metadata.h"
#include <string_view>

namespace MMM
{
/// @brief 根长条的尾节点是否独立采样 HS；由 MMM 元数据持久化与同步。
/// @note 不保存原始拍位，编辑后的结束时间仍由时间戳与时长计算。
inline constexpr std::string_view HOLD_INDEPENDENT_END_HS =
    "hold_independent_end_hs";

/// @brief 按长条语义返回尾部采样 HS 的时间。
/// @param metadata 物件元数据；原生根 Hold 默认与 Slide 单段 seg 使用相同语义。
/// @param start 头部时间，单位由调用方保持一致。
/// @param duration 持续时间，与 start 同单位。
/// @return 根长条默认返回结束时间；显式共享的 endbeat 返回起点。
/// @warning 逐物件热路径：仅作透明哈希查找，不解析 JSON、不分配字符串。
inline double holdEndHsAnchor(const NoteMetadata& metadata, double start,
                              double duration)
{
    // 标记放在 MMM 域，MC 导出不会将内部语义键泄漏到游戏效果参数。
    const auto domain = metadata.note_properties.find(NoteMetadataType::MMM);
    if ( domain == metadata.note_properties.end() ) return start + duration;
    const auto flag = domain->second.find(HOLD_INDEPENDENT_END_HS);
    // 无标记的原生长条会导出为 seg，不能在编辑器中继续共享头部倍率。
    // endbeat 来源显式保存 false；折线虚拟载体由调用方保持头部锚点。
    return flag != domain->second.end() && flag->second == "false"
               ? start
               : start + duration;
}
}  // namespace MMM
