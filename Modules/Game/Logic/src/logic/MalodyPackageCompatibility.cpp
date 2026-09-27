#include "logic/MalodyPackageCompatibility.h"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断 MC 节点是否为 Malody 自动音频对象。
/// @param node 待检查的 note 数组节点。
/// @return 字符串 SOUND 或数值类型 1 时返回 true。
/// @note 类型字段缺失或具有其他 JSON 类型时返回 false，不尝试字符串转换。
bool isMalodyAutomaticAudioNode(const nlohmann::json& node)
{
    // 只检查标准自动采样类型，不把带 sound 字段的任意玩家音符当成主音轨。
    if ( !node.is_object() ) return false;
    const auto typeIt = node.find("type");
    if ( typeIt == node.end() ) return false;
    if ( typeIt->is_string() ) {
        // 字符串按协议原样匹配，不接受大小写变体或数字字符串。
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
/// @details 本函数修改导出 JSON，不改变项目中的主音轨音量设置。
/// @note 原本没有 vol 的节点保持不变，重复调用不会继续产生改动。
/// @warning 打包兼容处理，遍历导出 note 数组，不用于会话逐帧更新。
std::size_t stripMalodyMainAudioVolumeFields(
    nlohmann::json&                        document,
    const std::unordered_set<std::string>& mainAudioReferences)
{
    if ( !document.is_object() || mainAudioReferences.empty() ) return 0;
    // 缺少可识别的目标引用时保持原文档，不猜测哪条 SOUND 是主音轨。
    const auto noteIt = document.find("note");
    if ( noteIt == document.end() || !noteIt->is_array() ) return 0;
    // 畸形或缺失 note 节点不自动修补，兼容处理不承担重建谱面的职责。

    std::size_t removedCount = 0;
    for ( auto& node : *noteIt ) {
        if ( !isMalodyAutomaticAudioNode(node) ) continue;
        const auto soundIt = node.find("sound");
        // 资源归属由调用方解析出的引用集合决定，不按路径后缀或轨道位置判断。
        if ( soundIt == node.end() || !soundIt->is_string() ) continue;
        if ( !mainAudioReferences.contains(
                 soundIt->get_ref<const std::string&>()) ) {
            continue;
        }
        // 仅移除 vol，保留时间、资源引用与所有其他扩展字段。
        removedCount += node.erase("vol");
        // erase 返回实际移除数量，原本没有 vol 的目标不计作改动。
    }
    // 返回字段数而非 SOUND 数，调用方可据此判断是否需要报告兼容调整。
    return removedCount;
}

}  // namespace MMM::Logic
