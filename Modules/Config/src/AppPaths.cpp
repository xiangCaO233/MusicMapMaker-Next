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
    if ( !homeDrive.empty() && !homePath.empty() ) return homeDrive / homePath;
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

    return userHomePath() / ".config";
}

}  // namespace

std::filesystem::path AppPaths::configRootPath()
{
    return baseConfigPath() / kAppConfigDirectoryName;
}

std::filesystem::path AppPaths::userConfigFilePath()
{
    return configRootPath() / kUserConfigFileName;
}

std::filesystem::path AppPaths::imguiIniFilePath()
{
    return configRootPath() / kImguiIniFileName;
}

std::filesystem::path AppPaths::assetsRootPath()
{
    return configRootPath() / kAssetsDirectoryName;
}

std::filesystem::path AppPaths::defaultSkinFilePath()
{
    return assetsRootPath() / utf8ToPath(kDefaultSkinRelativePath);
}

std::filesystem::path AppPaths::windowIconFilePath()
{
    return assetsRootPath() / utf8ToPath(kWindowIconRelativePath);
}

std::filesystem::path AppPaths::legacyUserConfigFilePath()
{
    return kUserConfigFileName;
}

}  // namespace MMM::Config
