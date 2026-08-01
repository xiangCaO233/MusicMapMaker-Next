#pragma once

#include "event/logic/BeatmapLoadDiagnosticEvent.h"

#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 将一次谱面载入产生的 Loader 诊断转换为 UI 事件并在本次载入内去重。
/// @param beatmap 已完成解析的谱面。
/// @return 可依次发布的诊断事件；相同类型和关联路径只保留一项。
/// @warning 低频谱面载入路径：会遍历本次 Loader 诊断并分配事件字符串，
/// 不允许放入每帧 update。
[[nodiscard]] std::vector<Event::BeatmapLoadDiagnosticEvent>
buildBeatmapLoadDiagnosticEvents(const MMM::BeatMap& beatmap);

/// @brief 发布一次谱面载入产生的全部用户可见诊断。
/// @param beatmap 已成功装入 Session 的谱面。
/// @warning 低频谱面载入路径：仅在 CmdLoadBeatmap 成功处理后调用一次。
void publishBeatmapLoadDiagnostics(const MMM::BeatMap& beatmap);

}  // namespace MMM::Logic
