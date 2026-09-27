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

/// @brief 串行多个谱面会话提交的保存、导出与打包文件操作。
/// @return 逻辑文件指令共享的进程内互斥量。
/// @warning 各逻辑会话仅尝试加锁，失败时保留原指令并在后续 update 重试；
/// UI 不取得此锁，进度气泡也不阻塞或隐藏普通视图。
std::mutex& beatmapFileOperationGate();
}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveProgressEvent,
                       MMM::Event::BaseEvent)
