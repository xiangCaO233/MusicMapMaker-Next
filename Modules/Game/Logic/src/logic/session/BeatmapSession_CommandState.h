#pragma once

#include "common/LogicCommands.h"
#include "logic/BeatmapSession.h"
#include <concurrentqueue.h>
#include <deque>
#include <optional>

namespace MMM::Logic
{

/// @brief 会话独占的命令队列实现，只有入队和消费命令的实现文件读取。
/// @details 命令继续按值保存在原来的队列中，不改变原子同步和重试顺序。
/// 状态在会话构造时创建一次，保持入队生产者与逻辑消费者借用的地址稳定。
struct BeatmapSession::CommandState {
    /// @brief 跨线程无锁命令队列，载荷在队列发布后归消费者所有。
    moodycamel::ConcurrentQueue<LogicCommand> queue;
    /// @brief 仅由逻辑线程访问的待重试文件指令队列。
    /// @note 普通编辑命令继续被消费，文件命令之间保留原来的相对顺序。
    std::deque<LogicCommand> deferredFileCommands;
    /// @brief 活跃手势或本地确认屏障外等待的最新远端权威替换。
    /// @note 仅逻辑线程访问，新状态覆盖旧状态，保持原来的非阻塞合并语义。
    std::optional<CmdReplaceBeatmapData> deferredAuthoritativeReplacement;
};

}  // namespace MMM::Logic
