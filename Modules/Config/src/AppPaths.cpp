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
    // Windows API 不会为调用方分配缓冲区，因此从常见长度起步并按需扩容。
    // 上限与 Win32 扩展路径的常用边界一致，防止异常返回导致无限循环。
    std::wstring pathBuffer(256, L'\0');
    while ( pathBuffer.size() <= 32768 ) {
        // 返回长度小于容量才表示路径完整；等于容量时必须按截断处理。
        const DWORD pathLength = GetModuleFileNameW(
            nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
        // API 失败时不猜测安装位置，交由调用方使用当前目录后备。
        if ( pathLength == 0 ) return {};
        if ( pathLength < pathBuffer.size() ) {
            // 去掉预分配缓冲区尾部的空字符，保留原生宽字符路径。
            pathBuffer.resize(pathLength);
            return std::filesystem::path(pathBuffer);
        }
        // 每轮倍增以减少长路径场景下的系统调用次数。
        pathBuffer.resize(pathBuffer.size() * 2);
    }
    // 超过保护上限视为不可可靠解析，不返回可能被截断的路径。
    return {};
#elif defined(__APPLE__)
    // macOS 接口通过空缓冲区回填所需容量，先查询可避免固定长度截断。
    std::uint32_t pathLength = 0;
    (void)_NSGetExecutablePath(nullptr, &pathLength);
    if ( pathLength == 0 ) return {};

    // 容量包含结尾空字符，转换时使用 c_str() 只读取实际路径内容。
    std::string pathBuffer(pathLength, '\0');
    if ( _NSGetExecutablePath(pathBuffer.data(), &pathLength) != 0 ) return {};

    const std::filesystem::path rawPath = utf8ToPath(pathBuffer.c_str());
    // 应用包或符号链接启动时解析规范路径，使资源目录定位保持稳定。
    std::error_code canonicalError;
    const auto      canonicalPath =
        std::filesystem::weakly_canonical(rawPath, canonicalError);
    // 规范化失败仍保留系统返回的原始路径，避免丢失可用信息。
    return canonicalError ? rawPath : canonicalPath;
#elif defined(__linux__)
    // /proc/self/exe 由内核维护，比 argv[0] 更能代表实际运行文件。
    std::error_code executablePathError;
    const auto      executablePath =
        std::filesystem::canonical("/proc/self/exe", executablePathError);
    // 不抛异常的重载适用于启动阶段；失败由上层当前目录回退处理。
    return executablePathError ? std::filesystem::path{} : executablePath;
#else
    // 未提供平台实现时显式返回空值，不对可执行文件布局作错误假设。
    return {};
#endif
}

#ifdef _WIN32
/// @brief 读取 Windows 宽字符环境变量并转换为文件系统路径。
/// @param name 要读取的宽字符环境变量名称。
/// @return 环境变量对应路径，读取失败时返回空路径。
std::filesystem::path readWideEnvironmentPath(const wchar_t* name)
{
    // 首次调用只查询包含结尾空字符在内的所需容量。
    DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
    if ( requiredLength == 0 ) return {};

    // 直接保留宽字符以避免配置根中的非 ASCII 用户名发生有损转换。
    std::wstring value(requiredLength, L'\0');
    DWORD        writtenLength =
        GetEnvironmentVariableW(name, value.data(), requiredLength);
    // 环境变量在两次调用之间增长时会返回所需容量，此时拒绝截断结果。
    if ( writtenLength == 0 || writtenLength >= requiredLength ) return {};

    // filesystem::path 在 Windows 上原生接收 UTF-16 内容。
    value.resize(writtenLength);
    return std::filesystem::path(value);
}
#endif

