#pragma once

#include "common/NoteColor.h"
#include "logic/ecs/components/NoteComponent.h"
#include "mmm/Metadata.h"
#include <algorithm>
#include <glm/glm.hpp>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace MMM::Logic
{

/// @brief 获取颜色槽位在 MMM note metadata 中使用的键名。
inline std::string_view noteColorMetadataKey(NoteColorSlot slot)
{
    switch ( slot ) {
    case NoteColorSlot::Tap: return "note_tap";
    case NoteColorSlot::Head: return "note_head";
    case NoteColorSlot::Hold: return "note_hold";
    case NoteColorSlot::End: return "note_end";
    case NoteColorSlot::FlickArrow: return "note_flick_arrow";
    case NoteColorSlot::Node: return "note_node";
    }
    return "note_tap";
}

/// @brief 判断颜色槽位是否属于指定物件类型。
inline bool noteColorSlotAppliesToType(::MMM::NoteType type,
                                       NoteColorSlot    slot)
{
    switch ( type ) {
    case ::MMM::NoteType::NOTE: return slot == NoteColorSlot::Tap;
    case ::MMM::NoteType::HOLD:
        return slot == NoteColorSlot::Head || slot == NoteColorSlot::Hold ||
               slot == NoteColorSlot::End;
    case ::MMM::NoteType::FLICK:
        return slot == NoteColorSlot::Head || slot == NoteColorSlot::Hold ||
               slot == NoteColorSlot::FlickArrow;
    case ::MMM::NoteType::POLYLINE:
        return slot == NoteColorSlot::Head || slot == NoteColorSlot::Hold ||
               slot == NoteColorSlot::End ||
               slot == NoteColorSlot::FlickArrow || slot == NoteColorSlot::Node;
    default: return false;
    }
}

/// @brief 将 RGBA 颜色序列化为 metadata 字符串。
inline std::string formatNoteColorValue(glm::vec4 color)
{
    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << color.r << "," << color.g
        << "," << color.b << "," << color.a;
    return oss.str();
}

/// @brief 从 metadata 字符串解析 RGBA 颜色。
inline std::optional<glm::vec4> parseNoteColorValue(std::string_view value)
{
    std::string        ownedValue(value);
    std::istringstream iss(ownedValue);
    glm::vec4          color{ 1.0f };
    char               sep0 = '\0';
    char               sep1 = '\0';
    char               sep2 = '\0';

    if ( !(iss >> color.r >> sep0 >> color.g >> sep1 >> color.b >> sep2 >>
           color.a) ) {
        return std::nullopt;
    }
    if ( sep0 != ',' || sep1 != ',' || sep2 != ',' ) return std::nullopt;

    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);
    return color;
}

