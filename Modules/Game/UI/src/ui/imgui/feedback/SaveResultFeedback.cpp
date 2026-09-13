#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/feedback/SaveResultFeedback.h"

#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveProgressEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "mmm/project/PackageFileTypes.h"
#include "ui/Icons.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include <concurrentqueue.h>
#include <filesystem>
#include <imgui.h>
#include <memory>
#include <string>

/// @file SaveResultFeedback.cpp
/// @brief 保存、导出与打包结果从事件线程传递到状态栏或前景气泡的实现。
/// @details 事件回调只复制载荷并入队，UI 帧按 presentation 决定静默、状态栏
/// 或鼠标附近气泡；文件操作进行中在同一气泡中实时替换阶段文本。

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存反馈载荷。
struct SaveResultPayload {
    /// @brief 阶段通知与最终结果共用队列，保持单次操作的通知顺序。
    bool isProgress = false;
    /// @brief 阶段通知是否表示操作仍在执行。
    bool active = false;
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
/// @note 这里只构造正文，保存图标由调用方统一添加到可见气泡文本。
std::string buildSaveResultMessage(const SaveResultPayload& payload)
{
    // 路径转换集中在 UI 消费阶段，事件生产者只传递 UTF-8 字符串。
    const auto path      = Config::utf8ToPath(payload.path);
    const auto extension = Config::pathToUtf8(path.extension());
    // 已知谱面包扩展名和通用 zip 都使用“打包”语义。
    const bool isPackage =
        findPackageSupportedFileTypes(extension) != nullptr ||
        packageExtensionEquals(extension, ".zip");
    if ( !payload.success ) {
        // 业务层提供的具体错误优先于通用动作名称。
        if ( !payload.errorMessage.empty() ) return payload.errorMessage;
        if ( isPackage ) return "打包失败";
        return payload.isExport ? "导出失败" : "保存失败";
    }
    if ( !payload.isExport ) {
        // 普通保存不暴露内部路径，使用统一本地化状态文本。
        return TR("ui.status.beatmap.saved").data();
    }

    const std::string fileName = Config::pathToUtf8(path.filename());
    if ( fileName.empty() ) {
        // 路径缺失时仍给出动作成功反馈，但不拼接空文件名。
        return isPackage ? "打包成功" : "导出成功";
    }
    return isPackage ? "打包 " + fileName + " 成功"
                     : "导出 " + fileName + " 成功";
}
}  // namespace

/// @brief 保存结果反馈的事件订阅、队列和绘制状态实现。
/// @details 两个订阅共用一条队列以保持阶段与结果的投递顺序；订阅成员声明在
/// 队列之后，使逆序析构先取消回调，再销毁其访问的队列。
struct SaveResultFeedback::Impl {
    /// @brief 构造实现状态并订阅保存结果事件。
    Impl()
        : subscription(
              Event::EventBus::instance()
                  .subscribe<Event::BeatmapSaveResultEvent>(
                      [this](const Event::BeatmapSaveResultEvent& event) {
                          // 结果回调可能来自文件线程，只进行值复制和无锁入队。
                          queue.enqueue(SaveResultPayload{
                              .path         = event.path,
                              .success      = event.success,
                              .isExport     = event.isExport,
                              .errorMessage = event.errorMessage,
                              .presentation = event.presentation,
                          });
                      }))
        , progressSubscription(
              Event::EventBus::instance()
                  .subscribe<Event::BeatmapSaveProgressEvent>(
                      [this](const Event::BeatmapSaveProgressEvent& event) {
                          // 阶段文本复用 errorMessage 存储槽，但由 isProgress
                          // 区分。
                          queue.enqueue(
                              SaveResultPayload{ .isProgress   = true,
                                                 .active       = event.active,
                                                 .errorMessage = event.stage });
                      }))
    {
    }

    /// @brief 跨线程保存结果载荷队列。
    /// @warning 多线程生产、仅 UI 线程消费，禁止在事件回调中直接调用 ImGui。
    moodycamel::ConcurrentQueue<SaveResultPayload> queue;

    /// @brief 当前反馈气泡剩余显示时间，单位秒。
    /// @note 非正值表示没有需要绘制的最终结果。
    float remainingSeconds = 0.0f;

    /// @brief 当前反馈气泡是否表示成功。
    /// @note 活动失败气泡用于阻止紧随其后的成功通知覆盖具体原因。
    bool success = true;

