#pragma once

#include "event/core/BaseEvent.h"
#include "event/core/EventBus.h"

#include <mutex>
#include <string>

namespace MMM::Event
{
/// @brief 保存、另存为和打包的阶段通知；不提供虚假的完成百分比。
struct BeatmapSaveProgressEvent : BaseEvent {
    /// @brief 是否仍在执行文件操作。
    bool active{ true };
    /// @brief 当前阶段说明。
    std::string stage;
};

/// @brief 隔离普通 UI 帧与持有会话锁的耗时文件操作。
/// @return 保存文件操作共享的进程内互斥量。
/// @warning UI 每帧和逻辑文件指令仅尝试加锁，绝不等待此锁；失败时 UI
/// 绘制独立进度，逻辑保留原指令顺序并在后续 update 重试。
std::mutex& beatmapFileOperationGate();
}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveProgressEvent,
                       MMM::Event::BaseEvent)
