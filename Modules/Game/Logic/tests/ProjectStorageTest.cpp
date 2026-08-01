#include "logic/ProjectStorage.h"

#include "log/colorful-log.h"
#include "logic/ProjectDirectoryScanner.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>

namespace
{

/// @brief 校验测试条件并记录失败原因。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("Project storage test failed: {}", message);
    }
    return condition;
}

/// @brief 创建仅供本测试使用的唯一临时目录。
std::filesystem::path createTestRoot()
{
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("mmm_project_storage_test_" + std::to_string(suffix));
    std::error_code filesystemError;
    std::filesystem::create_directories(root, filesystemError);
    return filesystemError ? std::filesystem::path{} : root;
}

/// @brief 构造覆盖全部分片职责的项目数据。
MMM::Project makeProject()
{
    MMM::Project project;
    project.m_metadata.m_title                            = "Split Project";
    project.m_metadata.m_artist                           = "Artist";
    project.m_settings.m_lastOpenedBeatmap                = "Hard";
    project.m_settings.m_workspace.m_activeBeatmapPath    = "hard.mmm";
    project.m_settings.m_workspace.m_projectAudioToolOpen = true;
    project.m_settings.m_workspace.m_projectAudioToolSelectedResourceId = "hit";
    project.m_settings.m_workspace.m_projectAudioToolBrushVolume        = 0.65F;
    project.m_settings.m_workspace.m_projectAudioToolPlacements         = {
        MMM::ProjectAudioToolItemPlacement{
            .m_audioResourceId = "hit",
            .m_x               = 12.0F,
            .m_y               = 34.0F,
            .m_width           = 128.0F,
            .m_height          = 96.0F,
            .m_zOrder          = 2,
        },
    };
    project.m_audioResources = {
        MMM::AudioResource{
            .m_id   = "main",
            .m_path = "bgm.ogg",
            .m_type = MMM::AudioTrackType::Main,
        },
        MMM::AudioResource{
            .m_id   = "hit",
            .m_path = "hit.wav",
            .m_type = MMM::AudioTrackType::Effect,
        },
    };
    project.m_beatmaps = {
        MMM::Project::BeatmapEntry{
            .m_name     = "Hard",
            .m_filePath = "hard.mmm",
        },
    };
    project.m_excludedAudioPaths = { "unused.wav" };
    return project;
}

/// @brief 将旧版项目顶层 JSON 写入根目录。
bool writeLegacyProjectFile(const std::filesystem::path& root,
                            const MMM::Project&          project)
{
    std::ofstream file(root / "mmm_project.json", std::ios::trunc);
    if ( !file.is_open() ) return false;
    file << std::setw(4) << nlohmann::json(project) << '\n';
    return file.good();
}

/// @brief 验证分片保存、职责拆分和完整往返。
bool testSplitRoundTrip(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    std::string                errorMessage;
    const auto                 project = makeProject();
    if ( !storage.save(project, root, errorMessage) ) {
        XERROR("Failed to save split project: {}", errorMessage);
        return false;
    }

    const auto       directory = root / ".mmm";
    const std::array expectedFiles{
        "manifest.json",           "project.json",  "settings.json",
        "audio_resources.json",    "beatmaps.json", "workspace.json",
        "project_audio_tool.json",
    };
    for ( const auto* filename : expectedFiles ) {
        if ( !check(std::filesystem::is_regular_file(directory / filename),
                    "all split files should exist") ) {
            return false;
        }
    }

    nlohmann::json settingsJson;
    nlohmann::json workspaceJson;
    nlohmann::json audioToolJson;
    {
        std::ifstream settingsFile(directory / "settings.json");
        std::ifstream workspaceFile(directory / "workspace.json");
        std::ifstream audioToolFile(directory / "project_audio_tool.json");
        settingsJson  = nlohmann::json::parse(settingsFile, nullptr, false);
        workspaceJson = nlohmann::json::parse(workspaceFile, nullptr, false);
        audioToolJson = nlohmann::json::parse(audioToolFile, nullptr, false);
    }
    if ( !check(!settingsJson.contains("m_workspace"),
                "workspace should not remain in general settings") ||
         !check(!workspaceJson.contains("m_projectAudioToolPlacements"),
                "audio tool layout should not remain in general workspace") ||
         !check(std::abs(
                    audioToolJson.value("m_projectAudioToolBrushVolume", 0.0F) -
                    0.65F) < 1e-6F,
                "audio tool brush volume should use its own file") ||
         !check(audioToolJson["m_projectAudioToolPlacements"].size() == 1,
                "audio tool layout should use its own file") ) {
        return false;
    }

    const auto loaded = storage.load(root);
    return check(loaded.m_success, "split project should load") &&
           check(loaded.m_source == MMM::Logic::ProjectStorage::Source::Split,
                 "split project should report split source") &&
           check(loaded.m_project.m_metadata.m_title == "Split Project",
                 "metadata should round trip") &&
           check(loaded.m_project.m_audioResources.size() == 2,
                 "audio resources should round trip") &&
           check(loaded.m_project.m_beatmaps.size() == 1,
                 "beatmaps should round trip") &&
           check(loaded.m_project.m_settings.m_workspace
                         .m_projectAudioToolPlacements.size() == 1,
                 "audio tool layout should round trip") &&
           check(std::abs(loaded.m_project.m_settings.m_workspace
                              .m_projectAudioToolBrushVolume -
                          0.65F) < 1e-6F,
                 "audio tool brush volume should round trip") &&
           check(std::abs(loaded.m_project.m_settings.m_workspace
                              .m_projectAudioToolPlacements.front()
                              .m_width -
                          128.0F) < 1e-6F,
                 "custom block size should round trip");
}

