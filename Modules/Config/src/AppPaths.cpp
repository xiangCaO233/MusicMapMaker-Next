#include "config/AppPaths.h"
#include "config/Utf8Path.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#endif

namespace MMM::Config
{

namespace
{

/// @brief 应用在 .config 下使用的子目录名。
constexpr const char* kAppConfigDirectoryName = "mmm";

/// @brief 用户配置基础目录名。
constexpr const char* kBaseConfigDirectoryName = ".config";

/// @brief 用户全局配置文件名。
constexpr const char* kUserConfigFileName = "user_config.json";

/// @brief ImGui 窗口布局配置文件名。
constexpr const char* kImguiIniFileName = "imgui.ini";

/// @brief 用户资源包目录名。
constexpr const char* kAssetsDirectoryName = "assets";

/// @brief 用户资源包中的皮肤目录名。
constexpr const char* kSkinsDirectoryName = "skins";

/// @brief 用户资源包中的默认翻译目录名。
constexpr const char* kTranslationsDirectoryName = "translations";

/// @brief 用户插件目录名。
constexpr const char* kPluginsDirectoryName = "plugins";

/// @brief Lua 主题插件目录名。
constexpr const char* kThemePluginsDirectoryName = "themes";

/// @brief 默认资源包中的皮肤脚本相对路径。
constexpr const char* kDefaultSkinRelativePath = "skins/mmm-default/skin.lua";

/// @brief 默认资源包中的窗口图标相对路径。
constexpr const char* kWindowIconRelativePath =
    "skins/mmm-default/resources/image/logo.png";

/// @brief 查询当前进程可执行文件的绝对路径。
/// @return 查询成功时返回可执行文件路径，否则返回空路径。
std::filesystem::path currentExecutablePath()
{
#ifdef _WIN32
    /// @brief 动态扩展缓冲区，兼容启用长路径后的 Windows 可执行文件路径。
    std::wstring pathBuffer(256, L'\0');
    while ( pathBuffer.size() <= 32768 ) {
        const DWORD pathLength = GetModuleFileNameW(
            nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
        if ( pathLength == 0 ) return {};
        if ( pathLength < pathBuffer.size() ) {
            pathBuffer.resize(pathLength);
            return std::filesystem::path(pathBuffer);
        }
        pathBuffer.resize(pathBuffer.size() * 2);
    }
    return {};
#elif defined(__APPLE__)
    /// @brief 先查询所需容量，避免可执行文件路径被固定缓冲区截断。
    std::uint32_t pathLength = 0;
    (void)_NSGetExecutablePath(nullptr, &pathLength);
    if ( pathLength == 0 ) return {};

    std::string pathBuffer(pathLength, '\0');
    if ( _NSGetExecutablePath(pathBuffer.data(), &pathLength) != 0 ) return {};

    const std::filesystem::path rawPath = utf8ToPath(pathBuffer.c_str());
    std::error_code             canonicalError;
    const auto                  canonicalPath =
        std::filesystem::weakly_canonical(rawPath, canonicalError);
    return canonicalError ? rawPath : canonicalPath;
#elif defined(__linux__)
    std::error_code executablePathError;
    const auto      executablePath =
        std::filesystem::canonical("/proc/self/exe", executablePathError);
    return executablePathError ? std::filesystem::path{} : executablePath;
#else
    return {};
#endif
}

#ifdef _WIN32
/// @brief 读取 Windows 宽字符环境变量并转换为文件系统路径。
/// @param name 要读取的宽字符环境变量名称。
/// @return 环境变量对应路径，读取失败时返回空路径。
std::filesystem::path readWideEnvironmentPath(const wchar_t* name)
{
    /// @brief 查询环境变量需要的缓冲区长度。
    DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
    if ( requiredLength == 0 ) return {};

    /// @brief 承载 Windows 宽字符环境变量内容的缓冲区。
    std::wstring value(requiredLength, L'\0');
    DWORD        writtenLength =
        GetEnvironmentVariableW(name, value.data(), requiredLength);
    if ( writtenLength == 0 || writtenLength >= requiredLength ) return {};

    value.resize(writtenLength);
    return std::filesystem::path(value);
}
#endif

/// @brief 获取当前用户主目录路径。
/// @return 用户主目录；若系统环境无法提供，则退回到当前目录。
std::filesystem::path userHomePath()
{
#ifdef _WIN32
    /// @brief Windows 用户主目录路径，优先来自 USERPROFILE。
    std::filesystem::path userProfile = readWideEnvironmentPath(L"USERPROFILE");
    if ( !userProfile.empty() ) return userProfile;

    /// @brief Windows 用户主目录盘符，作为 USERPROFILE 不存在时的后备。
    std::filesystem::path homeDrive = readWideEnvironmentPath(L"HOMEDRIVE");
    /// @brief Windows 用户主目录相对路径，作为 USERPROFILE 不存在时的后备。
    std::filesystem::path homePath = readWideEnvironmentPath(L"HOMEPATH");
    if ( !homeDrive.empty() && !homePath.empty() ) {
        homeDrive /= homePath;
        return homeDrive;
    }
#else
    /// @brief POSIX 用户主目录环境变量值。
    const char* home = std::getenv("HOME");
    if ( home && home[0] != '\0' ) return utf8ToPath(home);
#endif

    /// @brief 无法读取用户主目录时使用的安全后备路径。
    std::error_code       currentPathError;
    std::filesystem::path currentPath =
        std::filesystem::current_path(currentPathError);
    if ( !currentPathError ) return currentPath;
    return ".";
}

/// @brief 获取基础 .config 目录路径。
/// @return 优先使用 MMM_CONFIG_ROOT 测试隔离目录；否则 Windows 下固定为
/// 用户目录/.config，其他平台优先使用 XDG_CONFIG_HOME。
std::filesystem::path baseConfigPath()
{
    /// @brief 本机多客户端测试使用的应用配置隔离根目录。
    const char* collaborationProfileRoot = std::getenv("MMM_CONFIG_ROOT");
    if ( collaborationProfileRoot && collaborationProfileRoot[0] != '\0' ) {
        return utf8ToPath(collaborationProfileRoot);
    }
#ifndef _WIN32
    /// @brief XDG 配置根目录环境变量值。
    const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    if ( xdgConfigHome && xdgConfigHome[0] != '\0' ) {
        return utf8ToPath(xdgConfigHome);
    }
#endif

    std::filesystem::path path = userHomePath();
    path /= kBaseConfigDirectoryName;
    return path;
}

#ifdef _WIN32
/// @brief 将目录设置为 Windows 隐藏目录。
/// @param path 需要隐藏的目录。
void markDirectoryHidden(const std::filesystem::path& path)
{
    if ( path.empty() ) return;

    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if ( attrs == INVALID_FILE_ATTRIBUTES ) return;
    if ( (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ) return;
    if ( (attrs & FILE_ATTRIBUTE_HIDDEN) != 0 ) return;

    SetFileAttributesW(path.wstring().c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
}
#endif

/// @brief 确保目录存在。
/// @param path 需要创建的目录。
/// @return 目录存在或创建成功时返回 true。
bool ensureDirectory(const std::filesystem::path& path)
{
    if ( path.empty() ) return false;

    std::error_code createError;
    std::filesystem::create_directories(path, createError);

    std::error_code existsError;
    bool            exists = std::filesystem::exists(path, existsError);
    if ( createError || existsError || !exists ) {
        return false;
    }

#ifdef _WIN32
    std::filesystem::path baseDirectory = userHomePath();
    baseDirectory /= kBaseConfigDirectoryName;
    markDirectoryHidden(baseDirectory);
#endif
    return true;
}

}  // namespace

std::filesystem::path AppPaths::executableDirectoryPath()
{
    const std::filesystem::path executablePath = currentExecutablePath();
    if ( !executablePath.empty() ) {
        const std::filesystem::path executableDirectory =
            executablePath.parent_path();
        if ( executableDirectory.is_absolute() ) return executableDirectory;
    }

    std::error_code       currentPathError;
    std::filesystem::path currentPath =
        std::filesystem::current_path(currentPathError);
    return currentPathError ? std::filesystem::path{} : currentPath;
}

std::filesystem::path AppPaths::configRootPath()
{
    std::filesystem::path path = baseConfigPath();
    path /= kAppConfigDirectoryName;
    ensureDirectory(path);
    return path;
}

bool AppPaths::ensureConfigRootPath()
{
    std::filesystem::path path = baseConfigPath();
    path /= kAppConfigDirectoryName;
    return ensureDirectory(path);
}

std::filesystem::path AppPaths::userConfigFilePath()
{
    std::filesystem::path path = configRootPath();
    path /= kUserConfigFileName;
    return path;
}

std::filesystem::path AppPaths::imguiIniFilePath()
{
    std::filesystem::path path = configRootPath();
    path /= kImguiIniFileName;
    return path;
}

std::filesystem::path AppPaths::assetsRootPath()
{
    std::filesystem::path path = configRootPath();
    path /= kAssetsDirectoryName;
    return path;
}

/// @brief 获取皮肤资源根目录。
/// @return skins 目录在用户资源包根目录下的完整路径。
std::filesystem::path AppPaths::skinsRootPath()
{
    std::filesystem::path path = assetsRootPath();
    path /= kSkinsDirectoryName;
    return path;
}

std::filesystem::path AppPaths::translationsRootPath()
{
    std::filesystem::path path = assetsRootPath();
    path /= kTranslationsDirectoryName;
    return path;
}

std::filesystem::path AppPaths::pluginsRootPath()
{
    std::filesystem::path path = configRootPath();
    path /= kPluginsDirectoryName;
    return path;
}

std::filesystem::path AppPaths::themePluginsRootPath()
{
    std::filesystem::path path = pluginsRootPath();
    path /= kThemePluginsDirectoryName;
    return path;
}

std::filesystem::path AppPaths::defaultSkinFilePath()
{
    std::filesystem::path path = assetsRootPath();
    path /= utf8ToPath(kDefaultSkinRelativePath);
    return path;
}

std::filesystem::path AppPaths::windowIconFilePath()
{
    std::filesystem::path path = assetsRootPath();
    path /= utf8ToPath(kWindowIconRelativePath);
    return path;
}

std::filesystem::path AppPaths::legacyUserConfigFilePath()
{
    return kUserConfigFileName;
}

}  // namespace MMM::Config
