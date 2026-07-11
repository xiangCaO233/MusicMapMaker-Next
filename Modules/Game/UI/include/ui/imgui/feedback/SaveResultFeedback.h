#pragma once

#include <memory>

namespace MMM::UI
{

/// @brief 消费谱面保存结果事件并渲染鼠标附近的反馈气泡。
class SaveResultFeedback final
{
public:
    /// @brief 创建保存结果反馈并订阅保存结果事件。
    SaveResultFeedback();

    /// @brief 取消保存结果事件订阅并释放反馈状态。
    ~SaveResultFeedback();

    /// @brief 禁止移动，确保事件回调捕获的实现地址稳定。
    SaveResultFeedback(SaveResultFeedback&&) = delete;

    /// @brief 禁止复制，避免重复订阅同一保存结果事件。
    SaveResultFeedback(const SaveResultFeedback&) = delete;

    /// @brief 禁止移动赋值，确保事件回调捕获的实现地址稳定。
    SaveResultFeedback& operator=(SaveResultFeedback&&) = delete;

    /// @brief 禁止复制赋值，避免复制事件订阅所有权。
    SaveResultFeedback& operator=(const SaveResultFeedback&) = delete;

    /// @brief 消费保存结果并更新反馈气泡计时器。
    /// @param deltaSeconds 自上一帧以来经过的秒数。
    /// @warning UI 热路径：每帧仅消费少量事件并更新常量规模状态。
    void update(float deltaSeconds);

    /// @brief 渲染当前有效的保存结果反馈气泡。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅在反馈计时器有效时提交固定数量绘制命令。
    void render(float dpiScale) const;

private:
    /// @brief 隐藏事件队列、订阅令牌和绘制状态的实现类型。
    struct Impl;

    /// @brief 保存结果反馈的独占实现状态。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace MMM::UI
