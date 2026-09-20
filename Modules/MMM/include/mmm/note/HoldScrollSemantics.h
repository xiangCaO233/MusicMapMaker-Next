#pragma once

#include "mmm/Metadata.h"
#include <string_view>

namespace MMM
{
/// @brief 单段原始 seg 的尾节点独立采样 HS；由 MMM 元数据持久化与同步。
/// @note 不保存原始拍位，编辑后的结束时间仍由时间戳与时长计算。
inline constexpr std::string_view HOLD_INDEPENDENT_END_HS =
    "hold_independent_end_hs";

/// @brief 按长条语义返回尾部采样 HS 的时间。
/// @param metadata 物件元数据；缺少标记的原生 Hold 保持共享头部 HS。
/// @param start 头部时间，单位由调用方保持一致。
/// @param duration 持续时间，与 start 同单位。
/// @return 原始单段 seg 返回结束时间，普通 Hold 和虚拟载体返回起点。
/// @warning 逐物件热路径：仅作透明哈希查找，不解析 JSON、不分配字符串。
inline double holdEndHsAnchor(const NoteMetadata& metadata, double start,
                              double duration)
{
    // 标记放在 MMM 域，MC 导出不会将内部语义键泄漏到游戏效果参数。
    const auto domain = metadata.note_properties.find(NoteMetadataType::MMM);
    if ( domain == metadata.note_properties.end() ) return start;
    const auto flag = domain->second.find(HOLD_INDEPENDENT_END_HS);
    // 仅接受明确启用的标记，旧谱面和其他来源不改变既有长条行为。
    return flag != domain->second.end() && flag->second == "true"
               ? start + duration
               : start;
}
}  // namespace MMM
