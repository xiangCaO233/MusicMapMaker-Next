#pragma once

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
};

/**
 * @brief 选择实体指令
 */
struct CmdSelectEntity {
    entt::entity entity;
    bool         clearOthers;
};

/**
 * @brief 开始拖拽指令
 */
struct CmdStartDrag {
    entt::entity entity;
    std::string  cameraId;
    bool         isCtrlDown{ false };
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
 * @brief 编辑工具类型
 */
enum class EditTool {
    Move,         ///< 移动工具
    Marquee,      ///< 矩形选取
    Draw,         ///< 绘制工具
    ColorBrush,   ///< 配色笔刷工具
    ColorEraser,  ///< 配色橡皮工具
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
};

/**
 * @brief 另存为谱面指令
 */
struct CmdSaveBeatmapAs {
    std::string path;
};

/**
 * @brief 打包谱面指令
 */
struct CmdPackBeatmap {
    /// @brief 打包输出路径，使用 UTF-8 编码。
    std::string exportPath;

    /// @brief 需要写入包内的项目相对文件路径列表，使用 UTF-8 编码。
    std::vector<std::string> selectedProjectRelativePaths;
};

/**
 * @brief 滚动指令 (鼠标滚轮)
 */
struct CmdScroll {
    std::string cameraId;
    float       wheel;
    bool        isShiftDown;
};

/**
 * @brief 更新时间线事件指令
 */
struct CmdUpdateTimelineEvent {
    entt::entity entity;
    double       newTime;
    double       newValue;
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
 * @brief 批量替换当前谱面的 Timing 列表。
 */
struct CmdReplaceBeatmapTimings {
    /// @brief 替换后的 Timing 列表，时间戳单位为毫秒。
    std::vector<::MMM::Timing> timings;

    /// @brief 是否保留当前谱面中非 BPM 的流速/特效 Timing。
    bool keepNonBpmTimings{ false };
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
};

/**
 * @brief 更新谱面元数据指令
 */
struct CmdUpdateBeatmapMetadata {
    ::MMM::BaseMapMeta baseMeta;
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

/**
 * @brief 移除音轨资源指令
 */
struct CmdRemoveAudioResource {
    std::string id;
};

/**
 * @brief 移除谱面指令
 */
struct CmdRemoveBeatmap {
    std::string filePath;
};

/**
 * @brief 所有可能的逻辑指令变体
 */
using LogicCommand = std::variant<
    CmdUpdateEditorConfig, CmdUpdateViewport, CmdSetPlayState, CmdLoadBeatmap,
    CmdCreateBeatmap, CmdSetHoveredEntity, CmdSelectEntity, CmdStartDrag,
    CmdUpdateDrag, CmdEndDrag, CmdUpdateTrackCount, CmdSeek,
    CmdSetPlaybackSpeed, CmdChangeTool, CmdSetMousePosition, CmdUndo, CmdRedo,
    CmdCopy, CmdPaste, CmdCut, CmdDeleteSelected, CmdMirrorSelected,
    CmdAlignSelectedToCommonBeats, CmdSelectAll, CmdSetBrushNoteColor,
    CmdApplyNoteColorToSelection, CmdSetBrushNotePalette,
    CmdApplyNotePaletteToSelection, CmdApplyBrushPaletteToEntity,
    CmdClearNoteColorOverrides, CmdSaveBeatmap, CmdSaveBeatmapAs,
    CmdPackBeatmap, CmdScroll, CmdUpdateTimelineEvent, CmdDeleteTimelineEvent,
    CmdCreateTimelineEvent, CmdReplaceBeatmapTimings, CmdStartMarquee,
    CmdUpdateMarquee, CmdEndMarquee, CmdRemoveMarqueeAt, CmdStartBrush,
    CmdUpdateBrush, CmdEndBrush, CmdStartErase, CmdUpdateErase, CmdEndErase,
    CmdUpdateBeatmapMetadata, CmdImportAudio, CmdUpdateAudioResource,
    CmdRemoveAudioResource, CmdRemoveBeatmap>;

}  // namespace MMM::Logic
