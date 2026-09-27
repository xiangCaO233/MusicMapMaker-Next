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
    /// 软件主配置文件；文件缺失时定位其父目录。
    SoftwareConfiguration,
    /// 用户皮肤根目录；缺失时允许创建。
    SkinsDirectory,
    /// 用户插件根目录；缺失时允许创建。
    PluginsDirectory,
};

/// @brief 确保目录存在并使用系统文件管理器打开。
/// @param directory 待创建并打开的目录。
/// @return 目录存在且文件管理器成功启动时返回 true。
/// @note 使用 error_code 版本文件系统 API，遵守项目禁用异常约束。
/// @warning 该函数会访问文件系统并启动外部程序，只能由低频用户动作调用。
bool ensureAndOpenDirectory(const std::filesystem::path& directory)
{
    // 空路径无法安全创建或交给桌面环境处理。
    if ( directory.empty() ) return false;

    std::error_code filesystemError;
    // create_directories 对已存在目录同样成功，可统一处理首次和重复打开。
    std::filesystem::create_directories(directory, filesystemError);
    // 创建失败时不再启动文件管理器，保留明确的失败结果。
    return !filesystemError &&
           DesktopPathUtils::openInFileManager(directory, false);
}

/// @brief 在文件存在时定位文件，否则打开其父目录。
/// @param filePath 期望定位的文件路径。
/// @param createParentDirectory 文件不存在时是否允许创建父目录。
/// @return 文件或父目录成功交给系统文件管理器时返回 true。
/// @note 配置文件存在时请求文件管理器选中目标，缺失时回退到父目录。
/// @warning 该函数执行同步文件系统查询，仅用于显式帮助菜单动作。
bool revealFileOrOpenParent(const std::filesystem::path& filePath,
                            bool                         createParentDirectory)
{
    // 防止空路径的 parent_path 意外解析为当前工作目录。
    if ( filePath.empty() ) return false;

    std::error_code filesystemError;
    // 无异常查询普通文件状态，错误通过 filesystemError 显式传递。
    const bool fileExists =
        std::filesystem::is_regular_file(filePath, filesystemError);
    if ( !filesystemError && fileExists ) {
        // 文件存在时要求桌面环境定位并选中具体配置文件。
        return DesktopPathUtils::openInFileManager(filePath, true);
    }
    if ( !createParentDirectory ) {
        // 禁止创建时只尝试打开现有父目录，由桌面工具报告失败。
        return DesktopPathUtils::openInFileManager(filePath.parent_path(),
                                                   false);
    }
    // 允许创建时复用目录入口，保证父目录准备完成后再打开。
    return ensureAndOpenDirectory(filePath.parent_path());
}

/// @brief 在系统文件管理器中打开帮助菜单指定路径的动作。
/// @details 将路径类别映射到 AppPaths，并通过状态消息接口报告失败。
class OpenHelpPathAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造路径打开动作。
    /// @param pathKind 待打开的支持路径类型。
    /// @param failureTranslationKey 操作失败时使用的翻译键。
    /// @note failureTranslationKey 为静态翻译键，处理器不拥有其内存。
    OpenHelpPathAction(HelpPathKind pathKind, const char* failureTranslationKey)
        : m_pathKind(pathKind), m_failureTranslationKey(failureTranslationKey)
    {
    }

    /// @brief 解析路径并启动系统文件管理器。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    /// @warning 低频路径：可能创建目录并启动系统文件管理器。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 成功时无需额外状态提示，桌面文件管理器本身提供可见反馈。
        if ( openPath() ) return;

        // 失败原因统一映射为入口专属翻译文本，避免暴露平台错误细节。
        context.statusMessageSink.showStatusMessage(
            TR(m_failureTranslationKey).data(), 3.0F);
    }

private:
    /// @brief 执行当前类型对应的路径打开逻辑。
    /// @return 系统文件管理器成功启动时返回 true。
    /// @warning 低频路径：包含同步文件系统访问和外部程序启动。
    bool openPath() const
    {
        // 每个枚举值使用对应的创建与定位策略。
        switch ( m_pathKind ) {
        case HelpPathKind::SoftwareConfiguration:
            return revealFileOrOpenParent(
                Config::AppPaths::userConfigFilePath(), true);
        case HelpPathKind::SkinsDirectory:
            return ensureAndOpenDirectory(Config::AppPaths::skinsRootPath());
        case HelpPathKind::PluginsDirectory:
            return ensureAndOpenDirectory(Config::AppPaths::pluginsRootPath());
        }
        // 防御未来新增枚举值未同步分支的情况。
        return false;
    }

    /// @brief 待打开的支持路径类型。
    HelpPathKind m_pathKind;

    /// @brief 操作失败时使用的翻译键。
    const char* m_failureTranslationKey;
};

}  // namespace

/// @brief 创建在系统文件管理器中定位软件配置文件的动作处理器。
/// @return 独占所有权的配置路径动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenSoftwareConfigurationAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::SoftwareConfiguration,
        "ui.help.open_software_configuration_failed");
}

/// @brief 创建打开皮肤目录的动作处理器。
/// @return 独占所有权的皮肤目录动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenSkinsDirectoryAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::SkinsDirectory, "ui.help.open_skins_directory_failed");
}

/// @brief 创建打开插件目录的动作处理器。
/// @return 独占所有权的插件目录动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginsDirectoryAction()
{
    return std::make_unique<OpenHelpPathAction>(
        HelpPathKind::PluginsDirectory,
        "ui.help.open_plugins_directory_failed");
}

}  // namespace MMM::UI
