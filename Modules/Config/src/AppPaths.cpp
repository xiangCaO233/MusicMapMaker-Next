#include "config/AppPaths.h"
#include "config/Utf8Path.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
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

/// @brief 默认资源包中的皮肤脚本相对路径。
constexpr const char* kDefaultSkinRelativePath = "skins/mmm-nightly/skin.lua";

/// @brief 默认资源包中的窗口图标相对路径。
constexpr const char* kWindowIconRelativePath =
    "skins/mmm-nightly/resources/image/logo.png";

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
/// @return Windows 下固定为 用户目录/.config，其他平台优先使用
/// XDG_CONFIG_HOME。
std::filesystem::path baseConfigPath()
{
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
