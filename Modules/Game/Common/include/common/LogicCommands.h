#pragma once

#include "config/EditorConfig.h"
#include "mmm/timing/Timing.h"
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <variant>

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
};

/**
 * @brief 更新拖拽位置指令
 */
struct CmdUpdateDrag {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
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
    bool        isHovering;
    bool        isDragging{ false };
};

/**
 * @brief 开始框选指令
 */
struct CmdStartMarquee {
    std::string cameraId;
    float       mouseX;
    float       mouseY;
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
    Move,     // 移动工具
    Marquee,  // 矩形选取
    Draw,     // 绘制工具
    Cut,      // 裁剪工具
};

/**
 * @brief 切换编辑工具指令
 */
struct CmdChangeTool {
    EditTool tool;
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
};

/**
 * @brief 剪切指令
 */
struct CmdCut {
};

/**
 * @brief 保存谱面指令
 */
struct CmdSaveBeatmap {
};

/**
 * @brief 打包谱面指令
 */
struct CmdPackBeatmap {
    std::string exportPath;
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
 * @brief 所有可能的逻辑指令变体
 */
using LogicCommand = std::variant<
    CmdUpdateEditorConfig, CmdUpdateViewport, CmdSetPlayState, CmdLoadBeatmap,
    CmdSetHoveredEntity, CmdSelectEntity, CmdStartDrag, CmdUpdateDrag,
    CmdEndDrag, CmdUpdateTrackCount, CmdSeek, CmdSetPlaybackSpeed,
    CmdChangeTool, CmdSetMousePosition, CmdUndo, CmdRedo, CmdCopy, CmdPaste,
    CmdCut, CmdSaveBeatmap, CmdPackBeatmap, CmdScroll, CmdUpdateTimelineEvent,
    CmdDeleteTimelineEvent, CmdCreateTimelineEvent, CmdStartMarquee,
    CmdUpdateMarquee, CmdEndMarquee>;

}  // namespace MMM::Logic
