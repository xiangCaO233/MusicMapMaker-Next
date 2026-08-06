#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "config/AppPaths.h"
#include "config/skin/SkinConfig.h"
#include "ui/utils/DesktopPathUtils.h"

#include <filesystem>
#include <memory>
#include <system_error>

namespace MMM::UI
{
namespace
{

/// @brief 帮助菜单可打开的支持路径类型。
enum class HelpPathKind {
    SoftwareConfiguration,
    SkinsDirectory,
    PluginsDirectory,
};

/// @brief 确保目录存在并使用系统文件管理器打开。
/// @param directory 待创建并打开的目录。
/// @return 目录存在且文件管理器成功启动时返回 true。
bool ensureAndOpenDirectory(const std::filesystem::path& directory)
{
    if ( directory.empty() ) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    return !filesystemError &&
           DesktopPathUtils::openInFileManager(directory, false);
}

/// @brief 在文件存在时定位文件，否则打开其父目录。
/// @param filePath 期望定位的文件路径。
/// @param createParentDirectory 文件不存在时是否允许创建父目录。
/// @return 文件或父目录成功交给系统文件管理器时返回 true。
bool revealFileOrOpenParent(const std::filesystem::path& filePath,
                            bool                         createParentDirectory)
{
    if ( filePath.empty() ) return false;

    std::error_code filesystemError;
    const bool      fileExists =
        std::filesystem::is_regular_file(filePath, filesystemError);
    if ( !filesystemError && fileExists ) {
        return DesktopPathUtils::openInFileManager(filePath, true);
    }
    if ( !createParentDirectory ) {
        return DesktopPathUtils::openInFileManager(filePath.parent_path(),
                                                   false);
    }
    return ensureAndOpenDirectory(filePath.parent_path());
}

/// @brief 在系统文件管理器中打开帮助菜单指定路径的动作。
class OpenHelpPathAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造路径打开动作。
    /// @param pathKind 待打开的支持路径类型。
    /// @param failureTranslationKey 操作失败时使用的翻译键。
    OpenHelpPathAction(HelpPathKind pathKind, const char* failureTranslationKey)
        : m_pathKind(pathKind), m_failureTranslationKey(failureTranslationKey)
    {
    }

    /// @brief 解析路径并启动系统文件管理器。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        if ( openPath() ) return;

        context.statusMessageSink.showStatusMessage(
            TR(m_failureTranslationKey).data(), 3.0F);
    }

private:
    /// @brief 执行当前类型对应的路径打开逻辑。
    /// @return 系统文件管理器成功启动时返回 true。
    bool openPath() const
    {
        switch ( m_pathKind ) {
        case HelpPathKind::SoftwareConfiguration:
            return revealFileOrOpenParent(
                Config::AppPaths::userConfigFilePath(), true);
        case HelpPathKind::SkinsDirectory:
            return ensureAndOpenDirectory(Config::AppPaths::skinsRootPath());
        case HelpPathKind::PluginsDirectory:
            return ensureAndOpenDirectory(Config::AppPaths::pluginsRootPath());
        }
        return false;
    }

    /// @brief 待打开的支持路径类型。
    HelpPathKind m_pathKind;

    /// @brief 操作失败时使用的翻译键。
    const char* m_failureTranslationKey;
};

}  // namespace

/// @brief 创建在系统文件管理器中定位软件配置文件的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenSoftwareConfigurationAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::SoftwareConfiguration,
        "ui.help.open_software_configuration_failed");
}

/// @brief 创建打开皮肤目录的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenSkinsDirectoryAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::SkinsDirectory, "ui.help.open_skins_directory_failed");
}

/// @brief 创建打开插件目录的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginsDirectoryAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::PluginsDirectory,
        "ui.help.open_plugins_directory_failed");
}

}  // namespace MMM::UI
