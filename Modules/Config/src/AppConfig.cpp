#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

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
    : m_collaborationParticipantId(makeCollaborationStableId())
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

    std::ifstream file(finalPath);
    if ( !file.is_open() ) {
        XERROR("Failed to open config file for reading: {}",
               pathToUtf8(finalPath));
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if ( j.is_discarded() || !j.is_object() || file.bad() ) {
        XERROR("Failed to parse config file: {}", pathToUtf8(finalPath));
        return false;
    }

    std::string serializedCollaborationParticipantId;
    if ( const auto identity = j.find("collaborationParticipantId");
         identity != j.end() && identity->is_string() ) {
        serializedCollaborationParticipantId = identity->get<std::string>();
    }
    const auto collaborationParticipantId =
        normalizeCollaborationStableId(serializedCollaborationParticipantId);
    const bool generatedCollaborationParticipantId =
        collaborationParticipantId.empty();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_editorConfig = j.get<EditorConfig>();
        if ( !generatedCollaborationParticipantId ) {
            m_collaborationParticipantId = collaborationParticipantId;
        } else if ( m_collaborationParticipantId.empty() ) {
            m_collaborationParticipantId = makeCollaborationStableId();
        }
    }

    XINFO("Config loaded successfully from: {}", pathToUtf8(finalPath));
    if ( useDefaultPath && (finalPath != getDefaultConfigPath() ||
                            generatedCollaborationParticipantId) ) {
        save();
    }
    return true;
}

bool AppConfig::save(const std::filesystem::path& path) const
{
    std::filesystem::path finalPath =
        path.empty() ? getDefaultConfigPath() : path;

    // 确保目录存在
    if ( auto parent = finalPath.parent_path(); !parent.empty() ) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if ( createDirectoryError ) {
            XERROR("Failed to create config directory: {}. Error: {}",
                   pathToUtf8(parent),
                   createDirectoryError.message());
            return false;
        }
    }

    nlohmann::json j;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        j                               = m_editorConfig;
        j["collaborationParticipantId"] = m_collaborationParticipantId;
    }

    std::filesystem::path tempPath = finalPath;
    tempPath += ".tmp";
    {
        std::ofstream file(tempPath);
        if ( !file.is_open() ) {
            XERROR("Failed to open config temp file for writing: {}",
                   pathToUtf8(tempPath));
            return false;
        }

        file << std::setw(4) << j << '\n';
        if ( !file.good() ) {
            XERROR("Failed to write config temp file: {}",
                   pathToUtf8(tempPath));
            return false;
        }
    }

    std::error_code replaceError;
    std::filesystem::rename(tempPath, finalPath, replaceError);
    if ( replaceError ) {
        std::error_code copyError;
        std::filesystem::copy_file(
            tempPath,
            finalPath,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        std::error_code removeTempError;
        std::filesystem::remove(tempPath, removeTempError);
        if ( copyError ) {
            XERROR("Failed to replace config file: {}. Error: {}",
                   pathToUtf8(finalPath),
                   copyError.message());
            return false;
        }
    }

    return true;
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
