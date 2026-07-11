#pragma once

#include "ui/imgui/status/IStatusMessageSink.h"

#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 保存并按显示时限提供 UI 状态栏消息。
class StatusMessageService final : public IStatusMessageSink
{
public:
    /// @brief 发布一条有显示时限的状态消息。
    /// @param message 状态消息文本。
    /// @param durationSeconds 显示时长，单位秒。
    void showStatusMessage(std::string message, float durationSeconds) override;

    /// @brief 更新当前消息的剩余显示时间。
    /// @param deltaSeconds 自上一帧以来经过的秒数。
    /// @warning UI 热路径：每帧执行；只允许更新常量规模的计时状态。
    void update(float deltaSeconds);

    /// @brief 获取当前仍在显示时限内的状态消息。
    /// @return 消息有效时返回只读视图，否则返回空视图。
    [[nodiscard]] std::string_view getStatusMessage() const;

private:
    /// @brief 当前状态栏显示的临时消息。
    std::string m_message;

    /// @brief 当前消息的剩余显示时间，单位秒。
    float m_remainingSeconds = 0.0f;
};

}  // namespace MMM::UI
