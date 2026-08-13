#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <concurrentqueue.h>
#include <imgui.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存冲突确认载荷。
struct SaveConflictPayload {
    /// @brief 存在覆盖风险的目标路径，使用 UTF-8 字符串。
    std::string path;
};

/// @brief 跨线程传递给保存 action 的保存结果载荷。
struct SaveResultPayload {
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
};

/// @brief 获取保存冲突确认队列。
moodycamel::ConcurrentQueue<SaveConflictPayload>& getSaveConflictQueue()
{
    static moodycamel::ConcurrentQueue<SaveConflictPayload> queue;
    return queue;
}

/// @brief 获取保存结果队列，用于重置当前保存确认状态。
moodycamel::ConcurrentQueue<SaveResultPayload>& getSaveResultQueue()
{
    static moodycamel::ConcurrentQueue<SaveResultPayload> queue;
    return queue;
}

/// @brief 订阅逻辑层保存冲突事件，将事件转交保存 action 帧内处理。
void ensureSaveActionSubscriptions()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::BeatmapSaveConflictEvent>(
        [](const Event::BeatmapSaveConflictEvent& event) {
            getSaveConflictQueue().enqueue(SaveConflictPayload{
                .path = event.path,
            });
        });
    Event::EventBus::instance().subscribe<Event::BeatmapSaveResultEvent>(
        [](const Event::BeatmapSaveResultEvent& event) {
            if ( event.presentation !=
                 Event::BeatmapSavePresentation::Transient ) {
                return;
            }
            getSaveResultQueue().enqueue(SaveResultPayload{
                .isExport = event.isExport,
            });
        });
    subscribed = true;
}

/// @brief 保存当前谱面动作，拥有保存兼容性确认与覆盖确认状态。
class SaveBeatmapAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造保存动作并确保保存事件订阅完成。
    SaveBeatmapAction() { ensureSaveActionSubscriptions(); }

    /// @brief 仅在存在活跃谱面时允许保存。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 请求保存当前谱面。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        requestSaveBeatmap(false);
    }

    /// @brief 消费 Ctrl+S 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        (void)context;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S) ) {
            requestSaveBeatmap(false);
            return true;
        }
        return false;
    }

    /// @brief 消费跨线程保存事件。
    /// @warning UI 热路径：每帧只消费无锁队列，不执行阻塞操作。
    void update(MainMenuContext& context) override
    {
        (void)context;
        SaveConflictPayload conflictPayload;
        while ( getSaveConflictQueue().try_dequeue(conflictPayload) ) {
            m_pendingSaveConflictPath = conflictPayload.path;
            m_showSaveConflictWarning = true;
        }

        SaveResultPayload resultPayload;
        while ( getSaveResultQueue().try_dequeue(resultPayload) ) {
            if ( !resultPayload.isExport ) {
                m_currentSaveKeyConversionWarningConfirmed = false;
            }
        }
    }

    /// @brief 渲染保存动作拥有的延迟弹窗，并消费快捷键请求。
    /// @warning UI 热路径：每帧检查布尔标志；只渲染已打开弹窗。
    void renderDeferred(MainMenuContext& context) override
    {
        renderCompatibilityWarningPopup(context.dpiScale);
        renderSaveConflictWarningPopup(context.dpiScale);
    }

