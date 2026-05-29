#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <fstream>
#include <iomanip>

namespace MMM::Config
{

AppConfig& AppConfig::instance()
{
    static AppConfig instance;
    return instance;
}

AppConfig::AppConfig()
{
    // 初始化默认值
    reset();
}

void AppConfig::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_editorConfig = EditorConfig();
}

bool AppConfig::load(const std::filesystem::path& path)
{
    /// @brief 是否使用应用默认配置路径。
    bool useDefaultPath = path.empty();
    /// @brief 本次实际尝试读取的配置文件路径。
    std::filesystem::path finalPath =
        useDefaultPath ? getDefaultConfigPath() : path;

    /// @brief 检查默认配置文件是否存在时接收的文件系统错误。
    std::error_code configExistsError;
    if ( !std::filesystem::exists(finalPath, configExistsError) &&
         useDefaultPath ) {
        /// @brief 旧版当前工作目录下的配置文件路径，用于迁移旧配置。
        std::filesystem::path legacyPath = AppPaths::legacyUserConfigFilePath();
        /// @brief 检查旧版配置文件是否存在时接收的文件系统错误。
        std::error_code legacyExistsError;
        if ( std::filesystem::exists(legacyPath, legacyExistsError) ) {
            finalPath = legacyPath;
            XINFO("Using legacy config for migration: {}",
                  pathToUtf8(finalPath));
        }
    }

    /// @brief 检查最终配置文件是否存在时接收的文件系统错误。
    std::error_code finalExistsError;
    if ( !std::filesystem::exists(finalPath, finalExistsError) ) {
        XINFO("Config file not found: {}. Using default values.",
              pathToUtf8(finalPath));
        return false;
    }

    try {
        std::ifstream file(finalPath);
        if ( !file.is_open() ) {
            XERROR("Failed to open config file for reading: {}",
                   pathToUtf8(finalPath));
            return false;
        }

        nlohmann::json j;
        file >> j;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_editorConfig = j.get<EditorConfig>();
        }

        XINFO("Config loaded successfully from: {}", pathToUtf8(finalPath));
        if ( useDefaultPath && finalPath != getDefaultConfigPath() ) {
            save();
        }
        return true;
    } catch ( const std::exception& e ) {
        XERROR("Failed to parse config file: {}. Error: {}",
               pathToUtf8(finalPath),
               e.what());
        return false;
    }
}

bool AppConfig::save(const std::filesystem::path& path) const
{
    std::filesystem::path finalPath =
        path.empty() ? getDefaultConfigPath() : path;

    try {
        // 确保目录存在
        if ( auto parent = finalPath.parent_path(); !parent.empty() ) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(finalPath);
        if ( !file.is_open() ) {
            XERROR("Failed to open config file for writing: {}",
                   pathToUtf8(finalPath));
            return false;
        }

        nlohmann::json j;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            j = m_editorConfig;
        }

        file << std::setw(4) << j << std::endl;
        // XINFO("Config saved successfully to: {}", finalPath.string());
        return true;
    } catch ( const std::exception& e ) {
        XERROR("Failed to save config file: {}. Error: {}",
               pathToUtf8(finalPath),
               e.what());
        return false;
    }
}

void AppConfig::addRecentProject(
    const std::string& path)  // path 必须为 UTF-8 编码
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto&                       list = m_editorConfig.recentProjects;
        int limit = m_editorConfig.settings.recentProjectsLimit;

        // 1. 移除已存在的相同路径 (去重)
        list.erase(std::remove(list.begin(), list.end(), path), list.end());

        // 2. 插入到最前面
        list.insert(list.begin(), path);

        // 3. 限制数量
        if ( list.size() > static_cast<size_t>(limit) ) {
            list.resize(static_cast<size_t>(limit));
        }
    }

    // 4. 自动保存
    save();
}

std::filesystem::path AppConfig::getDefaultConfigPath() const
{
    return AppPaths::userConfigFilePath();
}

}  // namespace MMM::Config
