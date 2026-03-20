#pragma once

#include <chrono>

namespace MMM::Event
{
///@brief 基本事件
struct BaseEvent {
    // 使用系统最高精度作为存储标准
    // 如果需要单调时间，可以使用 std::chrono::steady_clock
    using Clock = std::chrono::system_clock;
    using PrecisionTimePoint =
        std::chrono::time_point<Clock, std::chrono::nanoseconds>;
    ///@brief 内部以纳秒精度存储(发布时自动记录时间戳)
    PrecisionTimePoint timeStamp{
        std::chrono::time_point_cast<std::chrono::nanoseconds>(Clock::now())
    };
};
}  // namespace MMM::Event
