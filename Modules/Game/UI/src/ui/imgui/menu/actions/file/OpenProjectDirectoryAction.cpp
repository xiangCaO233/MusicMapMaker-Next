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
/// @details 动作只调用跨平台 DesktopPathUtils；失败经统一状态栏接口反馈，不在
/// 菜单层调用平台 Shell API。
class OpenProjectDirectoryAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在当前项目存在且根目录有效时允许执行。
    /// @param context 统一菜单上下文。
    /// @return 项目存在且根路径非空时返回 true。
    /// @warning UI 热路径：不访问文件系统，只检查内存路径。
    /// @note 路径是否仍存在由实际 DesktopPathUtils 调用判定。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        const auto* project =
            Logic::EditorEngine::instance().getCurrentProject();
        return project && !project->m_projectRoot.empty();
    }

    /// @brief 在系统文件管理器中打开当前项目根目录。
    /// @param context 提供状态消息接收器。
    /// @param activation 激活来源，不改变打开方式。
    /// @note 执行时再次读取项目，覆盖菜单绘制后项目已关闭的竞态。
    /// @warning 桌面打开可能失败，但不得抛出异常或阻塞等待文件管理器退出。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        const auto* project =
            Logic::EditorEngine::instance().getCurrentProject();
        // false 表示选择目录本身而非请求打开其中某个文件。
        if ( !project || project->m_projectRoot.empty() ||
             !DesktopPathUtils::openInFileManager(project->m_projectRoot,
                                                  false) ) {
            // 项目失效和桌面打开失败统一给出短时可见反馈。
            context.statusMessageSink.showStatusMessage(
                TR("ui.file.open_project_directory_failed").data(), 3.0f);
        }
    }
};
}  // namespace

/// @brief 创建打开当前项目目录的菜单项业务处理器。
/// @return 独占所有权的无状态处理器。
/// @note 处理器不缓存 Project 指针或文件系统路径。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectDirectoryAction()
{
    return std::make_unique<OpenProjectDirectoryAction>();
}

}  // namespace MMM::UI