/// @brief 验证资源扫描不会把内部配置目录中的文件识别为谱面。
bool testInternalDirectoryIsNotScanned(const std::filesystem::path& root)
{
    {
        std::ofstream visibleBeatmap(root / "visible.mmm");
        std::ofstream internalBeatmap(root / ".mmm" / "internal.mmm");
        if ( !visibleBeatmap.is_open() || !internalBeatmap.is_open() ) {
            return false;
        }
    }

    const MMM::Logic::ProjectDirectoryScanner scanner;
    const auto                                result = scanner.scan(root);
    return check(result.m_success, "project scan should complete") &&
           check(result.m_beatmapFiles.size() == 1,
                 "internal .mmm files should not be scanned") &&
           check(result.m_beatmapFiles.front().filename() ==
                     std::filesystem::path("visible.mmm"),
                 "visible project beatmap should remain discoverable");
}

/// @brief 验证新分片损坏时仍可回退旧文件并在成功迁移后移除旧文件。
bool testLegacyFallbackAndRemoval(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    const auto                 project = makeProject();
    if ( !writeLegacyProjectFile(root, project) ) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(root / ".mmm", filesystemError);
    if ( filesystemError ) return false;
    {
        std::ofstream manifest(root / ".mmm" / "manifest.json");
        manifest << R"({"format_version":1,"storage":"split"})";
    }

    const auto loaded = storage.load(root);
    if ( !check(loaded.m_success,
                "legacy project should load when split files are incomplete") ||
         !check(loaded.m_source == MMM::Logic::ProjectStorage::Source::Legacy,
                "incomplete split project should fall back to legacy") ) {
        return false;
    }

    std::string errorMessage;
    if ( !storage.save(loaded.m_project, root, errorMessage) ) {
        XERROR("Failed to migrate legacy project: {}", errorMessage);
        return false;
    }
    if ( !storage.removeLegacyProjectFile(root, errorMessage) ) {
        XERROR("Failed to remove legacy project: {}", errorMessage);
        return false;
    }
    return check(!std::filesystem::exists(root / "mmm_project.json"),
                 "legacy project file should be removed after migration") &&
           check(storage.load(root).m_source ==
                     MMM::Logic::ProjectStorage::Source::Split,
                 "migrated project should load from split storage");
}

}  // namespace

/// @brief 运行项目分片存储与旧格式迁移测试。
int main()
{
    const auto root = createTestRoot();
    if ( root.empty() ) return 1;
    const auto      fallbackRoot = root / "legacy";
    std::error_code filesystemError;
    std::filesystem::create_directories(fallbackRoot, filesystemError);

    const bool success = !filesystemError && testSplitRoundTrip(root) &&
                         testInternalDirectoryIsNotScanned(root) &&
                         testLegacyFallbackAndRemoval(fallbackRoot);
    std::filesystem::remove_all(root, filesystemError);
    return success ? 0 : 1;
}
