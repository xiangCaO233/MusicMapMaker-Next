#pragma once

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_set>

namespace MMM::Logic
{

/// @brief 删除 Malody 自动主音轨对象中的 vol 字段。
/// @param document 待修改的 MC JSON 根对象。
/// @param mainAudioReferences 解析为项目 Main 音轨的资源引用集合。
/// @return 实际删除的 vol 字段数量。
/// @note 只处理 type 为 SOUND 或数值 1 的自动采样，不修改玩家物件绑定音效。
std::size_t stripMalodyMainAudioVolumeFields(
    nlohmann::json&                        document,
    const std::unordered_set<std::string>& mainAudioReferences);

}  // namespace MMM::Logic
