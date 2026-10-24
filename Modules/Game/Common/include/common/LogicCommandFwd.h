#pragma once

#include <variant>

namespace MMM::Logic
{

/// @file LogicCommandFwd.h
/// @brief 命令类型与有序变体声明，不引入配置、谱面及音轨载荷定义。
/// @details 只接收命令引用的接口使用本头；构造、销毁、访问或分派命令时必须
/// 显式包含 LogicCommands.h，使 variant 的所有备选类型完整。
/// 备选顺序是现有类型身份的一部分，移动声明不能调整顺序或增加哨兵类型。

/// @brief 更新编辑器配置指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateEditorConfig;

/// @brief 更新视口尺寸指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateViewport;

/// @brief 设置播放状态指令，完整载荷定义位于 LogicCommands.h。
struct CmdSetPlayState;

/// @brief 加载新谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdLoadBeatmap;

/// @brief 新建谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdCreateBeatmap;

/// @brief 设置悬停实体指令，完整载荷定义位于 LogicCommands.h。
struct CmdSetHoveredEntity;

/// @brief 选择实体指令，完整载荷定义位于 LogicCommands.h。
struct CmdSelectEntity;

/// @brief 开始拖拽指令，完整载荷定义位于 LogicCommands.h。
struct CmdStartDrag;

/// @brief 更新拖拽位置指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateDrag;

/// @brief 结束拖拽指令，完整载荷定义位于 LogicCommands.h。
struct CmdEndDrag;

/// @brief 将项目音频资源作为自动采样放入主画布 BGM 轨道区，完整载荷定义位于
/// LogicCommands.h。
struct CmdCreateAudioSample;

/// @brief 原子更新一个自动采样的精确属性，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateAudioSampleProperties;

/// @brief 将单个谱面物件移动到精确秒时间戳，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateObjectTimestamp;

/// @brief 更新主画布单个玩家绑定或自动采样的物件音量，完整载荷定义位于
/// LogicCommands.h。
struct CmdUpdateObjectSampleVolume;

/// @brief 批量更新当前选中物件所含音频绑定的音量，完整载荷定义位于
/// LogicCommands.h。
struct CmdUpdateSelectedObjectSampleVolume;

/// @brief 更新轨道数量指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateTrackCount;

/// @brief 更新持久化 BGM 轨道数量指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateBgmTrackCount;

/// @brief 更新当前谱面独占草稿区的持久化轨道数量，完整载荷定义位于
/// LogicCommands.h。
struct CmdUpdateDraftTrackCount;

/// @brief 跳转时间指令，完整载荷定义位于 LogicCommands.h。
struct CmdSeek;

/// @brief 在逻辑线程一次性截取指定教学段落的正式轨道根物件，完整载荷定义位于
/// LogicCommands.h。
struct CmdCaptureComposeLessonNotes;

/// @brief 只对目标谱面会话生效的创作教学输入范围，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetComposeLessonInputLimit;

/// @brief 设置播放速度指令，完整载荷定义位于 LogicCommands.h。
struct CmdSetPlaybackSpeed;

/// @brief 设置全局预览播放音高，不修改谱面资源配置，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetPlaybackPitch;

/// @brief 设置单条玩家、草稿或 BGM 轨道的运行时 Key
/// 音静音状态，完整载荷定义位于 LogicCommands.h。
struct CmdSetKeySoundTrackMute;

/// @brief 设置单条玩家、草稿或 BGM 轨道的运行时 Key 音增益，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetKeySoundTrackGain;

/// @brief 设置一类玩家打击音效的实时线性增益，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetKeySoundEffectGroupGain;

/// @brief 设置整个草稿轨道区的运行时 Key 音静音状态，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetDraftKeySoundAreaMute;

/// @brief 设置整个 BGM 轨道区的运行时 Key 音静音状态，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetBgmKeySoundAreaMute;

/// @brief 切换编辑工具指令，完整载荷定义位于 LogicCommands.h。
struct CmdChangeTool;

/// @brief 设置鼠标在视口中的位置指令，完整载荷定义位于 LogicCommands.h。
struct CmdSetMousePosition;

/// @brief 撤销指令，完整载荷定义位于 LogicCommands.h。
struct CmdUndo;

/// @brief 重做指令，完整载荷定义位于 LogicCommands.h。
struct CmdRedo;

/// @brief 复制指令，完整载荷定义位于 LogicCommands.h。
struct CmdCopy;

/// @brief 粘贴指令，完整载荷定义位于 LogicCommands.h。
struct CmdPaste;

/// @brief 剪切指令，完整载荷定义位于 LogicCommands.h。
struct CmdCut;

/// @brief 删除选中物件指令，完整载荷定义位于 LogicCommands.h。
struct CmdDeleteSelected;

/// @brief
/// 删除创作教学红框指定的错误根物件，避免普通撤销误伤其它编辑，完整载荷定义位于
/// LogicCommands.h。
struct CmdRemoveComposeLessonNote;

/// @brief 镜像选中物件指令，完整载荷定义位于 LogicCommands.h。
struct CmdMirrorSelected;

/// @brief 对齐选中物件至常用分拍指令，完整载荷定义位于 LogicCommands.h。
struct CmdAlignSelectedToCommonBeats;

/// @brief 全选指令，完整载荷定义位于 LogicCommands.h。
struct CmdSelectAll;

/// @brief 设置画笔当前使用的自定义音符颜色，完整载荷定义位于 LogicCommands.h。
struct CmdSetBrushNoteColor;

/// @brief 将自定义音符颜色应用到当前选中物件，完整载荷定义位于
/// LogicCommands.h。
struct CmdApplyNoteColorToSelection;

/// @brief
/// 设置颜色画笔使用的完整方案，不把方案值视为物件显式染色，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetBrushNotePalette;

/// @brief 设置画笔新建物件使用的项目音频资源，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetBrushAudioResource;

/// @brief 将完整音符调色盘应用到当前选中物件，完整载荷定义位于
/// LogicCommands.h。
struct CmdApplyNotePaletteToSelection;

/// @brief 将当前画笔调色盘应用到指定音符物件，完整载荷定义位于
/// LogicCommands.h。
struct CmdApplyBrushPaletteToEntity;

/// @brief 清除指定音符物件的自定义配色覆写，完整载荷定义位于 LogicCommands.h。
struct CmdClearNoteColorOverrides;

/// @brief 清除当前谱面全部正式音符及折线子段的自定义颜色，完整载荷定义位于
/// LogicCommands.h。
struct CmdClearAllNoteColorOverrides;

/// @brief 保存谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdSaveBeatmap;

/// @brief 另存为谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdSaveBeatmapAs;

/// @brief 打包谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdPackBeatmap;

/// @brief 滚动指令（鼠标滚轮），完整载荷定义位于 LogicCommands.h。
struct CmdScroll;

/// @brief 按逻辑像素增量二维平移主画布，完整载荷定义位于 LogicCommands.h。
struct CmdPanCanvas;

/// @brief 更新时间线事件指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateTimelineEvent;

/// @brief 批量更新时间线事件指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateTimelineEvents;

/// @brief 删除时间线事件指令，完整载荷定义位于 LogicCommands.h。
struct CmdDeleteTimelineEvent;

/// @brief
/// 将一次选择或擦除操作涉及的时间线删除合并为单条撤销记录，完整载荷定义位于
/// LogicCommands.h。
struct CmdDeleteTimelineEvents;

/// @brief 创建时间线事件指令，完整载荷定义位于 LogicCommands.h。
struct CmdCreateTimelineEvent;

/// @brief 原子更新 BPM 并新增或更新同时间戳的保持预设流速 SV，完整载荷定义位于
/// LogicCommands.h。
struct CmdUpdateBpmWithKeepSpeedSv;

/// @brief 为选中的普通正 BPM 红线批量添加保持预设流速的 SV，完整载荷定义位于
/// LogicCommands.h。
struct CmdKeepSpeedForBpmEvents;

/// @brief 批量创建时间线事件指令，完整载荷定义位于 LogicCommands.h。
struct CmdCreateTimelineEvents;

/// @brief 批量替换当前谱面的 Timing 列表，完整载荷定义位于 LogicCommands.h。
struct CmdReplaceBeatmapTimings;

/// @brief 修改一个玩家物件或折线子物件的编辑器注释，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetNoteAnnotation;

/// @brief 新建或修改一条谱面批注，完整载荷定义位于 LogicCommands.h。
struct CmdUpsertBeatmapAnnotation;

/// @brief 删除一条谱面批注，完整载荷定义位于 LogicCommands.h。
struct CmdRemoveBeatmapAnnotation;

/// @brief
/// 使用另一个谱面作为来源，直接替换当前会话指定类别的数据，完整载荷定义位于
/// LogicCommands.h。
struct CmdReplaceBeatmapData;

/// @brief 确认协作权威文档已经接纳指定本地谱面变化，完整载荷定义位于
/// LogicCommands.h。
struct CmdAcknowledgeCollaborationMutation;

/// @brief 房主历史补偿后只调整本地动作栈，不再次执行领域编辑，完整载荷定义位于
/// LogicCommands.h。
struct CmdReconcileCollaborationHistory;

/// @brief 将已经完整校验的协作资源绑定到当前访客会话，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetCollaborationResources;

/// @brief 切换协作访客会话的离线只读状态，完整载荷定义位于 LogicCommands.h。
struct CmdSetCollaborationOfflineReadOnly;

/// @brief 切换协作会话的进程内剪贴板隔离范围，完整载荷定义位于
/// LogicCommands.h。
struct CmdSetCollaborationClipboardIsolation;

/// @brief 开始框选指令，完整载荷定义位于 LogicCommands.h。
struct CmdStartMarquee;

/// @brief 更新框选指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateMarquee;

/// @brief 结束框选指令，完整载荷定义位于 LogicCommands.h。
struct CmdEndMarquee;

/// @brief 移除指定位置的框选区域，完整载荷定义位于 LogicCommands.h。
struct CmdRemoveMarqueeAt;

/// @brief 开始画笔操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdStartBrush;

/// @brief 更新画笔操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateBrush;

/// @brief 结束画笔操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdEndBrush;

/// @brief 开始擦除操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdStartErase;

/// @brief 更新擦除操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateErase;

/// @brief 结束擦除操作指令，完整载荷定义位于 LogicCommands.h。
struct CmdEndErase;

/// @brief 更新谱面元数据指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateBeatmapMetadata;

/// @brief 标记直接修改的扩展谱面元数据，并请求尾随自动保存，完整载荷定义位于
/// LogicCommands.h。
struct CmdMarkBeatmapMetadataDirty;

/// @brief 导入音频指令，完整载荷定义位于 LogicCommands.h。
struct CmdImportAudio;

/// @brief 更新音轨资源类型指令，完整载荷定义位于 LogicCommands.h。
struct CmdUpdateAudioResource;

/// @brief 同时重命名项目音频资源的逻辑轨道 ID 与物理文件名，完整载荷定义位于
/// LogicCommands.h。
struct CmdRenameAudioResource;

/// @brief 更新项目音频资源的完整持久化 DSP 配置，完整载荷定义位于
/// LogicCommands.h。
struct CmdUpdateAudioResourceConfig;

/// @brief 移除音轨资源指令，完整载荷定义位于 LogicCommands.h。
struct CmdRemoveAudioResource;

/// @brief 移除谱面指令，完整载荷定义位于 LogicCommands.h。
struct CmdRemoveBeatmap;

/// @brief 将当前谱面导出为单谱面 RM/IMD 资源包，完整载荷定义位于
/// LogicCommands.h。
struct CmdExportImdPackage;

/// @brief 将当前临时项目保存为正式项目，完整载荷定义位于 LogicCommands.h。
struct CmdSaveTemporaryProject;

/// @brief 所有逻辑指令的原有按值变体，声明边界不改变索引与所有权。
/// @note 不完整备选类型只允许用于接口声明，不能在本头实例化命令对象。
using LogicCommand = std::variant<
    CmdUpdateEditorConfig, CmdUpdateViewport, CmdSetPlayState, CmdLoadBeatmap,
    CmdCreateBeatmap, CmdSetHoveredEntity, CmdSelectEntity, CmdStartDrag,
    CmdUpdateDrag, CmdEndDrag, CmdCreateAudioSample,
    CmdUpdateAudioSampleProperties, CmdUpdateObjectTimestamp,
    CmdUpdateObjectSampleVolume, CmdUpdateSelectedObjectSampleVolume,
    CmdUpdateTrackCount, CmdUpdateBgmTrackCount, CmdUpdateDraftTrackCount,
    CmdSeek, CmdCaptureComposeLessonNotes, CmdSetComposeLessonInputLimit,
    CmdSetPlaybackSpeed, CmdSetPlaybackPitch, CmdSetKeySoundTrackMute,
    CmdSetKeySoundTrackGain, CmdSetKeySoundEffectGroupGain,
    CmdSetDraftKeySoundAreaMute, CmdSetBgmKeySoundAreaMute, CmdChangeTool,
    CmdSetMousePosition, CmdUndo, CmdRedo, CmdCopy, CmdPaste, CmdCut,
    CmdDeleteSelected, CmdRemoveComposeLessonNote, CmdMirrorSelected,
    CmdAlignSelectedToCommonBeats, CmdSelectAll, CmdSetBrushNoteColor,
    CmdApplyNoteColorToSelection, CmdSetBrushNotePalette,
    CmdSetBrushAudioResource, CmdApplyNotePaletteToSelection,
    CmdApplyBrushPaletteToEntity, CmdClearNoteColorOverrides,
    CmdClearAllNoteColorOverrides, CmdSaveBeatmap, CmdSaveBeatmapAs,
    CmdPackBeatmap, CmdScroll, CmdPanCanvas, CmdUpdateTimelineEvent,
    CmdUpdateTimelineEvents, CmdDeleteTimelineEvent, CmdDeleteTimelineEvents,
    CmdCreateTimelineEvent, CmdUpdateBpmWithKeepSpeedSv,
    CmdKeepSpeedForBpmEvents, CmdCreateTimelineEvents, CmdReplaceBeatmapTimings,
    CmdSetNoteAnnotation, CmdUpsertBeatmapAnnotation,
    CmdRemoveBeatmapAnnotation, CmdReplaceBeatmapData,
    CmdAcknowledgeCollaborationMutation, CmdReconcileCollaborationHistory,
    CmdSetCollaborationResources, CmdSetCollaborationOfflineReadOnly,
    CmdSetCollaborationClipboardIsolation, CmdStartMarquee, CmdUpdateMarquee,
    CmdEndMarquee, CmdRemoveMarqueeAt, CmdStartBrush, CmdUpdateBrush,
    CmdEndBrush, CmdStartErase, CmdUpdateErase, CmdEndErase,
    CmdUpdateBeatmapMetadata, CmdMarkBeatmapMetadataDirty, CmdImportAudio,
    CmdUpdateAudioResource, CmdRenameAudioResource,
    CmdUpdateAudioResourceConfig, CmdRemoveAudioResource, CmdRemoveBeatmap,
    CmdExportImdPackage, CmdSaveTemporaryProject>;

}  // namespace MMM::Logic
