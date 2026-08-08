#include "logic/MalodyPackageCompatibility.h"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断 MC 节点是否为 Malody 自动音频对象。
/// @param node 待检查的 note 数组节点。
/// @return 字符串 SOUND 或数值类型 1 时返回 true。
bool isMalodyAutomaticAudioNode(const nlohmann::json& node)
{
    if ( !node.is_object() ) return false;
    const auto typeIt = node.find("type");
    if ( typeIt == node.end() ) return false;
    if ( typeIt->is_string() ) {
        return typeIt->get_ref<const std::string&>() == "SOUND";
    }
    if ( typeIt->is_number_integer() ) {
        return typeIt->get<std::int64_t>() == 1;
    }
    if ( typeIt->is_number_unsigned() ) {
        return typeIt->get<std::uint64_t>() == 1;
    }
    return false;
}

}  // namespace

namespace MMM::Logic
{

/// @brief 删除 Malody 自动主音轨对象中的 vol 字段。
/// @param document 待修改的 MC JSON 根对象。
/// @param mainAudioReferences 解析为项目 Main 音轨的资源引用集合。
/// @return 实际删除的 vol 字段数量。
/// @note 只处理 type 为 SOUND 或数值 1 的自动采样，不修改玩家物件绑定音效。
std::size_t stripMalodyMainAudioVolumeFields(
    nlohmann::json&                        document,
    const std::unordered_set<std::string>& mainAudioReferences)
{
    if ( !document.is_object() || mainAudioReferences.empty() ) return 0;
    const auto noteIt = document.find("note");
    if ( noteIt == document.end() || !noteIt->is_array() ) return 0;

    std::size_t removedCount = 0;
    for ( auto& node : *noteIt ) {
        if ( !isMalodyAutomaticAudioNode(node) ) continue;
        const auto soundIt = node.find("sound");
        if ( soundIt == node.end() || !soundIt->is_string() ) continue;
        if ( !mainAudioReferences.contains(
                 soundIt->get_ref<const std::string&>()) ) {
            continue;
        }
        removedCount += node.erase("vol");
    }
    return removedCount;
}

}  // namespace MMM::Logic
