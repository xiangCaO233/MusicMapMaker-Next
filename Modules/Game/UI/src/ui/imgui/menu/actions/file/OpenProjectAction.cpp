#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"
#include <concurrentqueue.h>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的项目打开失败载荷。
struct ProjectOpenFailedPayload {
    /// @brief 尝试打开的路径，使用 UTF-8 字符串。
    std::string path;

    /// @brief 失败原因。
    std::string errorMessage;

    /// @brief 是否是打开谱面包失败。
    bool isPackage{ false };
};

/// @brief 获取项目打开失败提示队列。
moodycamel::ConcurrentQueue<ProjectOpenFailedPayload>&
getProjectOpenFailedQueue()
{
    static moodycamel::ConcurrentQueue<ProjectOpenFailedPayload> queue;
    return queue;
}

/// @brief 订阅项目打开失败事件，将事件转交 UI 帧内处理。
void ensureProjectOpenFailedSubscription()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::ProjectOpenFailedEvent>(
        [](const Event::ProjectOpenFailedEvent& event) {
            getProjectOpenFailedQueue().enqueue(ProjectOpenFailedPayload{
                .path         = event.m_projectPath,
                .errorMessage = event.m_errorMessage,
                .isPackage    = event.m_isPackage,
            });
        });
    subscribed = true;
}

/// @brief 打开项目选择器动作。
class OpenProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造动作并订阅项目打开失败事件。
    OpenProjectAction() { ensureProjectOpenFailedSubscription(); }

    /// @brief 消费项目打开失败消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的失败消息。
    void update(MainMenuContext& context) override
    {
        (void)context;
        ProjectOpenFailedPayload payload;
        while ( getProjectOpenFailedQueue().try_dequeue(payload) ) {
            m_pendingFailedPath      = payload.path;
            m_pendingFailedMessage   = payload.errorMessage;
            m_pendingFailedIsPackage = payload.isPackage;
            m_showOpenFailedPopup    = true;
        }
    }

    /// @brief 打开项目目录选择器。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        MenuUtil::openProjectFolderPicker();
    }

    /// @brief 消费 Ctrl+O 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

    /// @brief 渲染项目或谱面包打开失败弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在有失败消息时打开弹窗。
    void renderDeferred(MainMenuContext& context) override
    {
        renderProjectOpenFailedPopup(context.dpiScale);
    }

private:
    /// @brief 渲染项目或谱面包打开失败弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时绘制文本。
    void renderProjectOpenFailedPopup(float dpiScale)
    {
        constexpr const char* popupId = "打开失败###ProjectOpenFailedModal";
        if ( m_showOpenFailedPopup ) {
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showOpenFailedPopup = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        {
            Utils::CenteredModalPopupScope popupStyle(dpiScale);
            if ( popupStyle.begin(popupId,
                                  nullptr,
                                  ImGuiWindowFlags_None,
                                  ImVec2(560.0f * dpiScale, 0.0f)) ) {
                ImGui::TextWrapped("%s",
                                   m_pendingFailedIsPackage ? "打开谱面包失败。"
                                                            : "打开项目失败。");
                if ( !m_pendingFailedMessage.empty() ) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", m_pendingFailedMessage.c_str());
                }
                if ( !m_pendingFailedPath.empty() ) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("目标路径：%s",
                                       m_pendingFailedPath.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
                if ( ::MMM::UI::FeedbackButton(TR("ui.common.confirm").data(),
                                               buttonSize) ) {
                    m_pendingFailedPath.clear();
                    m_pendingFailedMessage.clear();
                    m_pendingFailedIsPackage = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }
    }

    /// @brief 是否在下一帧打开项目或谱面包打开失败弹窗。
    bool m_showOpenFailedPopup = false;

    /// @brief 打开失败的项目目录、谱面文件或谱面包路径。
    std::string m_pendingFailedPath;

    /// @brief 打开失败的错误说明。
    std::string m_pendingFailedMessage;

    /// @brief 打开失败是否来自谱面包。
    bool m_pendingFailedIsPackage = false;
};
}  // namespace

/// @brief 创建打开项目选择器的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectAction()
{
    return std::make_unique<OpenProjectAction>();
}

}  // namespace MMM::UI
