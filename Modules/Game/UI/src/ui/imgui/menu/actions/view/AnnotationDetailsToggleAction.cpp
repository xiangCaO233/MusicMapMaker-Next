#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 主画布批注详情卡片显示开关动作。
class AnnotationDetailsToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取批注详情卡片显示设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showAnnotationDetails;
    }

    /// @brief 状态变化后保存编辑器设置。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建主画布批注详情显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createAnnotationDetailsToggleAction()
{
    return std::make_unique<AnnotationDetailsToggleAction>();
}

}  // namespace MMM::UI
