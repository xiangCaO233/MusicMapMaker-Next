#pragma once

#include "common/LogicCommands.h"
#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

/// @brief 在 UI、画布与逻辑线程之间解耦传递逻辑指令。
/// @note 事件按值持有命令，确保发布完成前不依赖调用方对象生命周期。
struct LogicCommandEvent : public BaseEvent {
    /// @brief 待逻辑层执行的类型擦除命令载荷。
    MMM::Logic::LogicCommand command;

    /// @brief 从左值命令复制构造事件。
    /// @param cmd 需要复制进事件的逻辑命令。
    LogicCommandEvent(const MMM::Logic::LogicCommand& cmd) : command(cmd) {}
    /// @brief 从右值命令移动构造事件，避免复制命令载荷。
    /// @param cmd 需要转移所有权的逻辑命令。
    LogicCommandEvent(MMM::Logic::LogicCommand&& cmd) : command(std::move(cmd))
    {
    }
};

}  // namespace MMM::Event

// 注册基础事件关系，使跨层指令仍可由通用事件监听器观察。
EVENT_REGISTER_PARENTS(MMM::Event::LogicCommandEvent, MMM::Event::BaseEvent)
