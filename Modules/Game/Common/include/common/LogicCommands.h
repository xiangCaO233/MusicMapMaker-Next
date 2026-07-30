#pragma once

#include "common/ChartObjectKind.h"
#include "common/EditTool.h"
#include "common/NoteColor.h"
#include "config/EditorConfig.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/AudioResource.h"
#include "mmm/timing/Timing.h"
#include <array>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// 前置声明
namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/**
 * @brief 更新编辑器配置指令
 */
struct CmdUpdateEditorConfig {
    Config::EditorConfig config;
};

/**
 * @brief 更新视口尺寸指令
 */
struct CmdUpdateViewport {
    std::string cameraId;
    float       width;
    float       height;
};

/**
 * @brief 设置播放状态指令
 */
struct CmdSetPlayState {
    bool isPlaying;
};

/**
 * @brief 加载新谱面指令
 */
struct CmdLoadBeatmap {
    std::shared_ptr<MMM::BeatMap> beatmap;
};

/**
 * @brief 设置悬停实体指令
 */
struct CmdSetHoveredEntity {
    entt::entity entity;    // 如果为 entt::null 则表示取消悬停
    uint8_t      part;      // 悬停的具体部位 (HoverPart)
    int          subIndex;  // 悬停的具体子索引
    /// @brief 实体所在的独立 ECS 注册表。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };
};

/**
 * @brief 选择实体指令
 */
struct CmdSelectEntity {
    entt::entity entity;
    bool         clearOthers;
    /// @brief 实体所在的独立 ECS 注册表。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };
};

/**
 * @brief 开始拖拽指令
 */
struct CmdStartDrag {
    entt::entity entity;
    std::string  cameraId;
    bool         isCtrlDown{ false };
    /// @brief 实体所在的独立 ECS 注册表。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };
};

/**
 * @brief 更新拖拽位置指令
 */
struct CmdUpdateDrag {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
    bool        isCtrlDown{ false };
};

/**
 * @brief 结束拖拽指令
 */
struct CmdEndDrag {
    std::string cameraId;
};

/// @brief 将项目音频资源作为自动采样放入主画布 BGM 轨道区。
struct CmdCreateAudioSample {
    /// @brief 项目内稳定音频资源 ID。
    std::string audioResourceId;

    /// @brief 接收放置操作的主画布 ID。
    std::string cameraId;

    /// @brief 鼠标相对画布的横坐标。
    float mouseX{ 0.0F };

    /// @brief 鼠标相对画布的纵坐标。
    float mouseY{ 0.0F };

    /// @brief 是否临时禁用分拍吸附。
    bool isCtrlDown{ false };
};

/// @brief 原子更新一个自动采样的精确属性。
struct CmdUpdateAudioSampleProperties {
    /// @brief 自动采样在独立 Registry 中的实体。
    entt::entity entity{ entt::null };

    /// @brief 项目资源 ID 或待兼容解析的已有资源引用。
    std::string audioResourceId;

    /// @brief 相对玩家轨道区的零基 BGM 轨道索引。
    std::int32_t bgmLane{ 0 };

    /// @brief 相对锚点的有符号毫秒偏移。
    std::int64_t offsetMs{ 0 };

    /// @brief 自动采样物件音量。
    float volume{ 1.0F };
};

/**
 * @brief 设置鼠标在视口中的位置指令
 */
struct CmdSetMousePosition {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
    float       viewportWidth{ 0.0f };   ///< 视口宽度 (用于边缘滚动计算)
    float       viewportHeight{ 0.0f };  ///< 视口高度 (用于边缘滚动计算)
    bool        isHovering;
    bool        isDragging{ false };
    double      hoverTime{ -1.0 };  ///< 可选：直接指定悬停时间 (如果 >= 0)
};

/**
 * @brief 开始框选指令
 */
struct CmdStartMarquee {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
    bool        isCtrlDown{ false };
};

/**
 * @brief 更新框选指令
 */
struct CmdUpdateMarquee {
    float mouseX;
    float mouseY;
};

/**
 * @brief 结束框选指令
 */
struct CmdEndMarquee {
};

/**
 * @brief 移除指定位置的框选区域
 */
