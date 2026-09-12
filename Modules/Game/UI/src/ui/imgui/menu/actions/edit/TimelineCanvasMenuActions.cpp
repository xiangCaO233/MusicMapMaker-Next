#include "ui/IAuxiliaryWindowView.h"
#include "ui/ICanvasView.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief Timeline 视图在 UIManager 注册表中的稳定名称。
/// @note 必须与视图注册处保持一致，避免菜单动作查询到空视图。
const std::string TIMELINE_VIEW_NAME{ "TimelineWindow" };

/// @brief 批注表在 UIManager 注册表中的稳定名称。
/// @note 该名称只用于能力查询，不作为窗口可见标题。
const std::string ANNOTATION_TABLE_VIEW_NAME{ "AnnotationTableWindow" };

/// @brief 获取承载 Timing 表的 Timeline 画布能力接口。
/// @param context 当前主菜单上下文。
/// @return Timeline 视图存在时返回观察指针，否则返回 nullptr。
/// @warning UI 菜单热路径：仅查询本地视图注册表，不复制共享所有权。
/// @note 返回值所有权属于 SourceManager，仅限当前 UI 调用链使用。
ICanvasView* timelineCanvas(const MainMenuContext& context)
{
    return context.sourceManager
               ? context.sourceManager->getCanvasView(TIMELINE_VIEW_NAME)
               : nullptr;
}

/// @brief 获取独立批注表窗口能力接口。
/// @param context 当前主菜单上下文。
/// @return 批注表视图存在时返回观察指针，否则返回 nullptr。
/// @warning UI 菜单热路径：仅查询本地视图注册表，不复制共享所有权。
/// @note 返回值所有权属于 SourceManager，不得跨帧缓存。
IAuxiliaryWindowView* annotationTable(const MainMenuContext& context)
{
    return context.sourceManager
               ? context.sourceManager->getAuxiliaryWindowView(
                     ANNOTATION_TABLE_VIEW_NAME)
               : nullptr;
}

/// @brief 打开 Timeline Timing 表动作。
/// @details 同时要求视图能力和活动谱面，避免向 Logo 占位画布发送请求。
class OpenTimingPointsTableAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief Timeline 视图具有有效活动谱面时允许打开 Timing 表。
    /// @param context 提供 Timeline 视图查询入口。
    /// @return 视图存在且活动谱面有效时返回 true。
    /// @warning UI 热路径：只做本地能力与会话状态查询。
    bool isEnabled(const MainMenuContext& context) const override
    {
        return timelineCanvas(context) && MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开独立的 Timing 表窗口。
    /// @param context 提供 Timeline 画布能力查询入口。
    /// @param activation 激活来源，不改变窗口打开行为。
    /// @note 执行时重复校验条件，防止菜单绘制后状态发生变化。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto* timeline = timelineCanvas(context);
        // 动作可能晚于启用状态计算执行，因此必须再次验证视图与谱面。
        if ( !timeline || !MenuUtil::hasActiveBeatmap(false) ) return;
        timeline->activateTimingPointsTable();
        // 仅在窗口确实进入打开状态后播放反馈。
        if ( timeline->isTimingPointsTableOpen() ) {
            PlayPopupOpenFeedback();
        }
    }
};

/// @brief 打开谱面批注表动作。
/// @details 通过辅助窗口能力接口操作，避免菜单层依赖具体窗口实现。
class OpenAnnotationTableAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 独立批注表视图和有效活动谱面同时存在时允许打开。
    /// @param context 提供辅助窗口查询入口。
    /// @return 批注表存在且活动谱面有效时返回 true。
    /// @warning UI 热路径：不得创建窗口或遍历批注数据。
    bool isEnabled(const MainMenuContext& context) const override
    {
        return annotationTable(context) && MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开独立的批注表窗口。
    /// @param context 提供批注表能力查询入口。
    /// @param activation 激活来源，不改变窗口打开行为。
    /// @note 执行前再次验证活动谱面，避免对失效会话打开窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto* window = annotationTable(context);
        // 视图或活动谱面缺失时保持无副作用退出。
        if ( !window || !MenuUtil::hasActiveBeatmap(false) ) return;
        window->activateWindow();
        // 只有能力接口确认窗口已打开时才提供声音反馈。
        if ( window->isWindowOpen() ) {
            PlayPopupOpenFeedback();
        }
    }
};
}  // namespace

/// @brief 创建打开 Timeline Timing 表动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenTimingPointsTableAction()
{
    return std::make_unique<OpenTimingPointsTableAction>();
}

/// @brief 创建打开谱面批注表动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenAnnotationTableAction()
{
    return std::make_unique<OpenAnnotationTableAction>();
}

}  // namespace MMM::UI
