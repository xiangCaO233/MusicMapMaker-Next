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
/// @param slot 逻辑部件颜色槽位。
/// @return 指向静态字符串的视图，未知枚举值兼容返回 Tap 键。
/// @note 返回值是持久化字段名，不用于翻译或界面展示。
/// @warning 可在热路径调用，不创建拥有型字符串。
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
/// @param type 物件类型，而不是折线父物件类型。
/// @param slot 要检查的部件槽位。
/// @return 未知类型返回 false；折线允许全部非 Tap 部件槽位。
/// @note 这里只判断类型能力，不检查当前实例是否实际包含某种端点。
/// @warning 热路径常量分支，不访问元数据或注册表。
inline bool noteColorSlotAppliesToType(::MMM::NoteType type, NoteColorSlot slot)
{
    switch ( type ) {
    case ::MMM::NoteType::NOTE: return slot == NoteColorSlot::Tap;
    case ::MMM::NoteType::HOLD:
        return slot == NoteColorSlot::Head || slot == NoteColorSlot::Hold ||
               slot == NoteColorSlot::End;
    case ::MMM::NoteType::FLICK:
        // 滑键横向连接体复用 Hold 槽位，箭头使用独立颜色。
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
/// @param color 归一化 RGBA；各通道在写入前限制到 0～1。
/// @return 四个逗号分隔的小数，不含 JSON 外层引号。
/// @pre 通道值为有限数；clamp 不负责将 NaN 转成合法颜色。
/// @warning 编辑与保存路径使用字符串流，不得放入逐物件渲染循环。
inline std::string formatNoteColorValue(glm::vec4 color)
{
    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);

    std::ostringstream oss;
    // 固定精度使同一颜色的文本表示稳定，避免保存时出现不必要的格式变化。
    oss << std::fixed << std::setprecision(6) << color.r << "," << color.g
        << "," << color.b << "," << color.a;
    return oss.str();
}

/// @brief 从 metadata 字符串解析 RGBA 颜色。
/// @param value 含四个逗号分隔数值的字符串视图。
/// @return 提取或分隔符检查失败时为空，否则返回限幅后的颜色。
/// @note 此解析器未要求消费完整输入，不能当作严格格式验证器。
/// @note 分隔字符前后的空白沿用流提取规则，不需要调用方预先修剪。
/// @warning 加载和编辑边界使用；构造字符串流，不用于渲染热路径。
inline std::optional<glm::vec4> parseNoteColorValue(std::string_view value)
{
    std::string ownedValue(value);
    // 流需要自己的字符串存储，后续解析不依赖外部视图的生命周期。
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
    // 只在四个通道都成功读取后返回结果，不暴露半解析的颜色。

    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);
    return color;
}