struct CmdRemoveMarqueeAt {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
};

/**
 * @brief 开始画笔操作指令
 */
struct CmdStartBrush {
    std::string cameraId;     ///< 发起画笔操作的视口 ID
    float       mouseX;       ///< 鼠标相对于视口的 X 坐标
    float       mouseY;       ///< 鼠标相对于视口的 Y 坐标
    bool        isShiftDown;  ///< 当前 Shift 键是否按下 (用于创建 Hold)
    bool        isCtrlDown;   ///< 当前 Ctrl 键是否按下 (用于禁用磁吸)
};

/**
 * @brief 更新画笔操作指令
 */
struct CmdUpdateBrush {
    std::string cameraId;     ///< 更新画笔操作的视口 ID
    float       mouseX;       ///< 鼠标相对于视口的 X 坐标
    float       mouseY;       ///< 鼠标相对于视口的 Y 坐标
    bool        isShiftDown;  ///< 当前 Shift 键是否按下 (用于创建 Hold)
    bool        isCtrlDown;   ///< 当前 Ctrl 键是否按下 (用于禁用磁吸)
};

/**
 * @brief 结束画笔操作指令
 */
struct CmdEndBrush {
    std::string cameraId;
};

/**
 * @brief 开始擦除操作指令
 */
struct CmdStartErase {
    std::string cameraId;
    bool        isShiftDown{ false };  ///< Shift 按下时整体删除 Polyline
};

/**
 * @brief 更新擦除操作指令
 */
struct CmdUpdateErase {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
    bool        isShiftDown{ false };  ///< Shift 按下时整体删除 Polyline
};

/**
 * @brief 结束擦除操作指令
 */
struct CmdEndErase {
    std::string cameraId;
};


/**
 * @brief 更新轨道数量指令
 */
struct CmdUpdateTrackCount {
    int32_t trackCount;
};

/// @brief 更新持久化 BGM 轨道数量指令。
struct CmdUpdateBgmTrackCount {
    int32_t bgmTrackCount;  ///< 目标持久化 BGM 轨道数量。
};

/**
 * @brief 跳转时间指令
 */
struct CmdSeek {
    double time;
};

/**
 * @brief 设置播放速度指令
 */
struct CmdSetPlaybackSpeed {
    double speed;
};

/**
 * @brief 切换编辑工具指令
 */
struct CmdChangeTool {
    EditTool tool;
};

/// @brief 设置画笔当前使用的自定义音符颜色。
struct CmdSetBrushNoteColor {
    /// @brief 要修改的音符颜色槽位。
    NoteColorSlot slot;
    /// @brief 自定义颜色；为空时清除该槽位并回退到皮肤默认色。
    std::optional<glm::vec4> color;
};

/// @brief 将自定义音符颜色应用到当前选中物件。
struct CmdApplyNoteColorToSelection {
    /// @brief 要修改的音符颜色槽位。
    NoteColorSlot slot;
    /// @brief 自定义颜色；为空时清除该槽位并回退到皮肤默认色。
    std::optional<glm::vec4> color;
};

/// @brief 设置画笔当前使用的完整音符调色盘。
struct CmdSetBrushNotePalette {
    /// @brief 完整自定义颜色表，顺序与 NoteColorSlot 一致。
    std::array<glm::vec4, NOTE_COLOR_SLOT_COUNT> colors;
};

/// @brief 设置画笔新建物件使用的项目音频资源。
struct CmdSetBrushAudioResource {
    /// @brief 项目内稳定音频资源 ID；空字符串表示清除选择。
    std::string audioResourceId;

    /// @brief 所选资源类型，用于限制玩家物件只能绑定 Effect。
    ::MMM::AudioTrackType audioTrackType{ ::MMM::AudioTrackType::Effect };
};

/// @brief 将完整音符调色盘应用到当前选中物件。
struct CmdApplyNotePaletteToSelection {
    /// @brief 完整自定义颜色表，顺序与 NoteColorSlot 一致。
    std::array<glm::vec4, NOTE_COLOR_SLOT_COUNT> colors;
};

