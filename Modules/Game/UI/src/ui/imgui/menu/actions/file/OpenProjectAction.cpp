#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "ui/imgui/menu/MainMenuTypes.h"
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
/// @details 载荷按值复制事件字段，避免队列元素引用事件总线临时对象。
struct ProjectOpenFailedPayload {
    /// @brief 尝试打开的路径，使用 UTF-8 字符串。
    std::string path;

    /// @brief 失败原因。
    std::string errorMessage;

    /// @brief 是否是打开谱面包失败。
    bool isPackage{ false };
};

/// @brief 获取项目打开失败提示队列。
/// @return 进程生命周期内唯一的多生产者、多消费者安全队列。
/// @warning UI 更新只允许非阻塞 try_dequeue，事件发布线程只执行 enqueue。
moodycamel::ConcurrentQueue<ProjectOpenFailedPayload>&
getProjectOpenFailedQueue()
{
    // 函数静态对象避免初始化顺序问题，并与事件订阅保持同一进程生命周期。
    static moodycamel::ConcurrentQueue<ProjectOpenFailedPayload> queue;
    return queue;
}

/// @brief 订阅项目打开失败事件，将事件转交 UI 帧内处理。
/// @note 订阅只建立一次，所有 OpenProjectAction 实例共享同一事件队列。
/// @warning 订阅回调可能在非 UI 线程执行，禁止直接操作 ImGui 状态。
void ensureProjectOpenFailedSubscription()
{
    static bool subscribed = false;
    // 菜单视图重建时复用既有订阅，避免同一失败事件重复入队。
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::ProjectOpenFailedEvent>(
        [](const Event::ProjectOpenFailedEvent& event) {
            // 将事件数据复制入无锁队列，由 UI 帧安全地更新弹窗状态。
            getProjectOpenFailedQueue().enqueue(ProjectOpenFailedPayload{
                .path         = event.m_projectPath,
                .errorMessage = event.m_errorMessage,
                .isPackage    = event.m_isPackage,
            });
        });
    // 只在订阅调用完成后置位，确保后续动作可依赖回调已经注册。
    subscribed = true;
}

/// @brief 打开项目选择器动作。
/// @details 统一菜单、快捷键和异步失败弹窗，并隔离事件线程与 ImGui 线程。
class OpenProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造动作并订阅项目打开失败事件。
    /// @warning 构造应发生在 UI 初始化阶段，避免热路径修改事件订阅表。
    OpenProjectAction() { ensureProjectOpenFailedSubscription(); }

    /// @brief 消费项目打开失败消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的失败消息。
    /// @note 同一帧存在多条失败时保留最后一条，弹窗展示最新操作结果。
    void update(MainMenuContext& context) override
    {
        (void)context;
        ProjectOpenFailedPayload payload;
        // 非阻塞排空队列，事件回调不与 ImGui 状态直接共享内存。
        while ( getProjectOpenFailedQueue().try_dequeue(payload) ) {
            // 复制到处理器持有状态，保证弹窗跨帧显示期间字符串有效。
            m_pendingFailedPath      = payload.path;
            m_pendingFailedMessage   = payload.errorMessage;
            m_pendingFailedIsPackage = payload.isPackage;
            m_showOpenFailedPopup    = true;
        }
    }

    /// @brief 打开项目目录选择器。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源；此入口固定标记为文件菜单。
    /// @warning 文件选择器属于低频阻塞交互，只能由用户显式触发。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 明确来源便于后续项目打开流程生成一致的统计或反馈上下文。
        MenuUtil::openProjectFolderPicker(Event::ProjectOpenOrigin::FileMenu);
    }

    /// @brief 消费 Ctrl+O 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 排除 Shift，避免与其他项目打开变体的组合键冲突。
    bool handleShortcut(MainMenuContext& context) override
    {
        ImGuiIO& io = ImGui::GetIO();
        // 使用快捷键来源区分菜单点击，项目打开行为本身保持一致。
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O) ) {
            MenuUtil::openProjectFolderPicker(
                Event::ProjectOpenOrigin::Shortcut);
            return true;
        }
        return false;
    }

    /// @brief 渲染项目或谱面包打开失败弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在有失败消息时打开弹窗。
    /// @note 弹窗使用上下文 DPI 缩放，失败数据由 update 阶段准备。
    void renderDeferred(MainMenuContext& context) override
    {
        renderProjectOpenFailedPopup(context.dpiScale);
    }

private:
    /// @brief 渲染项目或谱面包打开失败弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时绘制文本。
    /// @note 固定 ### ID 使本地化可见标题变化时仍保持弹窗状态稳定。
    void renderProjectOpenFailedPopup(float dpiScale)
    {
        constexpr const char* popupId = "打开失败###ProjectOpenFailedModal";
        if ( m_showOpenFailedPopup ) {
            // 将一次性请求转换为 ImGui 弹窗状态后立即清除标志。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showOpenFailedPopup = false;
        }

        // 没有活动弹窗时提前返回，避免创建样式作用域和布局控件。
        if ( !ImGui::IsPopupOpen(popupId) ) return;

        {
            // 居中样式作用域负责在离开代码块时恢复临时窗口设置。
            Utils::CenteredModalPopupScope popupStyle(dpiScale);
            if ( popupStyle.begin(popupId,
                                  nullptr,
                                  ImGuiWindowFlags_None,
                                  ImVec2(560.0f * dpiScale, 0.0f)) ) {
                // 包类型使用独立标题，帮助用户区分目录项目和归档导入失败。
                ImGui::TextWrapped("%s",
                                   m_pendingFailedIsPackage ? "打开谱面包失败。"
                                                            : "打开项目失败。");
                if ( !m_pendingFailedMessage.empty() ) {
                    // 后端未提供错误消息时省略该段，避免显示空白占位。
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", m_pendingFailedMessage.c_str());
                }
                if ( !m_pendingFailedPath.empty() ) {
                    // 保留完整路径以便用户定位失败目标，不在弹窗中截断。
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
                    // 确认后清理跨帧字符串，避免下次失败误用旧字段。
                    m_pendingFailedPath.clear();
                    m_pendingFailedMessage.clear();
                    m_pendingFailedIsPackage = false;
                    // 状态清理完成后再关闭弹窗，保证下一帧不会残留内容。
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }
    }

    /// @brief 是否在下一帧打开项目或谱面包打开失败弹窗。
    /// @note 仅由 update 置位，并由 renderProjectOpenFailedPopup 消费。
    bool m_showOpenFailedPopup = false;

    /// @brief 打开失败的项目目录、谱面文件或谱面包路径。
    std::string m_pendingFailedPath;

    /// @brief 打开失败的错误说明。
    std::string m_pendingFailedMessage;

    /// @brief 打开失败是否来自谱面包。
    /// @note 决定弹窗首行说明，不改变失败处理流程。
    bool m_pendingFailedIsPackage = false;
};
}  // namespace

/// @brief 创建打开项目选择器的菜单项业务处理器。
/// @return 独占所有权的项目打开处理器。
/// @note 事件订阅由处理器构造阶段按进程去重。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectAction()
{
    return std::make_unique<OpenProjectAction>();
}

}  // namespace MMM::UI