/// @brief 从 NoteMetadata 获取可选自定义颜色。
/// @param metadata 原始物件元数据，只读取 MMM 来源域。
/// @param slot 要查询的部件槽位。
/// @return 缺少键或解析失败时为空，由渲染调用方提供默认颜色。
/// @note 不跨来源域搜索相似字段，避免把其他格式的颜色语义混入 MMM 覆盖。
/// @warning 可能解析字符串，仅在颜色缓存加载或编辑时调用。
inline std::optional<glm::vec4> getNoteMetadataColor(
    const ::MMM::NoteMetadata& metadata, NoteColorSlot slot)
{
    auto sourceIt = metadata.note_properties.find(::MMM::NoteMetadataType::MMM);
    if ( sourceIt == metadata.note_properties.end() ) return std::nullopt;

    if ( auto keyIt = sourceIt->second.find(noteColorMetadataKey(slot));
         keyIt != sourceIt->second.end() ) {
        // 新键优先；即使其内容无效，也不再以旧键覆盖用户的显式配置。
        return parseNoteColorValue(keyIt->second);
    }

    switch ( slot ) {
    case NoteColorSlot::Tap:
        // 旧格式中点按有两种别名，按固定优先级兼容读取。
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
/// @param metadata 原地修改的元数据，其余来源域不受影响。
/// @param slot 目标部件的新格式键。
/// @param color 为空时删除新键，有值时覆盖该键的文本表示。
/// @note 不迁移或删除旧别名；读取旧格式的兼容规则仍然独立生效。
/// @note 清除新键后，重新加载时仍可能读到保留的旧别名。
/// @warning 写入可能分配字符串和映射节点，仅用于编辑边界。
inline void setNoteMetadataColor(::MMM::NoteMetadata&     metadata,
                                 NoteColorSlot            slot,
                                 std::optional<glm::vec4> color)
{
    auto key = std::string(noteColorMetadataKey(slot));
    if ( color.has_value() ) {
        // 仅写入时按需创建 MMM 域，删除不存在的颜色不会制造空域。
        metadata.note_properties[::MMM::NoteMetadataType::MMM][key] =
            formatNoteColorValue(*color);
        return;
    }

    auto sourceIt = metadata.note_properties.find(::MMM::NoteMetadataType::MMM);
    if ( sourceIt == metadata.note_properties.end() ) return;

    sourceIt->second.erase(key);
    if ( sourceIt->second.empty() ) {
        // 只有整个来源域为空才清除它，保留其他 MMM 物件属性。
        metadata.note_properties.erase(sourceIt);
    }
}

/// @brief 获取某个槽位的缓存自定义颜色。
/// @param colors 已解析的值类型颜色缓存。
/// @param slot 目标部件。
/// @return 对应 optional 的值副本；未知槽位返回空。
/// @note 没有覆盖与覆盖成白色是不同状态，调用方不得只比较 RGB 判断。
/// @warning 逐物件热路径只访问缓存，不解析元数据。
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
/// @param colors 原地修改的缓存表。
/// @param slot 目标部件，未知值不修改任何槽位。
/// @param color 为空表示没有覆盖，不等同于透明颜色。
/// @note 此重载只改缓存；持久化需要调用带物件参数的同步重载。
/// @note 缓存赋值不再限幅，调用方应遵守统一的归一化颜色契约。
inline void setNoteColorOverride(NoteColorOverrides& colors, NoteColorSlot slot,
                                 std::optional<glm::vec4> color)
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
/// @param colors 待检查的缓存表。
/// @return 任一 optional 有值即为 true，包括全透明或白色覆盖。
/// @warning 常量级缓存查询，不按颜色数值猜测是否为默认状态。
inline bool hasAnyNoteColorOverride(const NoteColorOverrides& colors)
{
    return colors.tap.has_value() || colors.head.has_value() ||
           colors.hold.has_value() || colors.end.has_value() ||
           colors.flickArrow.has_value() || colors.node.has_value();
}

/// @brief 读取 NoteComponent 上某个槽位的缓存自定义颜色。
/// @param note 已加载颜色缓存的组件。
/// @param slot 所需部件颜色。
/// @return 不进行类型过滤，调用方应选择当前绘制部件对应的槽位。
/// @warning 热路径只读取 m_customColors，不自动补载元数据。
inline std::optional<glm::vec4> getNoteColorOverride(const NoteComponent& note,
                                                     NoteColorSlot        slot)
{
    return getNoteColorOverride(note.m_customColors, slot);
}

/// @brief 按槽位解析颜色；没有自定义颜色时返回 fallback。
/// @param note 提供自定义颜色缓存的物件。
/// @param slot 本次实际绘制部件的槽位。
/// @param fallback 由调用方选择的皮肤或默认色。
/// @return 自定义色整体替代 fallback，不进行逐通道混合。
/// @note 自定义透明度同样覆盖默认值，不继承 fallback 的 alpha。
/// @warning 渲染热路径只做 optional 查询和值复制。
inline glm::vec4 resolveNoteColor(const NoteComponent& note, NoteColorSlot slot,
                                  glm::vec4 fallback)
{
    if ( auto color = getNoteColorOverride(note, slot) ) return *color;
    return fallback;
}

/// @brief 将 NoteComponent 的自定义颜色同步写入 metadata。
/// @param note 缓存作为源、元数据作为目标的物件。
/// @note 不修改缓存本身；不适用槽位仅从新格式元数据键中清除。
/// @pre NOTE_COLOR_SLOT_COUNT 覆盖从零开始连续排列的所有槽位枚举。
/// @warning 保存或编辑时遍历固定槽位，并可能格式化字符串。
inline void writeNoteColorOverridesToMetadata(NoteComponent& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteMetadataColor(
            note.m_metadata,
            slot,
            // 类型过滤在写入边界统一执行，避免保存不存在的部件新键。
            noteColorSlotAppliesToType(note.m_type, slot)
                ? getNoteColorOverride(note.m_customColors, slot)
                : std::nullopt);
    }
}

/// @brief 将折线子物件的自定义颜色同步写入 metadata。
/// @param note 需要同步颜色 metadata 的子物件数据。
/// @note 按子物件自身类型过滤，而不是无条件继承父折线的槽位集合。
inline void writeNoteColorOverridesToMetadata(NoteComponent::SubNote& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteMetadataColor(note.metadata,
                             slot,
                             noteColorSlotAppliesToType(note.type, slot)
                                 ? getNoteColorOverride(note.customColors, slot)
                                 : std::nullopt);
    }
}

/// @brief 将 metadata 中的颜色解析到 NoteComponent 缓存。
/// @param note 元数据作为源、缓存作为目标的物件。
/// @note 完整覆盖全部槽位，缺失或无效键会清空旧缓存中的对应颜色。
/// @note 不会反向改写读取到的旧别名，加载不是格式迁移操作。
/// @warning 仅在加载或元数据变更边界解析，不放入逐帧渲染。
inline void loadNoteColorOverridesFromMetadata(NoteComponent& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(note.m_customColors,
                             slot,
                             getNoteMetadataColor(note.m_metadata, slot));
    }
}

/// @brief 将 metadata 中的颜色解析到折线子物件缓存。
/// @param note 需要解析颜色 metadata 的子物件数据。
/// @note 读取阶段不按类型删除历史颜色，过滤留给应用或写回流程。
inline void loadNoteColorOverridesFromMetadata(NoteComponent::SubNote& note)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(
            note.customColors, slot, getNoteMetadataColor(note.metadata, slot));
    }
}

