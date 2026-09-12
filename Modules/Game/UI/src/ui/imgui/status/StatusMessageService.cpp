#include "ui/imgui/status/StatusMessageService.h"

#include <utility>

/// @file StatusMessageService.cpp
/// @brief 单条 UI 临时状态消息的覆盖发布和逐帧过期实现。
/// @note 服务不负责绘制；消费者通过只读字符串视图查询当前有效消息。

namespace MMM::UI
{

/// @brief 发布一条有显示时限的状态消息。
/// @param message 状态消息文本。
/// @param durationSeconds 显示时长，单位秒。
void StatusMessageService::showStatusMessage(std::string message,
                                             float       durationSeconds)
{
    // 新消息覆盖旧消息并从调用方指定的完整时限重新计时。
    m_message          = std::move(message);
    m_remainingSeconds = durationSeconds;
}

/// @brief 更新当前消息的剩余显示时间。
/// @param deltaSeconds 自上一帧以来经过的秒数。
/// @warning UI 热路径：每帧执行；只允许更新常量规模的计时状态。
void StatusMessageService::update(float deltaSeconds)
{
    // 没有活动消息时保持空闲，避免剩余时间继续向负值漂移。
    if ( m_remainingSeconds <= 0.0f ) return;

    m_remainingSeconds -= deltaSeconds;
    if ( m_remainingSeconds <= 0.0f ) {
        // 归零后释放旧文本内容，查询端同步得到空视图。
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
