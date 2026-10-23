#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_set>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 写出包含首拍兼容处理的 MC，必要时为主音轨生成前置静音副本。
/// @param beatMap 只读导出模型，源音频与编辑模型保持原样。
/// @param outputPath MC 输出路径，新增音频与它放在同一目录。
/// @param mainAudioSource 主音轨实际路径；为空时按源谱面歌曲提示解析。
/// @param deferAudioEncoding MCZ 已统一安排音频编码时设为真。
/// @param errorMessage 失败原因，成功时为空。
/// @param exportedMainAudio 可选接收最终 MC 引用的主音轨路径。
/// @return 谱面与必要的音频副本均写出成功时返回真。
/// @note 文件保存与打包使用同一份 MC 文档及前导毫秒值。
/// @warning
/// 低频导出路径：可能完整解码、重新编码并写入音频，禁止从常态更新调用。
[[nodiscard]] bool writeMalodyExportWithAudio(
    const BeatMap& beatMap, const std::filesystem::path& outputPath,
    const std::filesystem::path& mainAudioSource, bool deferAudioEncoding,
    std::string&           errorMessage,
    std::filesystem::path* exportedMainAudio = nullptr);

/// @brief 删除 Malody 自动主音轨对象中的 vol 字段。
/// @param document 待修改的 MC JSON 根对象。
/// @param mainAudioReferences 解析为项目 Main 音轨的资源引用集合。
/// @return 实际删除的 vol 字段数量。
/// @note 只处理 type 为 SOUND 或数值 1 的自动采样，不修改玩家物件绑定音效。
std::size_t stripMalodyMainAudioVolumeFields(
    nlohmann::json&                        document,
    const std::unordered_set<std::string>& mainAudioReferences);

}  // namespace MMM::Logic
