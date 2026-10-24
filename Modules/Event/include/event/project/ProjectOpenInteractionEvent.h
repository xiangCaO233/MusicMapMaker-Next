#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include "event/project/ProjectOpenOrigin.h"
#include <string>

namespace MMM::Event
{
/// @brief 项目打开或新建交互结果；仅实际进入项目后发布 m_completed。
/// @note m_completed 为 false 时表示已唤出选择器或新建向导，其余结果字段尚未
/// 生效。
struct ProjectOpenInteractionEvent : BaseEvent {
    /// @brief 本次用户操作入口，随异步请求传递。
    ProjectOpenOrigin m_origin{ ProjectOpenOrigin::Unknown };
    /// @brief false 表示唤起选择器或向导，true 表示项目已完成打开。
    bool m_completed{ false };
    /// @brief 本次打开的原始路径。
    std::string m_path;
    /// @brief 临时只读项目已就绪。
    /// @note 该标志只描述打开模式，不表示底层文件系统权限。
    bool m_readOnly{ false };
    /// @brief 指定的谱面已成功加载为会话。
    /// @note 项目可在未自动打开具体谱面的情况下完成加载。
    bool m_beatmapOpened{ false };
};
}  // namespace MMM::Event
// 注册基础事件关系，保留项目打开交互的时间戳分发能力。
EVENT_REGISTER_PARENTS(MMM::Event::ProjectOpenInteractionEvent,
                       MMM::Event::BaseEvent);
