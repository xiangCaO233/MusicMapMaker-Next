#include "ui/imgui/feedback/BeatmapLoadDiagnosticFeedback.h"

#include "event/core/EventBus.h"
#include "event/logic/BeatmapLoadDiagnosticEvent.h"
#include "graphic/imguivk/VKContext.h"

#include <concurrentqueue.h>
#include <string>

/// @file BeatmapLoadDiagnosticFeedback.cpp
/// @brief 谱面加载诊断从事件总线跨线程转交到中央通知的实现。
/// @details 逻辑线程只入队轻量载荷，UI 帧负责构造最终文本并调用已有 Vulkan
/// 上下文通知入口，订阅生命周期由 Impl 自动管理。

namespace MMM::UI
{
namespace
{

/// @brief 中央提示保持可读的时间，单位秒。
constexpr float DIAGNOSTIC_NOTIFICATION_DURATION_SECONDS = 8.0F;

/// @brief 跨线程传入 UI 帧内消费的谱面加载诊断载荷。
struct BeatmapLoadDiagnosticPayload {
    /// @brief 本条诊断的稳定类型。
    Event::BeatmapLoadDiagnosticKind kind{
        Event::BeatmapLoadDiagnosticKind::LegacyMmmOriginalMalodyAvailable
    };

    /// @brief 关联文件路径，使用 UTF-8 字符串。
    std::string relatedPath;

    /// @brief Loader 提供的具体诊断内容。
    std::string message;
};

/// @brief 构建谱面加载诊断的用户可见中央提示。
/// @param payload 待显示的诊断载荷。
/// @return 包含修复建议和关联路径的提示文本。
std::string buildNotificationMessage(
    const BeatmapLoadDiagnosticPayload& payload)
{
    // 每种稳定诊断类型在 UI 层映射为简短建议，并按需附带上下文。
    switch ( payload.kind ) {
    case Event::BeatmapLoadDiagnosticKind::LegacyMmmOriginalMalodyAvailable: {
        std::string message = "旧 MMM 已有损，建议重新导入原始 .mc";
        if ( !payload.relatedPath.empty() ) {
            // 仅在 Loader 找到原始谱面时展示可操作路径。
            message += "\n原始文件：" + payload.relatedPath;
        }
        return message;
    }
    case Event::BeatmapLoadDiagnosticKind::AudioSampleTrackRelocated: {
        std::string message = "自动采样的非法轨道已移到首条可用 BGM 轨";
        if ( !payload.message.empty() ) {
            // Loader 的具体迁移说明作为第二行补充，不替代稳定摘要。
            message += "\n" + payload.message;
        }
        return message;
    }
    }
    // 防御未来枚举扩展遗漏映射，未知类型不弹出空通知。
    return {};
}

}  // namespace

/// @brief 谱面加载诊断反馈的事件订阅与跨线程队列实现。
/// @details subscription 必须晚于 queue 析构，因此成员声明顺序保证订阅先
/// 解除，再销毁回调可能访问的队列。
struct BeatmapLoadDiagnosticFeedback::Impl {
    /// @brief 构造实现状态并订阅谱面加载诊断事件。
    Impl()
        : subscription(
              Event::EventBus::instance()
                  .subscribe<Event::BeatmapLoadDiagnosticEvent>(
                      [this](const Event::BeatmapLoadDiagnosticEvent& event) {
                          // 回调可能运行在加载线程，只复制值并无锁入队。
                          queue.enqueue(BeatmapLoadDiagnosticPayload{
                              .kind        = event.m_kind,
                              .relatedPath = event.m_relatedPath,
                              .message     = event.m_message,
                          });
                      }))
    {
    }

    /// @brief 跨线程谱面加载诊断队列。
    /// @warning 生产者可能来自加载线程，只有 UI 线程执行出队。
    moodycamel::ConcurrentQueue<BeatmapLoadDiagnosticPayload> queue;

    /// @brief 析构时自动取消的谱面加载诊断订阅。
    /// @note 声明在 queue 之后，使逆序析构时优先阻止新事件进入队列。
    Event::ScopedSubscription<Event::BeatmapLoadDiagnosticEvent> subscription;
};

/// @brief 创建实现状态并立即开始接收谱面加载诊断。
/// @note unique_ptr 将事件系统相关类型隐藏在公开头文件之外。
BeatmapLoadDiagnosticFeedback::BeatmapLoadDiagnosticFeedback()
    : m_impl(std::make_unique<Impl>())
{
}

/// @brief 销毁反馈器并通过 ScopedSubscription 取消事件订阅。
BeatmapLoadDiagnosticFeedback::~BeatmapLoadDiagnosticFeedback() = default;

/// @brief 在 UI 帧消费所有待处理诊断并显示中央通知。
/// @warning UI 热路径：每帧调用；只处理已入队事件，不执行文件系统访问。
void BeatmapLoadDiagnosticFeedback::update()
{
    BeatmapLoadDiagnosticPayload payload;
    // 单帧排空队列，保持事件先后顺序并避免旧诊断延迟到后续帧。
    while ( m_impl->queue.try_dequeue(payload) ) {
        const std::string message = buildNotificationMessage(payload);
        // 未知类型或无映射内容不会生成空白通知。
        if ( message.empty() ) continue;

        // VKContext 可能在启动或退出阶段不可用，此时安全丢弃 UI 提示。
        if ( auto context = Graphic::VKContext::get() ) {
            // 所有加载诊断使用统一较长时限，保证路径和建议能够读完。
            context->get().showCenterNotification(
                message, DIAGNOSTIC_NOTIFICATION_DURATION_SECONDS);
        }
    }
}

}  // namespace MMM::UI
