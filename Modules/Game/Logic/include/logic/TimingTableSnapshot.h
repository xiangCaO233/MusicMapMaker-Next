#pragma once

#include "common/render/TimelineInteractiveElement.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace MMM::Logic
{

/// @brief 完整的时间点表格数据，与请求时的活动会话和谱面实例绑定。
/// @note 实体标识只用于既有编辑命令，表格不会解引用逻辑注册表。
struct TimingTableSnapshot {
    /// @brief 与 RenderSnapshot 使用同一实例令牌，零表示尚无已载入的谱面。
    std::uintptr_t beatmapInstanceId{};
    /// @brief 按时间和效果排序的实际事件，不包含用于渲染的虚拟采样点。
    std::vector<Common::Render::TimelineInteractiveElement> rows;
};

/// @brief 为活动会话请求较新的时间点表格数据，并取得上次发布的快照。
/// @return 无活动会话时为空 optional，首次发布前为存在但为空的共享句柄。
/// @warning 表格可见时每帧调用。仅短暂保活会话并读取发布句柄，不等待会话锁。
std::optional<std::shared_ptr<const TimingTableSnapshot>>
readActiveTimingTable();

}  // namespace MMM::Logic