/// @brief 获取当前用户主目录路径。
/// @return 用户主目录；若系统环境无法提供，则退回到当前目录。
std::filesystem::path userHomePath()
{
#ifdef _WIN32
    // USERPROFILE 是 Windows 上最完整且通常已经规范化的用户目录来源。
    std::filesystem::path userProfile = readWideEnvironmentPath(L"USERPROFILE");
    if ( !userProfile.empty() ) return userProfile;

    // 旧式环境只提供盘符和相对路径，必须两者同时存在才可拼接。
    std::filesystem::path homeDrive = readWideEnvironmentPath(L"HOMEDRIVE");
    std::filesystem::path homePath  = readWideEnvironmentPath(L"HOMEPATH");
    if ( !homeDrive.empty() && !homePath.empty() ) {
        // 路径运算符负责处理分隔符，避免手工拼接破坏 UNC 或盘符语义。
        homeDrive /= homePath;
        return homeDrive;
    }
#else
    // POSIX 桌面进程继承 HOME；空字符串与未设置具有相同的不可用语义。
    const char* home = std::getenv("HOME");
    if ( home && home[0] != '\0' ) return utf8ToPath(home);
#endif

    // 某些沙箱会清理用户环境，此时当前目录是唯一可查询的低风险后备。
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
    // 专用覆盖优先级最高，供测试和多客户端实例隔离全部持久化状态。
    const char* collaborationProfileRoot = std::getenv("MMM_CONFIG_ROOT");
    if ( collaborationProfileRoot && collaborationProfileRoot[0] != '\0' ) {
        return utf8ToPath(collaborationProfileRoot);
    }
#ifndef _WIN32
    // 非 Windows 平台遵循 XDG；该值本身已经代表配置基础目录。
    const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    if ( xdgConfigHome && xdgConfigHome[0] != '\0' ) {
        return utf8ToPath(xdgConfigHome);
    }
#endif

    // 常规回退沿用历史的 ~/.config/mmm 布局，避免迁移既有用户数据。
    std::filesystem::path path = userHomePath();
    path /= kBaseConfigDirectoryName;
    return path;
}

