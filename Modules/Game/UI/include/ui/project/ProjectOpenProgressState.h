#pragma once

#include "event/project/ProjectEvents.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace MMM::UI
{

/// @brief UI 线程持有的项目打开进度快照。
struct ProjectOpenProgressState {
    /// @brief 当前是否存在未完成的项目打开流程。
    bool active{ false };

    /// @brief 当前加载阶段。
    Event::ProjectOpenProgressStage stage{
        Event::ProjectOpenProgressStage::Validating
    };

    /// @brief 当前总进度，范围为 0 到 1。
    float fraction{ 0.0F };

    /// @brief 当前处理的项目、谱面或资源名称。
    std::string detail;
};

/// @brief 开始新的项目打开进度，并保留已先到达的进度事件。
/// @param state 待更新状态。
/// @param detail 初始项目或谱面包名称。
inline void beginProjectOpenProgress(ProjectOpenProgressState& state,
                                     std::string               detail)
{
    if ( state.active ) return;
    state.active   = true;
    state.stage    = Event::ProjectOpenProgressStage::Validating;
    state.fraction = 0.0F;
    state.detail   = std::move(detail);
}

/// @brief 应用一个跨线程项目加载进度载荷。
/// @param state 待更新状态。
/// @param stage 当前加载阶段。
/// @param fraction 当前总进度。
/// @param detail 当前处理对象。
inline void applyProjectOpenProgress(ProjectOpenProgressState&       state,
                                     Event::ProjectOpenProgressStage stage,
                                     float fraction, std::string detail)
{
    state.active = true;
    state.stage  = stage;
    state.fraction =
        std::clamp(std::isfinite(fraction) ? fraction : 0.0F, 0.0F, 1.0F);
    state.detail = std::move(detail);
}

/// @brief 结束项目打开进度并清理当前处理对象。
/// @param state 待更新状态。
inline void finishProjectOpenProgress(ProjectOpenProgressState& state)
{
    state.active   = false;
    state.fraction = 1.0F;
    state.detail.clear();
}

}  // namespace MMM::UI
