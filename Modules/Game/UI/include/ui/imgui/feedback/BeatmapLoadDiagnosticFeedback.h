#pragma once

#include <memory>

namespace MMM::UI
{

/// @brief 消费谱面加载诊断事件并显示非阻塞中央提示。
class BeatmapLoadDiagnosticFeedback final
{
public:
    /// @brief 创建反馈组件并订阅谱面加载诊断事件。
    BeatmapLoadDiagnosticFeedback();

    /// @brief 取消事件订阅并释放待显示诊断。
    ~BeatmapLoadDiagnosticFeedback();

    /// @brief 禁止移动，确保事件回调捕获的实现地址稳定。
    BeatmapLoadDiagnosticFeedback(BeatmapLoadDiagnosticFeedback&&) = delete;

    /// @brief 禁止复制，避免重复订阅谱面加载诊断事件。
    BeatmapLoadDiagnosticFeedback(const BeatmapLoadDiagnosticFeedback&) =
        delete;

    /// @brief 禁止移动赋值，确保事件回调捕获的实现地址稳定。
    BeatmapLoadDiagnosticFeedback& operator=(BeatmapLoadDiagnosticFeedback&&) =
        delete;

    /// @brief 禁止复制赋值，避免复制事件订阅所有权。
    BeatmapLoadDiagnosticFeedback& operator=(
        const BeatmapLoadDiagnosticFeedback&) = delete;

    /// @brief 在 UI 线程消费待显示诊断并提交中央通知。
    /// @warning UI 热路径：每帧只尝试消费低频事件队列；不访问文件系统。
    void update();

private:
    /// @brief 隐藏跨线程事件队列与订阅令牌的实现类型。
    struct Impl;

    /// @brief 反馈组件的独占实现状态。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace MMM::UI
