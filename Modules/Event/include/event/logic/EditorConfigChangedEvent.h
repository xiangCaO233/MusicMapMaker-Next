#pragma once

#include "config/EditorConfig.h"
#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

/// @brief 逻辑层修改编辑器配置后发布给 UI 的快照事件。
/// @note 配置按值保存，使订阅者看到同一次变更对应的稳定状态。
struct EditorConfigChangedEvent : public BaseEvent {
    /// @brief 变更完成后的完整编辑器配置快照。
    MMM::Config::EditorConfig config;

    /// @brief 从配置快照构造事件。
    /// @param cfg 需要复制到事件中的最新配置。
    EditorConfigChangedEvent(const MMM::Config::EditorConfig& cfg) : config(cfg)
    {
    }
};

}  // namespace MMM::Event

// 注册基础事件关系，使配置变更能够进入通用事件分发链。
// 订阅者通过快照更新自身缓存，不需要反向读取逻辑层配置对象。
EVENT_REGISTER_PARENTS(MMM::Event::EditorConfigChangedEvent,
                       MMM::Event::BaseEvent)