/// @brief 将当前画笔调色盘应用到指定音符物件。
struct CmdApplyBrushPaletteToEntity {
    /// @brief 目标音符实体。
    entt::entity entity{ entt::null };
};

/// @brief 清除指定音符物件的自定义配色覆写。
struct CmdClearNoteColorOverrides {
    /// @brief 目标音符实体。
    entt::entity entity{ entt::null };
};

/**
 * @brief 撤销指令
 */
struct CmdUndo {
};

/**
 * @brief 重做指令
 */
struct CmdRedo {
};

/**
 * @brief 复制指令
 */
struct CmdCopy {
};

/**
 * @brief 粘贴指令
 */
struct CmdPaste {
    /// @brief 是否在粘贴出的物件上立即应用轨道镜像。
    bool m_mirrored{ false };

    /// @brief 是否在粘贴后清空旧选择并选中新粘贴出的物件。
    bool m_selectPastedObjects{ false };
};

/**
 * @brief 剪切指令
 */
struct CmdCut {
};

/**
 * @brief 删除选中物件指令
 */
struct CmdDeleteSelected {
};

/**
 * @brief 镜像选中物件指令
 */
struct CmdMirrorSelected {
};

/**
 * @brief 对齐选中物件至常用分拍指令
 */
struct CmdAlignSelectedToCommonBeats {
};

/**
 * @brief 全选指令
 */
struct CmdSelectAll {
};

/**
 * @brief 保存谱面指令
 */
struct CmdSaveBeatmap {
    /// @brief 是否允许覆盖哈希已变化或未知的强制 MMM 保存目标。
    bool allowExternallyModifiedOverwrite{ false };
};

/**
 * @brief 另存为谱面指令
 */
struct CmdSaveBeatmapAs {
    /// @brief 导出 MC 时是否写入上架皮肤 mode_ext。
    bool addStoreModeExtForMalodyExport{ false };

    /// @brief 导出目标路径，使用 UTF-8 编码。
    std::string path;
};

/// @brief 打包转换时临时覆盖单个谱面的基础元数据。
struct PackageBeatmapMetadataOverride {
    /// @brief 项目相对谱面路径，使用 UTF-8 编码。
    std::string relativePath;

    /// @brief 转换导出时使用的基础谱面元数据。
    MMM::BaseMapMeta baseMeta;
};

/**
 * @brief 打包谱面指令
 */
struct CmdPackBeatmap {
    /// @brief 打包输出路径，使用 UTF-8 编码。
    std::string exportPath;

    /// @brief 需要写入包内的项目相对文件路径列表，使用 UTF-8 编码。
    std::vector<std::string> selectedProjectRelativePaths;

    /// @brief 是否将转换出的目标谱面文件保存回项目目录。
    bool saveConvertedBeatmapsToProject{ false };

    /// @brief MCZ 打包时是否额外在包内写入旧皮肤兼容的 IMD 谱面。
    bool includeLegacyImdBeatmapsInPackage{ false };

    /// @brief MCZ 打包时是否为写出的 MC 谱面写入上架皮肤 mode_ext。
    bool addStoreModeExtForMalodyExport{ false };

    /// @brief 转换指定谱面时临时覆盖的元数据列表。
    std::vector<PackageBeatmapMetadataOverride> metadataOverrides;
};

/// @brief 滚轮指令的处理意图。
enum class ScrollCommandIntent {
    /// @brief 按滚轮增量移动画布时间。
    MoveTimeline,

    /// @brief 只应用“播放时滚动则停止播放”策略，不移动画布时间。
    ModifierAdjustment,
};

/// @brief 滚动指令（鼠标滚轮）。
struct CmdScroll {
    std::string cameraId;     ///< 接收滚轮的画布 ID。
    float       wheel;        ///< 滚轮增量。
    bool        isShiftDown;  ///< 是否按住 Shift 加速普通时间滚动。
    /// @brief 本条滚轮指令需要执行的逻辑意图。
    ScrollCommandIntent intent{ ScrollCommandIntent::MoveTimeline };
};

/// @brief 按逻辑像素增量二维平移主画布。
struct CmdPanCanvas {
    /// @brief 接收平移的主画布 ID。
    std::string cameraId;

