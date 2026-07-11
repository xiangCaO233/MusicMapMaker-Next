#include "ui/imgui/status/StatusMessageService.h"

#include <utility>

namespace MMM::UI
{

/// @brief 发布一条有显示时限的状态消息。
/// @param message 状态消息文本。
/// @param durationSeconds 显示时长，单位秒。
void StatusMessageService::showStatusMessage(std::string message,
                                             float       durationSeconds)
{
    m_message          = std::move(message);
    m_remainingSeconds = durationSeconds;
}

/// @brief 更新当前消息的剩余显示时间。
/// @param deltaSeconds 自上一帧以来经过的秒数。
/// @warning UI 热路径：每帧执行；只允许更新常量规模的计时状态。
void StatusMessageService::update(float deltaSeconds)
{
    if ( m_remainingSeconds <= 0.0f ) return;

    m_remainingSeconds -= deltaSeconds;
    if ( m_remainingSeconds <= 0.0f ) {
        m_remainingSeconds = 0.0f;
        m_message.clear();
    }
}

/// @brief 获取当前仍在显示时限内的状态消息。
/// @return 消息有效时返回只读视图，否则返回空视图。
std::string_view StatusMessageService::getStatusMessage() const
{
    return m_remainingSeconds > 0.0f ? std::string_view(m_message)
                                     : std::string_view{};
}

}  // namespace MMM::UI
