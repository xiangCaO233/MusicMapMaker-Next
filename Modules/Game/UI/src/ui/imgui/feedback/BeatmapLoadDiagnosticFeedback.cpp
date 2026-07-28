#include "ui/imgui/feedback/BeatmapLoadDiagnosticFeedback.h"

#include "event/core/EventBus.h"
#include "event/logic/BeatmapLoadDiagnosticEvent.h"
#include "graphic/imguivk/VKContext.h"

#include <concurrentqueue.h>
#include <string>

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
};

/// @brief 构建谱面加载诊断的用户可见中央提示。
/// @param payload 待显示的诊断载荷。
/// @return 包含修复建议和关联路径的提示文本。
std::string buildNotificationMessage(
    const BeatmapLoadDiagnosticPayload& payload)
{
    switch ( payload.kind ) {
    case Event::BeatmapLoadDiagnosticKind::LegacyMmmOriginalMalodyAvailable: {
        std::string message = "旧 MMM 已有损，建议重新导入原始 .mc";
        if ( !payload.relatedPath.empty() ) {
            message += "\n原始文件：" + payload.relatedPath;
        }
        return message;
    }
    }
    return {};
}

}  // namespace

/// @brief 谱面加载诊断反馈的事件订阅与跨线程队列实现。
struct BeatmapLoadDiagnosticFeedback::Impl {
    /// @brief 构造实现状态并订阅谱面加载诊断事件。
    Impl()
        : subscription(
              Event::EventBus::instance()
                  .subscribe<Event::BeatmapLoadDiagnosticEvent>(
                      [this](const Event::BeatmapLoadDiagnosticEvent& event) {
                          queue.enqueue(BeatmapLoadDiagnosticPayload{
                              .kind        = event.m_kind,
                              .relatedPath = event.m_relatedPath,
                          });
                      }))
    {
    }

    /// @brief 跨线程谱面加载诊断队列。
    moodycamel::ConcurrentQueue<BeatmapLoadDiagnosticPayload> queue;

    /// @brief 析构时自动取消的谱面加载诊断订阅。
    Event::ScopedSubscription<Event::BeatmapLoadDiagnosticEvent> subscription;
};

BeatmapLoadDiagnosticFeedback::BeatmapLoadDiagnosticFeedback()
    : m_impl(std::make_unique<Impl>())
{
}

BeatmapLoadDiagnosticFeedback::~BeatmapLoadDiagnosticFeedback() = default;

void BeatmapLoadDiagnosticFeedback::update()
{
    BeatmapLoadDiagnosticPayload payload;
    while ( m_impl->queue.try_dequeue(payload) ) {
        const std::string message = buildNotificationMessage(payload);
        if ( message.empty() ) continue;

        if ( auto context = Graphic::VKContext::get() ) {
            context->get().showCenterNotification(
                message, DIAGNOSTIC_NOTIFICATION_DURATION_SECONDS);
        }
    }
}

}  // namespace MMM::UI