    /// @brief 内容在屏幕上的横向逻辑像素位移。
    float deltaX{ 0.0F };

    /// @brief 内容在屏幕上的纵向逻辑像素位移。
    float deltaY{ 0.0F };

    /// @brief 产生本次输入时的视口逻辑宽度。
    float viewportWidth{ 0.0F };

    /// @brief 产生本次输入时的视口逻辑高度。
    float viewportHeight{ 0.0F };

    /// @brief 当前画布纵向渲染缩放。
    float renderScaleY{ 1.0F };
};

/**
 * @brief 更新时间线事件指令
 */
struct CmdUpdateTimelineEvent {
    /// @brief 待更新的 Timeline 实体。
    entt::entity entity;
    /// @brief 新时间戳，单位秒。
    double newTime;
    /// @brief 新效果参数。
    double newValue;
    /// @brief 可选的新元数据；为空时由逻辑层保留或清理旧元数据。
    std::optional<::MMM::TimingMetadata> metadataOverride;
};

/**
 * @brief 批量更新时间线事件指令。
 */
struct CmdUpdateTimelineEvents {
    /// @brief 待合并为单个撤销步骤的 Timeline 更新列表。
    std::vector<CmdUpdateTimelineEvent> events;
};

/**
 * @brief 删除时间线事件指令
 */
struct CmdDeleteTimelineEvent {
    entt::entity entity;
};

/**
 * @brief 创建时间线事件指令
 */
struct CmdCreateTimelineEvent {
    double              time;
    ::MMM::TimingEffect type;
    double              value;
};

/**
 * @brief 批量创建时间线事件指令
 */
struct CmdCreateTimelineEvents {
    /// @brief 单个待创建 Timeline 事件。
    struct Entry {
        /// @brief Timeline 时间戳，单位秒。
        double time{ 0.0 };

        /// @brief Timeline 类型。
        ::MMM::TimingEffect type{ ::MMM::TimingEffect::BPM };

        /// @brief Timeline 参数值。
        double value{ 0.0 };

        /// @brief 创建后写入 Timeline 组件的原始元数据。
        ::MMM::TimingMetadata metadata;
    };

    /// @brief 待创建 Timeline 事件列表。
    std::vector<Entry> events;
};

/**
 * @brief 批量替换当前谱面的 Timing 列表。
 */
struct CmdReplaceBeatmapTimings {
    /// @brief 替换后的 Timing 列表，时间戳单位为毫秒。
    std::vector<::MMM::Timing> timings;

    /// @brief 是否保留当前谱面中非 BPM 的流速/特效 Timing。
    bool keepNonBpmTimings{ false };
};

/// @brief 使用另一个谱面作为来源，直接替换当前会话指定类别的数据。
struct CmdReplaceBeatmapData {
    /// @brief 数据来源谱面。
    std::shared_ptr<const MMM::BeatMap> sourceBeatmap;

    /// @brief 是否替换物件数据。
    bool replaceObjects{ false };

    /// @brief 是否替换时间线数据。
    bool replaceTimelines{ false };

    /// @brief 是否替换谱面元数据。
    bool replaceMetadata{ false };
};

/**
 * @brief 从模板创建谱面时可复制的数据类别。
 */
struct BeatmapTemplateCreateOptions {
    /// @brief 是否复制模板谱面的额外谱面元数据。
    bool copyMetadata{ false };

    /// @brief 是否复制模板谱面的全部 Timing/BPM/流速事件。
    bool copyTimelines{ false };

    /// @brief 是否复制模板谱面的全部物件。
    bool copyObjects{ false };
};

/**
 * @brief 新建谱面指令
 */
struct CmdCreateBeatmap {
    /// @brief 新谱面的基础元数据。
    ::MMM::BaseMapMeta baseMeta;

    /// @brief 可选模板谱面；为空时创建空白谱面。
    std::shared_ptr<const MMM::BeatMap> templateBeatmap;

    /// @brief 从模板谱面复制的数据类别。
    BeatmapTemplateCreateOptions templateOptions;

    /// @brief 新建谱面时预置写入的 Timing 列表，时间戳单位为毫秒。
    std::vector<::MMM::Timing> initialTimings;
};

