#include "config/AppConfig.h"
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
    std::filesystem::path finalPath =
        path.empty() ? getDefaultConfigPath() : path;

    if ( !std::filesystem::exists(finalPath) ) {
        XINFO("Config file not found: {}. Using default values.",
              finalPath.string());
        return false;
    }

    try {
        std::ifstream file(finalPath);
        if ( !file.is_open() ) {
            XERROR("Failed to open config file for reading: {}",
                   finalPath.string());
            return false;
        }

        nlohmann::json j;
        file >> j;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_editorConfig = j.get<EditorConfig>();

        XINFO("Config loaded successfully from: {}", finalPath.string());
        return true;
    } catch ( const std::exception& e ) {
        XERROR("Failed to parse config file: {}. Error: {}",
               finalPath.string(),
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
                   finalPath.string());
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
               finalPath.string(),
               e.what());
        return false;
    }
}

void AppConfig::addRecentProject(const std::string& path)
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
    // 默认存放在当前可执行文件同级或预定义的配置目录下
    // 这里暂时使用当前目录下的 user_config.json
    return "user_config.json";
}

}  // namespace MMM::Config