/// @brief 从 NoteMetadata 获取可选自定义颜色。
inline std::optional<glm::vec4> getNoteMetadataColor(
    const ::MMM::NoteMetadata& metadata, NoteColorSlot slot)
{
    auto sourceIt = metadata.note_properties.find(::MMM::NoteMetadataType::MMM);
    if ( sourceIt == metadata.note_properties.end() ) return std::nullopt;

    if ( auto keyIt = sourceIt->second.find(noteColorMetadataKey(slot));
         keyIt != sourceIt->second.end() ) {
        return parseNoteColorValue(keyIt->second);
    }

    switch ( slot ) {
    case NoteColorSlot::Tap:
        if ( auto keyIt = sourceIt->second.find("color.note");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        if ( auto keyIt = sourceIt->second.find("color.note_head");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    case NoteColorSlot::Head:
        if ( auto keyIt = sourceIt->second.find("color.hold_flick_head");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    case NoteColorSlot::Hold:
        if ( auto keyIt = sourceIt->second.find("color.hold_body");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    case NoteColorSlot::End:
        if ( auto keyIt = sourceIt->second.find("color.hold_end");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    case NoteColorSlot::FlickArrow:
        if ( auto keyIt = sourceIt->second.find("color.flick_end");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    case NoteColorSlot::Node:
        if ( auto keyIt = sourceIt->second.find("color.polyline_node");
             keyIt != sourceIt->second.end() ) {
            return parseNoteColorValue(keyIt->second);
        }
        break;
    }
    return std::nullopt;
}

/// @brief 写入或清除 NoteMetadata 中的可选自定义颜色。
inline void setNoteMetadataColor(::MMM::NoteMetadata& metadata,
                                 NoteColorSlot        slot,
                                 std::optional<glm::vec4> color)
{
    auto key = std::string(noteColorMetadataKey(slot));
    if ( color.has_value() ) {
        metadata.note_properties[::MMM::NoteMetadataType::MMM][key] =
            formatNoteColorValue(*color);
        return;
    }

    auto sourceIt = metadata.note_properties.find(::MMM::NoteMetadataType::MMM);
    if ( sourceIt == metadata.note_properties.end() ) return;

    sourceIt->second.erase(key);
    if ( sourceIt->second.empty() ) {
        metadata.note_properties.erase(sourceIt);
    }
}

/// @brief 获取某个槽位的缓存自定义颜色。
inline std::optional<glm::vec4> getNoteColorOverride(
    const NoteColorOverrides& colors, NoteColorSlot slot)
{
    switch ( slot ) {
    case NoteColorSlot::Tap: return colors.tap;
    case NoteColorSlot::Head: return colors.head;
    case NoteColorSlot::Hold: return colors.hold;
    case NoteColorSlot::End: return colors.end;
    case NoteColorSlot::FlickArrow: return colors.flickArrow;
    case NoteColorSlot::Node: return colors.node;
    }
    return std::nullopt;
}

/// @brief 设置某个槽位的缓存自定义颜色。
inline void setNoteColorOverride(NoteColorOverrides&        colors,
                                 NoteColorSlot              slot,
                                 std::optional<glm::vec4>   color)
{
    switch ( slot ) {
    case NoteColorSlot::Tap: colors.tap = color; break;
    case NoteColorSlot::Head: colors.head = color; break;
    case NoteColorSlot::Hold: colors.hold = color; break;
    case NoteColorSlot::End: colors.end = color; break;
    case NoteColorSlot::FlickArrow: colors.flickArrow = color; break;
    case NoteColorSlot::Node: colors.node = color; break;
    }
}

/// @brief 判断颜色覆盖表是否包含任意槽位。
inline bool hasAnyNoteColorOverride(const NoteColorOverrides& colors)
{
    return colors.tap.has_value() || colors.head.has_value() ||
           colors.hold.has_value() || colors.end.has_value() ||
           colors.flickArrow.has_value() || colors.node.has_value();
}

/// @brief 读取 NoteComponent 上某个槽位的缓存自定义颜色。
inline std::optional<glm::vec4> getNoteColorOverride(
    const NoteComponent& note, NoteColorSlot slot)
{
    return getNoteColorOverride(note.m_customColors, slot);
}

/// @brief 按槽位解析颜色；没有自定义颜色时返回 fallback。
inline glm::vec4 resolveNoteColor(const NoteComponent& note,
                                  NoteColorSlot        slot,
                                  glm::vec4            fallback)
{
    if ( auto color = getNoteColorOverride(note, slot) ) return *color;
    return fallback;
}

/// @brief 将 NoteComponent 的自定义颜色同步写入 metadata。
inline void writeNoteColorOverridesToMetadata(NoteComponent& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteMetadataColor(
            note.m_metadata,
            slot,
            noteColorSlotAppliesToType(note.m_type, slot)
                ? getNoteColorOverride(note.m_customColors, slot)
                : std::nullopt);
    }
}

/// @brief 将 metadata 中的颜色解析到 NoteComponent 缓存。
inline void loadNoteColorOverridesFromMetadata(NoteComponent& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(
            note.m_customColors, slot, getNoteMetadataColor(note.m_metadata, slot));
    }
}

/// @brief 设置 NoteComponent 的自定义颜色，同时保持 metadata 同步。
inline void setNoteColorOverride(NoteComponent&            note,
                                 NoteColorSlot             slot,
                                 std::optional<glm::vec4>  color)
{
    setNoteColorOverride(note.m_customColors, slot, color);
    setNoteMetadataColor(note.m_metadata, slot, color);
}

/// @brief 将一组颜色覆盖应用到 NoteComponent，并写入 metadata。
inline void applyNoteColorOverrides(NoteComponent&              note,
                                    const NoteColorOverrides&   colors)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        if ( noteColorSlotAppliesToType(note.m_type, slot) ) {
            setNoteColorOverride(note, slot, getNoteColorOverride(colors, slot));
        } else {
            setNoteColorOverride(note, slot, std::nullopt);
        }
    }
}

}  // namespace MMM::Logic