#ifdef _WIN32
/// @brief 将目录设置为 Windows 隐藏目录。
/// @param path 需要隐藏的目录。
void markDirectoryHidden(const std::filesystem::path& path)
{
    // 空路径不允许传给 Win32 属性接口，避免意外查询当前目录。
    if ( path.empty() ) return;

    // 只有已存在的目录才设置隐藏位；文件或不可访问路径保持原状。
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if ( attrs == INVALID_FILE_ATTRIBUTES ) return;
    if ( (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ) return;
    if ( (attrs & FILE_ATTRIBUTE_HIDDEN) != 0 ) return;

    // 保留只读等既有属性，仅补充隐藏标记；失败不影响目录可用性。
    SetFileAttributesW(path.wstring().c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
}
#endif

/// @brief 确保目录存在。
/// @param path 需要创建的目录。
/// @return 目录存在或创建成功时返回 true。
bool ensureDirectory(const std::filesystem::path& path)
{
    // 调用方不得把空路径解释成当前目录并报告成功。
    if ( path.empty() ) return false;

    // 使用 error_code 重载保证配置初始化不通过异常中断启动流程。
    std::error_code createError;
    std::filesystem::create_directories(path, createError);

    // create_directories 对已存在目录返回 false，因此额外检查最终状态。
    std::error_code existsError;
    bool            exists = std::filesystem::exists(path, existsError);
    if ( createError || existsError || !exists ) {
        return false;
    }

#ifdef _WIN32
    // 仅隐藏历史约定的 .config 层，不隐藏其内部具体应用目录或资源。
    std::filesystem::path baseDirectory = userHomePath();
    baseDirectory /= kBaseConfigDirectoryName;
    markDirectoryHidden(baseDirectory);
#endif
    return true;
}

}  // namespace

/// @brief 获取当前程序所在目录。
/// @return 可执行文件的绝对父目录；平台查询失败时返回当前工作目录。
/// @note 该目录只用于定位随程序部署的内容，不承载用户可写配置。
std::filesystem::path AppPaths::executableDirectoryPath()
{
    // 正常路径以实际可执行文件为锚点，支持从任意工作目录启动应用。
    const std::filesystem::path executablePath = currentExecutablePath();
    if ( !executablePath.empty() ) {
        const std::filesystem::path executableDirectory =
            executablePath.parent_path();
        // 只接受绝对父目录，避免后续资源查找受到 cwd 变化影响。
        if ( executableDirectory.is_absolute() ) return executableDirectory;
    }

    // 平台查询失败时返回当前目录，使开发构建仍能按相对布局工作。
    std::error_code       currentPathError;
    std::filesystem::path currentPath =
        std::filesystem::current_path(currentPathError);
    return currentPathError ? std::filesystem::path{} : currentPath;
}

/// @brief 获取并尽力创建应用配置根目录。
/// @return 由环境覆盖或用户目录推导出的 mmm 配置目录。
/// @note 需要获知创建结果的调用方应改用 ensureConfigRootPath。
std::filesystem::path AppPaths::configRootPath()
{
    // 所有配置路径统一从基础目录追加应用名，避免各调用点布局漂移。
    std::filesystem::path path = baseConfigPath();
    path /= kAppConfigDirectoryName;
    // 查询根路径同时确保目录存在，保持旧调用方的初始化契约。
    ensureDirectory(path);
    return path;
}

/// @brief 确保应用配置根目录存在。
/// @return 目录最终存在且可被文件系统观察到时返回 true。
bool AppPaths::ensureConfigRootPath()
{
    // 显式入口向需要处理失败的启动代码返回目录创建结果。
    std::filesystem::path path = baseConfigPath();
    path /= kAppConfigDirectoryName;
    return ensureDirectory(path);
}

/// @brief 获取用户全局配置文件路径。
/// @return 配置根下的 user_config.json 路径。
std::filesystem::path AppPaths::userConfigFilePath()
{
    // 用户 JSON 与 ImGui 布局共享配置根，但各自保持独立文件生命周期。
    std::filesystem::path path = configRootPath();
    path /= kUserConfigFileName;
    return path;
}

/// @brief 获取 ImGui 布局配置文件路径。
/// @return 配置根下的 imgui.ini 路径。
std::filesystem::path AppPaths::imguiIniFilePath()
{
    // 固定文件名让 ImGui 重启后能够恢复同一套窗口停靠布局。
    std::filesystem::path path = configRootPath();
    path /= kImguiIniFileName;
    return path;
}

/// @brief 获取用户可更新资源根目录。
/// @return 配置根下的 assets 目录路径。
std::filesystem::path AppPaths::assetsRootPath()
{
    // 可更新资源位于用户配置根内，不依赖只读的程序安装目录。
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

/// @brief 获取默认翻译资源目录。
/// @return 用户资源根下的 translations 目录路径。
std::filesystem::path AppPaths::translationsRootPath()
{
    // 默认翻译与皮肤资源并列，便于构建期同步脚本独立更新。
    std::filesystem::path path = assetsRootPath();
    path /= kTranslationsDirectoryName;
    return path;
}

/// @brief 获取用户插件根目录。
/// @return 配置根下的 plugins 目录路径。
std::filesystem::path AppPaths::pluginsRootPath()
{
    // 插件根与 assets 分离，避免资源同步误删用户安装的扩展。
    std::filesystem::path path = configRootPath();
    path /= kPluginsDirectoryName;
    return path;
}

/// @brief 获取 Lua 主题插件目录。
/// @return 插件根下的 themes 目录路径。
std::filesystem::path AppPaths::themePluginsRootPath()
{
    // 主题插件拥有专用子目录，为后续其他插件类型预留命名空间。
    std::filesystem::path path = pluginsRootPath();
    path /= kThemePluginsDirectoryName;
    return path;
}

/// @brief 获取同步到用户资源根的默认皮肤入口。
/// @return mmm-default 皮肤的 skin.lua 完整路径。
std::filesystem::path AppPaths::defaultSkinFilePath()
{
    // 相对路径常量描述内置皮肤布局，统一经 UTF-8 路径转换。
    std::filesystem::path path = assetsRootPath();
    path /= utf8ToPath(kDefaultSkinRelativePath);
    return path;
}

/// @brief 获取默认窗口图标文件路径。
/// @return 默认皮肤资源中的 logo.png 完整路径。
std::filesystem::path AppPaths::windowIconFilePath()
{
    // 窗口图标复用默认皮肤资源，避免维护第二份安装级图像。
    std::filesystem::path path = assetsRootPath();
    path /= utf8ToPath(kWindowIconRelativePath);
    return path;
}

/// @brief 获取旧版工作目录配置文件的相对路径。
/// @return 保持历史文件名的 user_config.json 相对路径。
std::filesystem::path AppPaths::legacyUserConfigFilePath()
{
    // 旧版本把配置写在工作目录；迁移代码需要原样定位该相对文件。
    return kUserConfigFileName;
}

}  // namespace MMM::Config
