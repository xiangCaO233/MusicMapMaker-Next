#pragma once

#include "common/LogicCommands.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"

namespace MMM::Logic
{
/// @brief 返回本地协作权限门闩处理该命令所需的谱面数据类别。
/// @param command 待检查的逻辑命令。
/// @return 不修改谱面，或属于已由开始命令授权的连续交互时返回 None。
/// @note 返回值供本地协作权限门闩使用，不替代命令执行时的业务校验。
/// @warning 命令入队热路径：只执行一次 variant 类型分派，不访问 ECS、谱面或
/// 文件系统；无法在入队边界确定选择内容的命令会保守要求全部潜在类别。
[[nodiscard]] inline ::MMM::BeatmapMutationFlags requiredBeatmapMutationFlags(
    const LogicCommand& command)
{
    // std::visit 在编译期为每个命令类型选择分支，不复制 variant 载荷。
    // 静态分类只读取命令自身字段，需要会话数据的类别由消费阶段补充。
    return std::visit(
        [](const auto& value) {
            // 去除引用和 const，便于使用 is_same_v 精确匹配命令 DTO。
            using T = std::decay_t<decltype(value)>;
            if constexpr ( std::is_same_v<T, CmdReplaceBeatmapData> ) {
                // 权威远端替换已在协作协议层完成鉴权，本地门闩不得再次拦截。
                if ( value.authoritativeRemote ) {
                    return ::MMM::BeatmapMutationFlags::None;
                }
                // 替换命令可只更新部分数据域，按实际开关累积所需权限。
                auto flags = ::MMM::BeatmapMutationFlags::None;
                if ( value.replaceObjects ) {
                    // 玩家物件替换需要 Objects 权限。
                    flags |= ::MMM::BeatmapMutationFlags::Objects;
                }
                if ( value.replaceTimelines ) {
                    // BPM、SV 等时间线替换需要 Timelines 权限。
                    flags |= ::MMM::BeatmapMutationFlags::Timelines;
                }
                if ( value.replaceMetadata ) {
                    // 标题、作者等谱面字段替换需要 Metadata 权限。
                    flags |= ::MMM::BeatmapMutationFlags::Metadata;
                }
                if ( value.replaceAudioSamples ) {
                    // 自动采样集合替换需要 AudioSamples 权限。
                    flags |= ::MMM::BeatmapMutationFlags::AudioSamples;
                }
                if ( value.replaceAnnotations ) {
                    // 谱面及物件批注替换需要 Annotations 权限。
                    flags |= ::MMM::BeatmapMutationFlags::Annotations;
                }
                // 无替换开关时保持 None，使纯同步消息可以通过本地门闩。
                return flags;
            } else if constexpr ( std::is_same_v<T, CmdSetNoteAnnotation> ||
                                  std::is_same_v<T,
                                                 CmdUpsertBeatmapAnnotation> ||
                                  std::is_same_v<T,
                                                 CmdRemoveBeatmapAnnotation> ) {
                // 音符批注和谱面批注的新增、更新、删除统一属于批注数据域。
                // 具体目标存在性由命令处理器验证，不在入队边界访问 ECS。
                return ::MMM::BeatmapMutationFlags::Annotations;
            } else if constexpr ( std::is_same_v<T, CmdStartDrag> ) {
                // 拖拽开始携带稳定对象领域，可在不访问注册表时完成分类。
                // 自动采样与可操作音符分别受 AudioSamples 和 Objects 控制。
                return value.kind == ChartObjectKind::AudioSample
                           ? ::MMM::BeatmapMutationFlags::AudioSamples
                           : ::MMM::BeatmapMutationFlags::Objects;
            } else if constexpr ( std::is_same_v<T, CmdCreateAudioSample> ||
                                  std::is_same_v<
                                      T,
                                      CmdUpdateAudioSampleProperties> ) {
                // 创建采样和修改采样属性都会直接改变音频时间线对象。
                // 两者目标领域明确，无需读取当前采样注册表即可授权。
                return ::MMM::BeatmapMutationFlags::AudioSamples;
            } else if constexpr ( std::is_same_v<
                                      T,
                                      CmdUpdateObjectSampleVolume> ) {
                // 单对象音量命令同时支持音符与自动采样，必须依据对象领域分类。
                // 此处只读取命令字段，不解析实体或当前轨道布局。
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
                // 开始命令在会话层确定权限后，连续更新和结束命令沿用该授权。
                // 因此入队边界返回 None，避免在手势中途重复阻断已开始的操作。
                return ::MMM::BeatmapMutationFlags::None;
            } else if constexpr (
                std::is_same_v<T, CmdMirrorSelected> ||
                std::is_same_v<T, CmdAlignSelectedToCommonBeats> ||
                std::is_same_v<T, CmdApplyNoteColorToSelection> ||
                std::is_same_v<T, CmdApplyNotePaletteToSelection> ||
                std::is_same_v<T, CmdApplyBrushPaletteToEntity> ||
                std::is_same_v<T, CmdClearNoteColorOverrides> ) {
                // 镜像、对齐和局部配色都只修改玩家物件几何或显示属性。
                // 选择集内容在执行阶段解析，但不会跨出 Objects 数据域。
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
                // 单条和批量时间事件的增删改都属于 Timelines 数据域。
                // 保速 SV 调整和整体 timing 替换也必须使用同一权限门闩。
                return ::MMM::BeatmapMutationFlags::Timelines;
            } else if constexpr ( std::is_same_v<T,
                                                 CmdUpdateBeatmapMetadata> ) {
                // 静态最低要求是 Metadata；会迁移采样的字段由消费阶段补权。
                return ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T,
                                                 CmdMarkBeatmapMetadataDirty> ||
                                  std::is_same_v<T, CmdUpdateBgmTrackCount> ) {
                // 标记脏状态只要求 Metadata；BGM 轨数还会重排自动采样轨道。
                // 条件表达式在编译期按 T 选择，不在运行时检查 variant 类型。
                return std::is_same_v<T, CmdUpdateBgmTrackCount>
                           ? ::MMM::BeatmapMutationFlags::Metadata |
                                 ::MMM::BeatmapMutationFlags::AudioSamples
                           : ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                // 玩家轨道数量属于谱面结构元数据，物件迁移由执行器另行处理。
                // 当前存在自动采样时，消费阶段会再补充 AudioSamples 权限。
                return ::MMM::BeatmapMutationFlags::Metadata;
            } else if constexpr ( std::is_same_v<T,
                                                 CmdUpdateDraftTrackCount> ) {
                // 草稿组不属于当前 BeatMap，但仍沿用物件编辑权限进行本地门禁。
                // 该映射保持草稿编辑与正式物件编辑的协作授权体验一致。
                return ::MMM::BeatmapMutationFlags::Objects;
            } else if constexpr ( std::is_same_v<T, CmdUndo> ||
                                  std::is_same_v<T, CmdRedo> ) {
                // 撤销和重做可能跨多个数据域，真实权限由历史命令执行路径处理。
                // 入队边界缺少历史项内容，因此不能在此静态推断权限集合。
                return ::MMM::BeatmapMutationFlags::None;
            } else if constexpr ( std::is_same_v<T, CmdPaste> ||
                                  std::is_same_v<T, CmdCut> ||
                                  std::is_same_v<T, CmdDeleteSelected> ||
                                  std::is_same_v<
                                      T,
                                      CmdUpdateSelectedObjectSampleVolume> ) {
                // 剪贴板和选择集命令的实际对象领域只有会话状态能够确定。
                // 返回 None 表示延后分类，而不是声明命令不会修改谱面。
                // 执行器仍需在读取选择内容后应用对应的协作权限策略。
                return ::MMM::BeatmapMutationFlags::None;
            } else {
                // 播放、视图和工具状态等未列出的命令不需要谱面数据权限。
                // 新增会修改谱面的命令时必须在此前加入显式分类分支。
                // 默认 None
                // 仅适用于经过审查的非变更命令，不能作为新命令的豁免。
                return ::MMM::BeatmapMutationFlags::None;
            }
        },
        command);
}
}  // namespace MMM::Logic