/**
 * @brief 更新谱面元数据指令
 */
struct CmdUpdateBeatmapMetadata {
    ::MMM::BaseMapMeta baseMeta;
};

/// @brief 标记直接修改的扩展谱面元数据，并请求尾随自动保存。
struct CmdMarkBeatmapMetadataDirty {
};

/**
 * @brief 导入音频指令
 */
struct CmdImportAudio {
    std::string           path;
    ::MMM::AudioTrackType trackType;
};

/**
 * @brief 更新音轨资源类型指令
 */
struct CmdUpdateAudioResource {
    std::string           id;
    ::MMM::AudioTrackType newType;
};

/// @brief 同时重命名项目音频资源的逻辑轨道 ID 与物理文件名。
struct CmdRenameAudioResource {
    /// @brief 重命名前的稳定资源 ID。
    std::string id;

    /// @brief 不含目录的目标文件名；扩展名必须与源文件一致。
    std::string newFileName;
};

/// @brief 更新项目音频资源的完整持久化 DSP 配置。
struct CmdUpdateAudioResourceConfig {
    /// @brief 待更新资源的稳定 ID。
    std::string id;

    /// @brief 替换写入的完整资源配置。
    ::MMM::AudioTrackConfig config;
};

/**
 * @brief 移除音轨资源指令
 */
struct CmdRemoveAudioResource {
    /// @brief 待删除资源的稳定 ID。
    std::string id;

    /// @brief 是否同时删除项目目录中的源音频文件。
    bool deleteSourceFile{ false };
};

/**
 * @brief 移除谱面指令
 */
struct CmdRemoveBeatmap {
    std::string filePath;
};

/// @brief 将当前临时项目保存为正式项目。
struct CmdSaveTemporaryProject {
    /// @brief 用户选择的保存目录，使用 UTF-8 编码。
    std::string destinationPath;
};

/**
 * @brief 所有可能的逻辑指令变体
 */
using LogicCommand = std::variant<
    CmdUpdateEditorConfig, CmdUpdateViewport, CmdSetPlayState, CmdLoadBeatmap,
    CmdCreateBeatmap, CmdSetHoveredEntity, CmdSelectEntity, CmdStartDrag,
    CmdUpdateDrag, CmdEndDrag, CmdCreateAudioSample,
    CmdUpdateAudioSampleProperties, CmdUpdateTrackCount, CmdUpdateBgmTrackCount,
    CmdSeek, CmdSetPlaybackSpeed, CmdChangeTool, CmdSetMousePosition, CmdUndo,
    CmdRedo, CmdCopy, CmdPaste, CmdCut, CmdDeleteSelected, CmdMirrorSelected,
    CmdAlignSelectedToCommonBeats, CmdSelectAll, CmdSetBrushNoteColor,
    CmdApplyNoteColorToSelection, CmdSetBrushNotePalette,
    CmdSetBrushAudioResource, CmdApplyNotePaletteToSelection,
    CmdApplyBrushPaletteToEntity, CmdClearNoteColorOverrides, CmdSaveBeatmap,
    CmdSaveBeatmapAs, CmdPackBeatmap, CmdScroll, CmdPanCanvas,
    CmdUpdateTimelineEvent, CmdUpdateTimelineEvents, CmdDeleteTimelineEvent,
    CmdCreateTimelineEvent, CmdCreateTimelineEvents, CmdReplaceBeatmapTimings,
    CmdReplaceBeatmapData, CmdStartMarquee, CmdUpdateMarquee, CmdEndMarquee,
    CmdRemoveMarqueeAt, CmdStartBrush, CmdUpdateBrush, CmdEndBrush,
    CmdStartErase, CmdUpdateErase, CmdEndErase, CmdUpdateBeatmapMetadata,
    CmdMarkBeatmapMetadataDirty, CmdImportAudio, CmdUpdateAudioResource,
    CmdRenameAudioResource, CmdUpdateAudioResourceConfig,
    CmdRemoveAudioResource, CmdRemoveBeatmap, CmdSaveTemporaryProject>;

}  // namespace MMM::Logic