private:
    /// @brief 直接分发当前谱面保存命令。
    /// @param allowExternallyModifiedOverwrite
    /// 是否允许覆盖外部修改过的当前文件。
    void dispatchSaveBeatmap(bool allowExternallyModifiedOverwrite)
    {
        MenuUtil::dispatchCommand(Logic::CmdSaveBeatmap{
            .allowExternallyModifiedOverwrite =
                allowExternallyModifiedOverwrite,
        });
    }

    /// @brief 请求保存当前谱面，必要时先展示格式兼容性警告。
    /// @param allowExternallyModifiedOverwrite
    /// 是否允许覆盖外部修改过的当前文件。
    void requestSaveBeatmap(bool allowExternallyModifiedOverwrite)
    {
        std::string path;
        {
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( session && session->getContext().currentBeatmap ) {
                auto savePath = session->getContext()
                                    .currentBeatmap->m_baseMapMetadata.map_path;
                if ( Config::AppConfig::instance()
                         .getEditorSettings()
                         .saveFormatPreference ==
                     Config::SaveFormatPreference::ForceMMM ) {
                    savePath.replace_extension(".mmm");
                }
                path = Config::pathToUtf8(savePath);
            }
        }

        auto warnings = MenuUtil::collectExportCompatibilityWarnings(path);
        if ( warnings.empty() ) {
            dispatchSaveBeatmap(allowExternallyModifiedOverwrite);
            return;
        }

        m_pendingExportPath     = std::move(path);
        m_pendingExportWarnings = std::move(warnings);
        m_pendingCompatibilityWarningAllowOverwrite =
            allowExternallyModifiedOverwrite;
        m_showExportCompatibilityWarning = true;
    }

    /// @brief 清空当前兼容性确认弹窗状态。
    void clearCompatibilityWarningState()
    {
        m_pendingExportPath.clear();
        m_pendingExportWarnings.clear();
        m_pendingCompatibilityWarningAllowOverwrite = false;
    }

    /// @brief 渲染保存目标格式兼容性警告弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderCompatibilityWarningPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "谱面兼容性警告###CurrentBeatmapSaveWarningModal";
        if ( m_showExportCompatibilityWarning ) {
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showExportCompatibilityWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0f * dpiScale, 0.0f)) ) {
            ImGui::Text(
                "%s %s 前需要确认以下兼容性变化：", "保存", "Malody Key");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            for ( const auto& warning : m_pendingExportWarnings ) {
                MenuUtil::drawWrappedBulletText(warning);
            }

            ImGui::Spacing();
            MenuUtil::drawWrappedLabelValue("目标文件：", m_pendingExportPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            MenuUtil::centerNextItem(actionButtonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续保存", actionButtonSize) ) {
                m_currentSaveKeyConversionWarningConfirmed = true;
                dispatchSaveBeatmap(
                    m_pendingCompatibilityWarningAllowOverwrite);
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           actionButtonSize) ) {
                m_currentSaveKeyConversionWarningConfirmed = false;
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 渲染保存目标被外部修改时的覆盖确认弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderSaveConflictWarningPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "文件已被另外修改过###SaveConflictWarningModal";
        if ( m_showSaveConflictWarning ) {
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showSaveConflictWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(540.0f * dpiScale, 0.0f)) ) {
            ImGui::TextWrapped(
                "文件已被另外修改过，强行覆盖可能会导致丢失数据，是否确认？");
            if ( !m_pendingSaveConflictPath.empty() ) {
                ImGui::Spacing();
                ImGui::TextWrapped("目标文件：%s",
                                   m_pendingSaveConflictPath.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("确认覆盖", buttonSize) ) {
                if ( m_currentSaveKeyConversionWarningConfirmed ) {
                    dispatchSaveBeatmap(true);
                    m_currentSaveKeyConversionWarningConfirmed = false;
                } else {
                    requestSaveBeatmap(true);
                }
                m_pendingSaveConflictPath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                m_pendingSaveConflictPath.clear();
                m_currentSaveKeyConversionWarningConfirmed = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 是否在下一帧打开保存兼容性警告弹窗。
    bool m_showExportCompatibilityWarning = false;
    /// @brief 是否在下一帧打开保存覆盖风险确认弹窗。
    bool m_showSaveConflictWarning = false;
    /// @brief 当前保存的 key 模式降级警告是否已经确认。
    bool m_currentSaveKeyConversionWarningConfirmed = false;
    /// @brief 待确认当前保存是否允许覆盖外部修改。
    bool m_pendingCompatibilityWarningAllowOverwrite = false;
    /// @brief 待确认保存的目标路径。
    std::string m_pendingExportPath;
    /// @brief 待确认保存的兼容性警告消息。
    std::vector<std::string> m_pendingExportWarnings;
    /// @brief 待确认覆盖的保存目标路径。
    std::string m_pendingSaveConflictPath;
};
}  // namespace

/// @brief 创建保存当前谱面的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAction()
{
    return std::make_unique<SaveBeatmapAction>();
}

}  // namespace MMM::UI
