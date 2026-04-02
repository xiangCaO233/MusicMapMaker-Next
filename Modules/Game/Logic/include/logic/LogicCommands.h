#pragma once

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
    entt::entity entity;  // 如果为 entt::null 则表示取消悬停
};

/**
 * @brief 所有可能的逻辑指令变体
 */
using LogicCommand = std::variant<CmdUpdateViewport, CmdSetPlayState,
                                  CmdLoadBeatmap, CmdSetHoveredEntity>;

}  // namespace MMM::Logic
