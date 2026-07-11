#pragma once

#include <string>

namespace MMM::UI
{

/// @brief 接收 UI 临时状态消息的抽象接口。
class IStatusMessageSink
{
public:
    /// @brief 默认析构状态消息接收接口。
    virtual ~IStatusMessageSink() = default;

    /// @brief 发布一条有显示时限的状态消息。
    /// @param message 状态消息文本。
    /// @param durationSeconds 显示时长，单位秒。
    virtual void showStatusMessage(std::string message,
                                   float       durationSeconds) = 0;
};

}  // namespace MMM::UI
