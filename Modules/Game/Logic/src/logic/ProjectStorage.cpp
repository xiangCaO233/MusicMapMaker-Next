#include "logic/ProjectStorage.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Logic
{
namespace
{

/// @brief 当前分片项目存储格式版本。
constexpr int SPLIT_STORAGE_FORMAT_VERSION = 1;

/// @brief 隐藏项目配置目录名。
constexpr std::string_view STORAGE_DIRECTORY_NAME = ".mmm";

/// @brief 分片格式入口文件名。
constexpr std::string_view MANIFEST_FILE_NAME = "manifest.json";

/// @brief 项目元数据与排除项文件名。
constexpr std::string_view PROJECT_FILE_NAME = "project.json";

/// @brief 项目设置文件名。
constexpr std::string_view SETTINGS_FILE_NAME = "settings.json";

/// @brief 音频资源文件名。
constexpr std::string_view AUDIO_RESOURCES_FILE_NAME = "audio_resources.json";

/// @brief 谱面入口文件名。
constexpr std::string_view BEATMAPS_FILE_NAME = "beatmaps.json";

/// @brief 一般项目工作区文件名。
constexpr std::string_view WORKSPACE_FILE_NAME = "workspace.json";

/// @brief 项目音频工具状态与布局文件名。
constexpr std::string_view AUDIO_TOOL_FILE_NAME = "project_audio_tool.json";

/// @brief 旧版根目录项目配置文件名。
constexpr std::string_view LEGACY_PROJECT_FILE_NAME = "mmm_project.json";

/// @brief 读取一个 JSON 文件并要求其顶层是对象。
bool readJsonObject(const std::filesystem::path& path, nlohmann::json& output,
                    std::string& errorMessage)
{
    std::ifstream file(path);
    if ( !file.is_open() ) {
        errorMessage = "无法打开配置分片：" + Config::pathToUtf8(path);
        return false;
    }
    output = nlohmann::json::parse(file, nullptr, false);
    if ( output.is_discarded() || !output.is_object() || file.bad() ) {
        errorMessage = "无法解析配置分片：" + Config::pathToUtf8(path);
        return false;
    }
    return true;
}

/// @brief 以临时文件替换方式写入一个 JSON 分片。
bool writeJsonAtomically(const std::filesystem::path& path,
                         const nlohmann::json& value, std::string& errorMessage)
{
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    {
        std::ofstream file(tempPath, std::ios::trunc);
        if ( !file.is_open() ) {
            errorMessage =
                "无法创建配置临时文件：" + Config::pathToUtf8(tempPath);
            return false;
        }
        file << std::setw(4) << value << '\n';
        if ( !file.good() ) {
            errorMessage =
                "无法写入配置临时文件：" + Config::pathToUtf8(tempPath);
            return false;
        }
    }

    std::error_code replaceError;
    std::filesystem::rename(tempPath, path, replaceError);
    if ( !replaceError ) return true;

    std::error_code copyError;
    std::filesystem::copy_file(
        tempPath,
        path,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    std::error_code removeError;
    std::filesystem::remove(tempPath, removeError);
    if ( copyError ) {
        errorMessage = "无法替换配置分片：" + Config::pathToUtf8(path) +
                       "，错误：" + copyError.message();
        return false;
    }
    return true;
}

/// @brief 从分片内容组装与旧版 Project JSON 相同的结构。
nlohmann::json assembleProjectJson(const nlohmann::json& projectJson,
                                   nlohmann::json        settingsJson,
                                   const nlohmann::json& audioResourcesJson,
                                   const nlohmann::json& beatmapsJson,
                                   nlohmann::json        workspaceJson,
                                   const nlohmann::json& audioToolJson)
{
    workspaceJson["m_projectAudioToolSelectedResourceId"] = audioToolJson.value(
        "m_projectAudioToolSelectedResourceId", std::string{});
    workspaceJson["m_projectAudioToolBrushVolume"] =
        audioToolJson.value("m_projectAudioToolBrushVolume", 1.0F);
    workspaceJson["m_projectAudioToolOpen"] =
        audioToolJson.value("m_projectAudioToolOpen", false);
    workspaceJson["m_projectAudioToolPlacements"] = audioToolJson.value(
        "m_projectAudioToolPlacements", nlohmann::json::array());
    settingsJson["m_workspace"] = std::move(workspaceJson);

    return nlohmann::json{
        { "m_metadata", projectJson.at("m_metadata") },
        { "m_settings", std::move(settingsJson) },
        { "m_audioResources", audioResourcesJson.at("m_audioResources") },
        { "m_beatmaps", beatmapsJson.at("m_beatmaps") },
        { "m_excludedBeatmapPaths",
          projectJson.value("m_excludedBeatmapPaths",
                            std::vector<std::string>{}) },
        { "m_excludedAudioPaths",
          projectJson.value("m_excludedAudioPaths",
                            std::vector<std::string>{}) },
    };
}

}  // namespace

std::filesystem::path ProjectStorage::storageDirectory(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / STORAGE_DIRECTORY_NAME;
}

std::filesystem::path ProjectStorage::manifestPath(
    const std::filesystem::path& projectRoot)
{
    return storageDirectory(projectRoot) / MANIFEST_FILE_NAME;
}

std::filesystem::path ProjectStorage::legacyProjectFilePath(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / LEGACY_PROJECT_FILE_NAME;
}

bool ProjectStorage::hasProjectConfiguration(
    const std::filesystem::path& projectRoot)
{
    std::error_code filesystemError;
    if ( std::filesystem::is_regular_file(manifestPath(projectRoot),
                                          filesystemError) &&
         !filesystemError ) {
        return true;
    }
    filesystemError.clear();
    return std::filesystem::is_regular_file(legacyProjectFilePath(projectRoot),
                                            filesystemError) &&
           !filesystemError;
}

ProjectStorage::LoadResult ProjectStorage::load(
    const std::filesystem::path& projectRoot) const
{
    std::error_code filesystemError;
    const bool      splitExists = std::filesystem::is_regular_file(
                                      manifestPath(projectRoot), filesystemError) &&
                                  !filesystemError;
    filesystemError.clear();
    const bool legacyExists =
        std::filesystem::is_regular_file(legacyProjectFilePath(projectRoot),
                                         filesystemError) &&
        !filesystemError;

    if ( splitExists ) {
        auto splitResult = loadSplit(projectRoot);
        if ( splitResult.m_success || !legacyExists ) return splitResult;
        XWARN(
            "Failed to load split project storage, falling back to legacy "
            "file: {}",
            splitResult.m_errorMessage);
    }
    if ( legacyExists ) return loadLegacy(projectRoot);
    return {};
}

ProjectStorage::LoadResult ProjectStorage::loadSplit(
    const std::filesystem::path& projectRoot) const
{
    LoadResult result;
    result.m_source      = Source::Split;
    const auto directory = storageDirectory(projectRoot);

    nlohmann::json manifestJson;
    nlohmann::json projectJson;
    nlohmann::json settingsJson;
    nlohmann::json audioResourcesJson;
    nlohmann::json beatmapsJson;
    nlohmann::json workspaceJson;
    nlohmann::json audioToolJson;
    if ( !readJsonObject(directory / MANIFEST_FILE_NAME,
                         manifestJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / PROJECT_FILE_NAME,
                         projectJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / SETTINGS_FILE_NAME,
                         settingsJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / AUDIO_RESOURCES_FILE_NAME,
                         audioResourcesJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / BEATMAPS_FILE_NAME,
                         beatmapsJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / WORKSPACE_FILE_NAME,
                         workspaceJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / AUDIO_TOOL_FILE_NAME,
                         audioToolJson,
                         result.m_errorMessage) ) {
        return result;
    }

    if ( manifestJson.value("format_version", 0) !=
             SPLIT_STORAGE_FORMAT_VERSION ||
         !projectJson.contains("m_metadata") ||
         !audioResourcesJson.contains("m_audioResources") ||
         !audioResourcesJson["m_audioResources"].is_array() ||
         !beatmapsJson.contains("m_beatmaps") ||
         !beatmapsJson["m_beatmaps"].is_array() ) {
        result.m_errorMessage = "项目分片格式无效或版本不受支持";
        return result;
    }

    result.m_serializedProject = assembleProjectJson(projectJson,
                                                     std::move(settingsJson),
                                                     audioResourcesJson,
                                                     beatmapsJson,
                                                     std::move(workspaceJson),
                                                     audioToolJson);
    result.m_project           = result.m_serializedProject.get<Project>();
    result.m_success           = true;
    return result;
}

ProjectStorage::LoadResult ProjectStorage::loadLegacy(
    const std::filesystem::path& projectRoot) const
{
    LoadResult result;
    result.m_source = Source::Legacy;
    if ( !readJsonObject(legacyProjectFilePath(projectRoot),
                         result.m_serializedProject,
                         result.m_errorMessage) ) {
        return result;
    }
    if ( !result.m_serializedProject.contains("m_metadata") ||
         !result.m_serializedProject.contains("m_settings") ||
         !result.m_serializedProject.contains("m_audioResources") ||
         !result.m_serializedProject["m_audioResources"].is_array() ||
         !result.m_serializedProject.contains("m_beatmaps") ||
         !result.m_serializedProject["m_beatmaps"].is_array() ) {
        result.m_errorMessage = "旧项目配置结构无效";
        return result;
    }

    result.m_project = result.m_serializedProject.get<Project>();
    result.m_success = true;
    return result;
}

bool ProjectStorage::save(const Project&               project,
                          const std::filesystem::path& projectRoot,
                          std::string&                 errorMessage) const
{
    errorMessage.clear();
    const auto      directory = storageDirectory(projectRoot);
    std::error_code createDirectoryError;
    std::filesystem::create_directories(directory, createDirectoryError);
    if ( createDirectoryError ) {
        errorMessage = "无法创建项目配置目录：" +
                       Config::pathToUtf8(directory) + "，错误：" +
                       createDirectoryError.message();
        return false;
    }

    nlohmann::json settingsJson = project.m_settings;
    settingsJson.erase("m_workspace");
    nlohmann::json workspaceJson = project.m_settings.m_workspace;
    workspaceJson.erase("m_projectAudioToolSelectedResourceId");
    workspaceJson.erase("m_projectAudioToolBrushVolume");
    workspaceJson.erase("m_projectAudioToolOpen");
    workspaceJson.erase("m_projectAudioToolPlacements");

    const auto&          audioTool = project.m_settings.m_workspace;
    const nlohmann::json manifestJson{
        { "format_version", SPLIT_STORAGE_FORMAT_VERSION },
        { "storage", "split" },
    };
    const nlohmann::json projectJson{
        { "m_metadata", project.m_metadata },
        { "m_excludedBeatmapPaths", project.m_excludedBeatmapPaths },
        { "m_excludedAudioPaths", project.m_excludedAudioPaths },
    };
    const nlohmann::json audioResourcesJson{
        { "m_audioResources", project.m_audioResources },
    };
    const nlohmann::json beatmapsJson{
        { "m_beatmaps", project.m_beatmaps },
    };
    const nlohmann::json audioToolJson{
        { "m_projectAudioToolSelectedResourceId",
          audioTool.m_projectAudioToolSelectedResourceId },
        { "m_projectAudioToolBrushVolume",
          audioTool.m_projectAudioToolBrushVolume },
        { "m_projectAudioToolOpen", audioTool.m_projectAudioToolOpen },
        { "m_projectAudioToolPlacements",
          audioTool.m_projectAudioToolPlacements },
    };

    if ( !writeJsonAtomically(
             directory / PROJECT_FILE_NAME, projectJson, errorMessage) ||
         !writeJsonAtomically(
             directory / SETTINGS_FILE_NAME, settingsJson, errorMessage) ||
         !writeJsonAtomically(directory / AUDIO_RESOURCES_FILE_NAME,
                              audioResourcesJson,
                              errorMessage) ||
         !writeJsonAtomically(
             directory / BEATMAPS_FILE_NAME, beatmapsJson, errorMessage) ||
         !writeJsonAtomically(
             directory / WORKSPACE_FILE_NAME, workspaceJson, errorMessage) ||
         !writeJsonAtomically(
             directory / AUDIO_TOOL_FILE_NAME, audioToolJson, errorMessage) ) {
        return false;
    }

    return writeJsonAtomically(
        directory / MANIFEST_FILE_NAME, manifestJson, errorMessage);
}

bool ProjectStorage::removeLegacyProjectFile(
    const std::filesystem::path& projectRoot, std::string& errorMessage) const
{
    errorMessage.clear();
    const auto      legacyPath = legacyProjectFilePath(projectRoot);
    std::error_code filesystemError;
    const bool exists = std::filesystem::exists(legacyPath, filesystemError) &&
                        !filesystemError;
    if ( filesystemError ) {
        errorMessage = "无法检查旧项目配置：" + filesystemError.message();
        return false;
    }
    if ( !exists ) return true;

    if ( !std::filesystem::remove(legacyPath, filesystemError) ||
         filesystemError ) {
        errorMessage = "无法移除旧项目配置：" + Config::pathToUtf8(legacyPath);
        if ( filesystemError ) {
            errorMessage += "，错误：" + filesystemError.message();
        }
        return false;
    }
    return true;
}

}  // namespace MMM::Logic
