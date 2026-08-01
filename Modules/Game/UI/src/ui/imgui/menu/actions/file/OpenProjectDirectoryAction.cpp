#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "ui/utils/DesktopPathUtils.h"

namespace MMM::UI
{
namespace
{
/// @brief 在系统文件管理器中打开当前项目目录的动作。
class OpenProjectDirectoryAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在当前项目存在且根目录有效时允许执行。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        const auto* project =
            Logic::EditorEngine::instance().getCurrentProject();
        return project && !project->m_projectRoot.empty();
    }

    /// @brief 在系统文件管理器中打开当前项目根目录。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        const auto* project =
            Logic::EditorEngine::instance().getCurrentProject();
        if ( !project || project->m_projectRoot.empty() ||
             !DesktopPathUtils::openInFileManager(project->m_projectRoot,
                                                  false) ) {
            context.statusMessageSink.showStatusMessage(
                TR("ui.file.open_project_directory_failed").data(), 3.0f);
        }
    }
};
}  // namespace

/// @brief 创建打开当前项目目录的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectDirectoryAction()
{
    return std::make_unique<OpenProjectDirectoryAction>();
}

}  // namespace MMM::UI
