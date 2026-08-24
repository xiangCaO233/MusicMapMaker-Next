#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/feedback/SaveResultFeedback.h"

#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "mmm/project/PackageFileTypes.h"
#include "ui/Icons.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include <concurrentqueue.h>
#include <filesystem>
#include <imgui.h>
#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存反馈载荷。
struct SaveResultPayload {
    /// @brief 目标文件路径，使用 UTF-8 字符串。
    std::string path;

    /// @brief 是否为成功状态。
    bool success = true;

    /// @brief 是否来自另存为或导出流程。
    bool isExport = false;

    /// @brief 失败时由业务层提供的具体原因。
    std::string errorMessage;

    /// @brief 保存成功时应采用的界面反馈形式。
    Event::BeatmapSavePresentation presentation{
        Event::BeatmapSavePresentation::Transient
    };
};

/// @brief 根据保存结果构建用户可见的反馈文本。
/// @param payload 保存结果载荷。
/// @return 与保存、导出或打包结果对应的反馈文本。
std::string buildSaveResultMessage(const SaveResultPayload& payload)
{
    const auto path      = Config::utf8ToPath(payload.path);
    const auto extension = Config::pathToUtf8(path.extension());
    const bool isPackage =
        findPackageSupportedFileTypes(extension) != nullptr ||
        packageExtensionEquals(extension, ".zip");
    if ( !payload.success ) {
        if ( !payload.errorMessage.empty() ) return payload.errorMessage;
        if ( isPackage ) return "打包失败";
        return payload.isExport ? "导出失败" : "保存失败";
    }
    if ( !payload.isExport ) {
        return TR("ui.status.beatmap.saved").data();
    }

    const std::string fileName = Config::pathToUtf8(path.filename());
    if ( fileName.empty() ) {
        return isPackage ? "打包成功" : "导出成功";
    }
    return isPackage ? "打包 " + fileName + " 成功"
                     : "导出 " + fileName + " 成功";
}
}  // namespace

/// @brief 保存结果反馈的事件订阅、队列和绘制状态实现。
struct SaveResultFeedback::Impl {
    /// @brief 构造实现状态并订阅保存结果事件。
    Impl()
        : subscription(
              Event::EventBus::instance()
                  .subscribe<Event::BeatmapSaveResultEvent>(
                      [this](const Event::BeatmapSaveResultEvent& event) {
                          queue.enqueue(SaveResultPayload{
                              .path         = event.path,
                              .success      = event.success,
                              .isExport     = event.isExport,
                              .errorMessage = event.errorMessage,
                              .presentation = event.presentation,
                          });
                      }))
    {
    }

    /// @brief 跨线程保存结果载荷队列。
    moodycamel::ConcurrentQueue<SaveResultPayload> queue;

    /// @brief 当前反馈气泡剩余显示时间，单位秒。
    float remainingSeconds = 0.0f;

    /// @brief 当前反馈气泡是否表示成功。
    bool success = true;

    /// @brief 已拼接图标的反馈气泡显示文本，避免渲染时重复分配。
    std::string displayText;

    /// @brief 保存结果事件订阅令牌，析构时自动取消订阅。
    Event::ScopedSubscription<Event::BeatmapSaveResultEvent> subscription;
};

/// @brief 创建保存结果反馈并订阅保存结果事件。
SaveResultFeedback::SaveResultFeedback() : m_impl(std::make_unique<Impl>()) {}

/// @brief 取消保存结果事件订阅并释放反馈状态。
SaveResultFeedback::~SaveResultFeedback() = default;

/// @brief 消费保存结果并更新反馈气泡计时器。
/// @param deltaSeconds 自上一帧以来经过的秒数。
/// @param statusMessageSink 自动保存成功时使用的状态栏消息入口。
/// @warning UI 热路径：每帧仅消费少量事件并更新常量规模状态。
void SaveResultFeedback::update(float               deltaSeconds,
                                IStatusMessageSink& statusMessageSink)
{
    SaveResultPayload payload;
    while ( m_impl->queue.try_dequeue(payload) ) {
        if ( payload.success && !m_impl->success &&
             m_impl->remainingSeconds > 0.0F ) {
            continue;
        }
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TimedAutoSaveStatus ) {
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.timed_auto_save_success").data(), 2.0f);
            continue;
        }
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TriggeredAutoSaveStatus ) {
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.triggered_auto_save_success").data(),
                2.0f);
            continue;
        }
        if ( payload.success &&
             payload.presentation == Event::BeatmapSavePresentation::Silent ) {
            continue;
        }
        m_impl->displayText =
            std::string(ICON_MMM_SAVE) + "  " + buildSaveResultMessage(payload);
        m_impl->success          = payload.success;
        m_impl->remainingSeconds = payload.success ? 2.0f : 3.0f;
    }

    if ( m_impl->remainingSeconds > 0.0f ) {
        m_impl->remainingSeconds -= deltaSeconds;
    }
}

/// @brief 渲染当前有效的保存结果反馈气泡。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：仅在反馈计时器有效时提交固定数量绘制命令。
void SaveResultFeedback::render(float dpiScale) const
{
    if ( m_impl->remainingSeconds <= 0.0f ) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2   mousePos = ImGui::GetMousePos();

    ImVec2 pivot{ 0.0f, 0.0f };
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f ) {
        pivot.x = 1.0f;
    }
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f ) {
        pivot.y = 1.0f;
    }

    const float offsetX =
        pivot.x == 0.0f ? 20.0f * dpiScale : -20.0f * dpiScale;
    const float offsetY =
        pivot.y == 0.0f ? 20.0f * dpiScale : -20.0f * dpiScale;
    const ImVec2 padding{ 16.0f * dpiScale, 10.0f * dpiScale };
    const ImVec2 textSize = ImGui::CalcTextSize(m_impl->displayText.c_str());
    const ImVec2 size{ textSize.x + padding.x * 2.0f,
                       textSize.y + padding.y * 2.0f };
    const ImVec2 pos{ mousePos.x + offsetX, mousePos.y + offsetY };
    const ImVec2 rectMin{ pos.x - size.x * pivot.x, pos.y - size.y * pivot.y };
    const ImVec2 rectMax{ rectMin.x + size.x, rectMin.y + size.y };

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    const ImU32 backgroundColor =
        ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.88f));
    const ImU32 textColor =
        ImGui::GetColorU32(m_impl->success ? ImVec4(0.45f, 1.0f, 0.48f, 1.0f)
                                           : ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    drawList->AddRectFilled(rectMin, rectMax, backgroundColor, 8.0f * dpiScale);
    drawList->AddText(ImVec2(rectMin.x + padding.x, rectMin.y + padding.y),
                      textColor,
                      m_impl->displayText.c_str());
}

}  // namespace MMM::UI
