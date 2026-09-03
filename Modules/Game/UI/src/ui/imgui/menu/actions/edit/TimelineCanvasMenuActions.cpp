#include "config/AppConfig.h"
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
const std::string TIMELINE_VIEW_NAME{ "TimelineWindow" };

/// @brief 获取承载 Timing 表和批注表的 Timeline 画布能力接口。
/// @param context 当前主菜单上下文。
/// @return Timeline 视图存在时返回观察指针，否则返回 nullptr。
/// @warning UI 菜单热路径：仅查询本地视图注册表，不复制共享所有权。
ICanvasView* timelineCanvas(const MainMenuContext& context)
{
    return context.sourceManager
               ? context.sourceManager->getCanvasView(TIMELINE_VIEW_NAME)
               : nullptr;
}

/// @brief Timeline 专业模式开关动作。
class TimelineProfessionalModeToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取 Timeline 专业模式配置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .timelineProfessionalMode;
    }

    /// @brief 保存专业模式配置变化。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};

/// @brief 打开 Timeline Timing 表动作。
class OpenTimingPointsTableAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief Timeline 视图具有有效活动谱面时允许打开 Timing 表。
    bool isEnabled(const MainMenuContext& context) const override
    {
        return timelineCanvas(context) && MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开独立的 Timing 表窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto* timeline = timelineCanvas(context);
        if ( !timeline || !MenuUtil::hasActiveBeatmap(false) ) return;
        timeline->activateTimingPointsTable();
        if ( timeline->isTimingPointsTableOpen() ) {
            PlayPopupOpenFeedback();
        }
    }
};

/// @brief 打开谱面批注表动作。
class OpenAnnotationTableAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief Timeline 视图具有有效活动谱面时允许打开批注表。
    bool isEnabled(const MainMenuContext& context) const override
    {
        return timelineCanvas(context) && MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开独立的批注表窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto* timeline = timelineCanvas(context);
        if ( !timeline || !MenuUtil::hasActiveBeatmap(false) ) return;
        timeline->activateAnnotationTable();
        if ( timeline->isAnnotationTableOpen() ) {
            PlayPopupOpenFeedback();
        }
    }
};
}  // namespace

/// @brief 创建 Timeline 专业模式开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createTimelineProfessionalModeToggleAction()
{
    return std::make_unique<TimelineProfessionalModeToggleAction>();
}

/// @brief 创建打开 Timeline Timing 表动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenTimingPointsTableAction()
{
    return std::make_unique<OpenTimingPointsTableAction>();
}

/// @brief 创建打开谱面批注表动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenAnnotationTableAction()
{
    return std::make_unique<OpenAnnotationTableAction>();
}

}  // namespace MMM::UI