    /// @brief 已拼接图标的反馈气泡显示文本，避免渲染时重复分配。
    /// @note 仅由 UI 线程更新和读取，不需要额外同步。
    std::string displayText;

    /// @brief 已拼接图标的当前文件操作阶段文本。
    /// @note 阶段由低频事件更新，渲染函数不再逐帧分配字符串。
    std::string progressText;
    /// @brief 是否收到尚未结束的操作通知。
    /// @note 该字段直接选择进度或最终结果气泡，不依赖文件互斥量。
    bool progressActive = false;

    /// @brief 保存结果事件订阅令牌，析构时自动取消订阅。
    /// @note 结果与进度订阅都必须在 queue 之前析构。
    Event::ScopedSubscription<Event::BeatmapSaveResultEvent> subscription;
    /// @brief 文件操作阶段订阅，先于队列析构。
    /// @note 逆序析构时该成员最先取消，随后才处理结果订阅与队列。
    Event::ScopedSubscription<Event::BeatmapSaveProgressEvent>
        progressSubscription;
};

/// @brief 创建保存结果反馈并订阅保存结果事件。
/// @note PImpl 保持公开头不依赖事件总线和并发队列具体类型。
SaveResultFeedback::SaveResultFeedback() : m_impl(std::make_unique<Impl>()) {}

/// @brief 取消保存结果事件订阅并释放反馈状态。
/// @note Impl 逆序析构会先取消订阅，再销毁回调访问的队列。
SaveResultFeedback::~SaveResultFeedback() = default;

/// @brief 消费保存结果并更新反馈气泡计时器。
/// @param deltaSeconds 自上一帧以来经过的秒数。
/// @param statusMessageSink 自动保存成功时使用的状态栏消息入口。
/// @details 先推进旧反馈计时，再消费新载荷，使本帧新结果从完整时限开始；
/// 阶段事件切换同一个气泡的实时文案，最终结果再按 presentation 分流。
/// @warning UI 热路径：每帧仅消费少量事件并更新常量规模状态。
void SaveResultFeedback::update(float               deltaSeconds,
                                IStatusMessageSink& statusMessageSink)
{
    // 当前帧间隔只属于此前已经显示的反馈；新到达的结果必须从完整时长开始，
    // 避免原生文件选择器或耗时导出造成的长帧让新反馈在首次绘制前直接过期。
    if ( !m_impl->progressActive && m_impl->remainingSeconds > 0.0f ) {
        // 进行中阶段优先显示，已有最终结果的可见时限暂不消耗。
        m_impl->remainingSeconds -= deltaSeconds;
    }

    SaveResultPayload payload;
    // 排空本帧前已到达的载荷，保留队列中的阶段和结果先后顺序。
    while ( m_impl->queue.try_dequeue(payload) ) {
        if ( payload.isProgress ) {
            // 阶段事件只切换气泡内容，不覆盖已有最终结果与计时状态。
            m_impl->progressActive = payload.active;
            if ( payload.active ) {
                const char* stage = payload.errorMessage.empty()
                                        ? "正在处理文件…"
                                        : payload.errorMessage.c_str();
                m_impl->progressText =
                    std::string(ICON_MMM_SAVE) + "  " + stage;
            }
            continue;
        }
        if ( payload.success && !m_impl->success &&
             m_impl->remainingSeconds > 0.0F ) {
            // 尚未读完的失败原因优先于随后到达的成功通知。
            continue;
        }
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TimedAutoSaveStatus ) {
            // 定时自动保存成功只进入状态栏，避免周期性打断指针附近操作。
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.timed_auto_save_success").data(), 2.0f);
            continue;
        }
        // 以下自动保存与备份分支只消费成功结果，失败仍进入显眼气泡。
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TriggeredAutoSaveStatus ) {
            // 条件触发自动保存使用独立文案，但保持相同两秒时限。
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.triggered_auto_save_success").data(),
                2.0f);
            continue;
        }
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TimedAutoBackupStatus ) {
            // 定时备份与正式保存区分，避免用户误认为当前谱面已显式保存。
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.timed_auto_backup_success").data(), 2.0f);
            continue;
        }
        if ( payload.success &&
             payload.presentation ==
                 Event::BeatmapSavePresentation::TriggeredAutoBackupStatus ) {
            // 条件触发备份同样只发布低干扰状态栏消息。
            statusMessageSink.showStatusMessage(
                TR("ui.status.beatmap.triggered_auto_backup_success").data(),
                2.0f);
            continue;
        }
        if ( payload.success &&
             payload.presentation == Event::BeatmapSavePresentation::Silent ) {
            // 静默成功用于调用方已有专属反馈的流程；失败仍需继续显示。
            continue;
        }
        // 可见气泡统一添加保存图标，并缓存最终文本供渲染阶段复用。
        m_impl->displayText =
            std::string(ICON_MMM_SAVE) + "  " + buildSaveResultMessage(payload);
        m_impl->success = payload.success;
        // 失败信息比成功信息多保留一秒，便于读取具体原因。
        m_impl->remainingSeconds = payload.success ? 2.0f : 3.0f;
    }
}