/// @brief 设置 NoteComponent 的自定义颜色，同时保持 metadata 同步。
/// @param note 同时修改缓存和元数据的目标物件。
/// @param slot 要修改的槽位，此入口不进行类型适用性检查。
/// @param color 可选覆盖色，空值清除缓存和新格式键。
/// @note 此处不通知编辑历史或标记会话脏状态，由外层编辑指令负责。
/// @warning 编辑路径调用，元数据写入可能分配内存。
inline void setNoteColorOverride(NoteComponent& note, NoteColorSlot slot,
                                 std::optional<glm::vec4> color)
{
    setNoteColorOverride(note.m_customColors, slot, color);
    setNoteMetadataColor(note.m_metadata, slot, color);
}

/// @brief 设置折线子物件的自定义颜色，同时保持 metadata 同步。
/// @param note 需要修改的折线子物件。
/// @param slot 颜色槽位。
/// @param color 自定义颜色；为空时清除该槽位。
/// @note 子物件的缓存与 metadata 一起更新，避免下一次实体重建丢失编辑。
inline void setNoteColorOverride(NoteComponent::SubNote&  note,
                                 NoteColorSlot            slot,
                                 std::optional<glm::vec4> color)
{
    setNoteColorOverride(note.customColors, slot, color);
    setNoteMetadataColor(note.metadata, slot, color);
}

/// @brief 将一组颜色覆盖应用到 NoteComponent，并写入 metadata。
/// @param note 被替换颜色方案的目标物件。
/// @param colors 完整颜色方案，不是只含修改项的增量补丁。
/// @note 目标中存在、方案中缺失的覆盖也会被清除，不能用它局部追加颜色。
/// @warning 编辑路径遍历固定槽位并同步文本，不用于每帧调色。
inline void applyNoteColorOverrides(NoteComponent&            note,
                                    const NoteColorOverrides& colors)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        if ( noteColorSlotAppliesToType(note.m_type, slot) ) {
            setNoteColorOverride(
                note, slot, getNoteColorOverride(colors, slot));
        } else {
            // 不适用的旧缓存也要清空，防止类型转换后遗留颜色再次出现。
            setNoteColorOverride(note, slot, std::nullopt);
        }
    }
}

/// @brief 将一组颜色覆盖应用到折线子物件，并写入 metadata。
/// @param note 需要修改的折线子物件。
/// @param colors 新的颜色覆盖表。
/// @note 按子物件自身类型替换全部槽位，方案中的空值同样会覆盖旧值。
inline void applyNoteColorOverrides(NoteComponent::SubNote&   note,
                                    const NoteColorOverrides& colors)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        if ( noteColorSlotAppliesToType(note.type, slot) ) {
            setNoteColorOverride(
                note, slot, getNoteColorOverride(colors, slot));
        } else {
            setNoteColorOverride(note, slot, std::nullopt);
        }
    }
}

/// @brief 将折线子物件数据转换为可注册的 NoteComponent。
/// @param sub 子物件数据源。
/// @param isSubNote 是否作为折线子实体注册。
/// @param parentPolyline 父折线实体。
/// @param subIndex 子物件索引。
/// @return 带完整颜色缓存和 metadata 的音符组件。
/// @note 仅返回值对象，不创建 entt 实体，也不验证父实体是否仍然有效。
/// @pre parentPolyline 和 subIndex 应由调用方按实际注册关系提供。
/// @note 不调用写回函数，输出元数据保留输入内容和键格式。
/// @note 颜色缓存为空时才解析元数据，兼容尚未经过颜色缓存加载的子物件。
/// @warning 子实体建立或重建时调用，复制元数据不适合逐帧绘制。
inline NoteComponent makeNoteComponentFromSubNote(
    const NoteComponent::SubNote& sub, bool isSubNote,
    entt::entity parentPolyline, int subIndex)
{
    NoteComponent::SubNote normalizedSub = sub;
    // 在局部副本上补缓存，保持调用方子物件数据不变。
    if ( !hasAnyNoteColorOverride(normalizedSub.customColors) ) {
        // 已有任意缓存覆盖就整体信任缓存，避免逐槽混入过时元数据颜色。
        loadNoteColorOverridesFromMetadata(normalizedSub);
    }

    NoteComponent note;
    // 几何、绑定和颜色一并转移，父关系则显式采用调用方提供的注册信息。
    note.m_type           = normalizedSub.type;
    note.m_timestamp      = normalizedSub.timestamp;
    note.m_duration       = normalizedSub.duration;
    note.m_trackIndex     = normalizedSub.trackIndex;
    note.m_dtrack         = normalizedSub.dtrack;
    note.m_metadata       = normalizedSub.metadata;
    note.m_sampleBinding  = normalizedSub.sampleBinding;
    note.m_customColors   = normalizedSub.customColors;
    note.m_isSubNote      = isSubNote;
    note.m_parentPolyline = parentPolyline;
    note.m_subIndex       = subIndex;
    return note;
}

}  // namespace MMM::Logic
