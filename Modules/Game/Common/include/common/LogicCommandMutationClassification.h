#pragma once

#include "common/LogicCommands.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"

namespace MMM::Logic
{
/// @brief 返回本地协作权限门闩处理该命令所需的谱面数据类别。
/// @param command 待检查的逻辑命令。
/// @return 不修改谱面，或属于已由开始命令授权的连续交互时返回 None。
/// @warning 命令入队热路径：只执行一次 variant 类型分派，不访问 ECS、谱面或
/// 文件系统；无法在入队边界确定选择内容的命令会保守要求全部潜在类别。
[[nodiscard]] inline ::MMM::BeatmapMutationFlags requiredBeatmapMutationFlags(
    const LogicCommand& command)
{
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr ( std::is_same_v<T, CmdReplaceBeatmapData> ) {
                if ( value.authoritativeRemote ) {
                    return ::MMM::BeatmapMutationFlags::None;
                }
                auto flags = ::MMM::BeatmapMutationFlags::None;
                if ( value.replaceObjects ) {
                    flags |= ::MMM::BeatmapMutationFlags::Objects;
                }
                if ( value.replaceTimelines ) {
                    flags |= ::MMM::BeatmapMutationFlags::Timelines;
                }
                if ( value.replaceMetadata ) {
                    flags |= ::MMM::BeatmapMutationFlags::Metadata;
                }
                if ( value.replaceAudioSamples ) {
                    flags |= ::MMM::BeatmapMutationFlags::AudioSamples;
                }
                if ( value.replaceAnnotations ) {
                    flags |= ::MMM::BeatmapMutationFlags::Annotations;
                }
                return flags;
            } else if constexpr ( std::is_same_v<T, CmdSetNoteAnnotation> ||
                                  std::is_same_v<T,
                                                 CmdUpsertBeatmapAnnotation> ||
                                  std::is_same_v<T,
                                                 CmdRemoveBeatmapAnnotation> ) {
                return ::MMM::BeatmapMutationFlags::Annotations;
            } else if constexpr ( std::is_same_v<T, CmdStartDrag> ) {
                return value.kind == ChartObjectKind::AudioSample
                           ? ::MMM::BeatmapMutationFlags::AudioSamples
                           : ::MMM::BeatmapMutationFlags::Objects;
            } else if constexpr ( std::is_same_v<T, CmdCreateAudioSample> ||
                                  std::is_same_v<
                                      T,
                                      CmdUpdateAudioSampleProperties> ) {
                return ::MMM::BeatmapMutationFlags::AudioSamples;
            } else if constexpr ( std::is_same_v<
                                      T,
                                      CmdUpdateObjectSampleVolume> ) {
                return value.kind == ChartObjectKind::AudioSample
                           ? ::MMM::BeatmapMutationFlags::AudioSamples
                           : ::MMM::BeatmapMutationFlags::Objects;
            } else if constexpr ( std::is_same_v<T, CmdStartBrush> ||
                                  std::is_same_v<T, CmdUpdateBrush> ||
                                  std::is_same_v<T, CmdEndBrush> ||
                                  std::is_same_v<T, CmdStartErase> ||
                                  std::is_same_v<T, CmdUpdateErase> ||
                                  std::is_same_v<T, CmdEndErase> ) {
                // 画笔和橡皮擦可在玩家物件与 BGM 自动采样之间切换，具体类别
                // 必须结合会话中的轨道投影或当前手势状态判断。
                return ::MMM::BeatmapMutationFlags::None;
            } else if constexpr (
                std::is_same_v<T, CmdMirrorSelected> ||
                std::is_same_v<T, CmdAlignSelectedToCommonBeats> ||
                std::is_same_v<T, CmdApplyNoteColorToSelection> ||
                std::is_same_v<T, CmdApplyNotePaletteToSelection> ||
                std::is_same_v<T, CmdApplyBrushPaletteToEntity> ||
                std::is_same_v<T, CmdClearNoteColorOverrides> ) {
                return ::MMM::BeatmapMutationFlags::Objects;
            } else if constexpr ( std::is_same_v<T, CmdUpdateTimelineEvent> ||
                                  std::is_same_v<T, CmdUpdateTimelineEvents> ||
                                  std::is_same_v<T,
                                                 CmdUpdateBpmWithKeepSpeedSv> ||
                                  std::is_same_v<T, CmdDeleteTimelineEvent> ||
                                  std::is_same_v<T, CmdCreateTimelineEvent> ||
                                  std::is_same_v<T, CmdCreateTimelineEvents> ||
                                  std::is_same_v<T,
                                                 CmdReplaceBeatmapTimings> ) {
                return ::MMM::BeatmapMutationFlags::Timelines;
            } else if constexpr ( std::is_same_v<T,
                                                 CmdUpdateBeatmapMetadata> ) {
                return ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T,
                                                 CmdMarkBeatmapMetadataDirty> ||
                                  std::is_same_v<T, CmdUpdateBgmTrackCount> ) {
                return std::is_same_v<T, CmdUpdateBgmTrackCount>
                           ? ::MMM::BeatmapMutationFlags::Metadata |
                                 ::MMM::BeatmapMutationFlags::AudioSamples
                           : ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                return ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T, CmdUndo> ||
                                  std::is_same_v<T, CmdRedo> ) {
                return ::MMM::BeatmapMutationFlags::None;
            } else if constexpr ( std::is_same_v<T, CmdPaste> ||
                                  std::is_same_v<T, CmdCut> ||
                                  std::is_same_v<T, CmdDeleteSelected> ||
                                  std::is_same_v<
                                      T,
                                      CmdUpdateSelectedObjectSampleVolume> ) {
                return ::MMM::BeatmapMutationFlags::None;
            } else {
                return ::MMM::BeatmapMutationFlags::None;
            }
        },
        command);
}
}  // namespace MMM::Logic
