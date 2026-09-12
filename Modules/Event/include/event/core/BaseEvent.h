#pragma once

#include <chrono>

namespace MMM::Event
{
/// @brief 为所有可发布时间戳的事件提供公共基类。
/// @note 时间戳使用系统壁钟，便于日志关联；持续时长测量应改用单调时钟。
struct BaseEvent {
    /// @brief 事件发布时间戳采用的壁钟类型。
    using Clock = std::chrono::system_clock;
    /// @brief 统一到纳秒精度的时间点类型。
    using PrecisionTimePoint =
        std::chrono::time_point<Clock, std::chrono::nanoseconds>;
    /// @brief 事件构造时自动记录的纳秒精度时间戳。
    /// @note 该值用于事件排序和诊断，不承诺跨系统时钟调整后保持单调。
    PrecisionTimePoint timeStamp{
        std::chrono::time_point_cast<std::chrono::nanoseconds>(Clock::now())
    };
};
}  // namespace MMM::Event