/// @brief 渲染当前有效的保存结果反馈气泡。
/// @param dpiScale 当前窗口内容缩放。
/// @details 进行中阶段与最终结果复用同一套鼠标附近气泡几何；根据鼠标靠近的
/// 工作区边缘翻转气泡枢轴，减少反馈超出可用区域。
/// @warning UI 热路径：仅在反馈计时器有效时提交固定数量绘制命令。
void SaveResultFeedback::render(float dpiScale) const
{
    const bool showProgress = m_impl->progressActive;
    // 没有活动阶段且最终结果已经过期时不生成任何前景几何。
    if ( !showProgress && m_impl->remainingSeconds <= 0.0f ) return;

    // 阶段与结果只切换文本和颜色，位置、背景及边距完全复用。
    const std::string& visibleText =
        showProgress ? m_impl->progressText : m_impl->displayText;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 读取当前帧鼠标位置，事件线程无需携带或同步指针坐标。
    const ImVec2 mousePos = ImGui::GetMousePos();

    // 默认气泡位于鼠标右下方，靠近工作区边缘时翻转枢轴。
    ImVec2 pivot{ 0.0f, 0.0f };
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f ) {
        // 靠右时以气泡右边缘为枢轴，令内容向左展开。
        pivot.x = 1.0f;
    }
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f ) {
        // 靠下时以气泡下边缘为枢轴，令内容向上展开。
        pivot.y = 1.0f;
    }

    // 偏移方向随枢轴翻转，使气泡远离鼠标并保持在主要工作区内。
    const float offsetX =
        pivot.x == 0.0f ? 20.0f * dpiScale : -20.0f * dpiScale;
    const float offsetY =
        pivot.y == 0.0f ? 20.0f * dpiScale : -20.0f * dpiScale;
    // 文本测量与 DPI 内边距共同决定气泡边界。
    const ImVec2 padding{ 16.0f * dpiScale, 10.0f * dpiScale };
    const ImVec2 textSize = ImGui::CalcTextSize(visibleText.c_str());
    const ImVec2 size{ textSize.x + padding.x * 2.0f,
                       textSize.y + padding.y * 2.0f };
    const ImVec2 pos{ mousePos.x + offsetX, mousePos.y + offsetY };
    // pos 是枢轴坐标，按 pivot 比例反推矩形左上角。
    const ImVec2 rectMin{ pos.x - size.x * pivot.x, pos.y - size.y * pivot.y };
    const ImVec2 rectMax{ rectMin.x + size.x, rectMin.y + size.y };

    // 成功与失败共享背景，仅通过高对比前景色表达结果语义。
    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    const ImU32 backgroundColor =
        ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.88f));
    // 进行中使用主题进度色，最终成功与失败分别使用绿色和红色。
    const ImU32 textColor =
        showProgress
            ? ImGui::GetColorU32(ImGuiCol_PlotHistogram)
            : ImGui::GetColorU32(m_impl->success
                                     ? ImVec4(0.45f, 1.0f, 0.48f, 1.0f)
                                     : ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    // 前景 DrawList 让反馈覆盖停靠窗口，但不创建可交互 ImGui 窗口。
    drawList->AddRectFilled(rectMin, rectMax, backgroundColor, 8.0f * dpiScale);
    drawList->AddText(ImVec2(rectMin.x + padding.x, rectMin.y + padding.y),
                      textColor,
                      visibleText.c_str());
}

}  // namespace MMM::UI
