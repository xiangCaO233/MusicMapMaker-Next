#pragma once
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{

/// @brief 项目工作区中一个已打开谱面的运行时状态。
struct ProjectWorkspaceBeatmapState {
    /// @brief 谱面文件路径，优先使用相对项目根目录的 UTF-8 路径。
    std::string m_filePath;

    /// @brief 谱面上次绑定的画布 ID，用于匹配 ImGui 停靠布局。
    std::string m_cameraId;

    /// @brief 画布标题或谱面显示名称。
    std::string m_displayName;

    /// @brief 该谱面上次停留的逻辑播放时间（秒）。
    double m_playbackTime{ 0.0 };

    /// @brief 序列化项目工作区谱面状态。
    friend void to_json(nlohmann::json&                     j,
                        const ProjectWorkspaceBeatmapState& state)
    {
        j = nlohmann::json{ { "m_filePath", state.m_filePath },
                            { "m_cameraId", state.m_cameraId },
                            { "m_displayName", state.m_displayName },
                            { "m_playbackTime", state.m_playbackTime } };
    }

    /// @brief 反序列化项目工作区谱面状态，并兼容旧项目文件。
    friend void from_json(const nlohmann::json&         j,
                          ProjectWorkspaceBeatmapState& state)
    {
        state.m_filePath     = j.value("m_filePath", std::string{});
        state.m_cameraId     = j.value("m_cameraId", std::string{});
        state.m_displayName  = j.value("m_displayName", std::string{});
        state.m_playbackTime = j.value("m_playbackTime", 0.0);
    }
};

/// @brief 项目工作区中的主窗口位置和尺寸。
struct ProjectWorkspaceWindowState {
    /// @brief 是否已经记录过有效窗口状态。
    bool m_valid{ false };

    /// @brief 窗口左上角 X 坐标。
    int m_x{ 100 };

    /// @brief 窗口左上角 Y 坐标。
    int m_y{ 100 };

    /// @brief 窗口宽度。
    int m_width{ 1280 };

    /// @brief 窗口高度。
    int m_height{ 720 };

    /// @brief 保存时窗口是否最大化。
    bool m_maximized{ false };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectWorkspaceWindowState, m_valid, m_x,
                                   m_y, m_width, m_height, m_maximized)
};

/// @brief 项目工作区中已打开的音轨控制器窗口状态。
struct ProjectWorkspaceAudioControllerState {
    /// @brief 音频资源 ID 或常驻音效 ID。
    std::string m_trackId;

    /// @brief 控制器窗口显示名称。
    std::string m_trackName;

    /// @brief 音轨类型，使用 Main 或 Effect 的稳定文本。
    std::string m_trackType{ "Main" };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectWorkspaceAudioControllerState,
                                   m_trackId, m_trackName, m_trackType)
};

/// @brief 项目级工作区状态，用于再次打开项目时恢复编辑现场。
struct ProjectWorkspaceState {
    /// @brief 上次打开的谱面列表，顺序对应画布标签顺序。
    std::vector<ProjectWorkspaceBeatmapState> m_openBeatmaps;

    /// @brief 上次激活谱面的项目相对路径。
    std::string m_activeBeatmapPath;

    /// @brief 上次激活谱面的播放时间（秒）。
    double m_activePlaybackTime{ 0.0 };

    /// @brief 项目专属 Dear ImGui ini 数据，包含窗口位置和 Docking 节点。
    std::string m_imguiIniData;

    /// @brief 主原生窗口的位置、尺寸和最大化状态。
    ProjectWorkspaceWindowState m_mainWindow;

    /// @brief 上次打开的音轨控制器窗口列表。
    std::vector<ProjectWorkspaceAudioControllerState> m_audioControllers;

    /// @brief 上次是否打开了主音轨波形窗口。
    bool m_audioWaveformOpen{ false };

    /// @brief 上次是否打开了主音轨频谱窗口。
    bool m_audioSpectrumOpen{ false };

    /// @brief 上次打开的侧边栏页签，None 表示侧边栏内容收起。
    std::string m_sidebarActiveTab{ "FileExplorer" };

    /// @brief 上次选中的编辑工具。
    std::string m_activeEditTool{ "Move" };

    /// @brief 上次是否打开了 BPM 测量工具。
    bool m_bpmMeasurementToolOpen{ false };

    /// @brief BPM 测量工具上次选中的音频资源 ID。
    std::string m_bpmMeasurementAudioTrackId;

    /// @brief 上次是否打开了时间点批量编辑表格。
    bool m_timingPointsTableOpen{ false };

    /// @brief 上次是否打开了重叠检测工具窗口。
    bool m_overlapCheckOpen{ false };

    /// @brief 上次是否打开了谱面额外元数据编辑窗口。
    bool m_metadataEditorOpen{ false };

    /// @brief 上次是否打开了音符元数据编辑窗口。
    bool m_noteMetadataEditorOpen{ false };

    /// @brief 序列化项目工作区状态。
    friend void to_json(nlohmann::json&              j,
                        const ProjectWorkspaceState& workspace)
    {
        j = nlohmann::json{
            { "m_openBeatmaps", workspace.m_openBeatmaps },
            { "m_activeBeatmapPath", workspace.m_activeBeatmapPath },
            { "m_activePlaybackTime", workspace.m_activePlaybackTime },
            { "m_imguiIniData", workspace.m_imguiIniData },
            { "m_mainWindow", workspace.m_mainWindow },
            { "m_audioControllers", workspace.m_audioControllers },
            { "m_audioWaveformOpen", workspace.m_audioWaveformOpen },
            { "m_audioSpectrumOpen", workspace.m_audioSpectrumOpen },
            { "m_sidebarActiveTab", workspace.m_sidebarActiveTab },
            { "m_activeEditTool", workspace.m_activeEditTool },
            { "m_bpmMeasurementToolOpen", workspace.m_bpmMeasurementToolOpen },
            { "m_bpmMeasurementAudioTrackId",
              workspace.m_bpmMeasurementAudioTrackId },
            { "m_timingPointsTableOpen", workspace.m_timingPointsTableOpen },
            { "m_overlapCheckOpen", workspace.m_overlapCheckOpen },
            { "m_metadataEditorOpen", workspace.m_metadataEditorOpen },
            { "m_noteMetadataEditorOpen", workspace.m_noteMetadataEditorOpen }
        };
    }

    /// @brief 反序列化项目工作区状态，并兼容旧项目文件。
    friend void from_json(const nlohmann::json&  j,
                          ProjectWorkspaceState& workspace)
    {
        workspace.m_openBeatmaps = j.value(
            "m_openBeatmaps", std::vector<ProjectWorkspaceBeatmapState>{});
        workspace.m_activeBeatmapPath =
            j.value("m_activeBeatmapPath", std::string{});
        workspace.m_activePlaybackTime = j.value("m_activePlaybackTime", 0.0);
        workspace.m_imguiIniData = j.value("m_imguiIniData", std::string{});
        workspace.m_mainWindow =
            j.value("m_mainWindow", ProjectWorkspaceWindowState{});
        workspace.m_audioControllers =
            j.value("m_audioControllers",
                    std::vector<ProjectWorkspaceAudioControllerState>{});
        workspace.m_audioWaveformOpen = j.value("m_audioWaveformOpen", false);
        workspace.m_audioSpectrumOpen = j.value("m_audioSpectrumOpen", false);
        workspace.m_sidebarActiveTab =
            j.value("m_sidebarActiveTab", std::string{ "FileExplorer" });
        workspace.m_activeEditTool =
            j.value("m_activeEditTool", std::string{ "Move" });
        workspace.m_bpmMeasurementToolOpen =
            j.value("m_bpmMeasurementToolOpen", false);
        workspace.m_bpmMeasurementAudioTrackId =
            j.value("m_bpmMeasurementAudioTrackId", std::string{});
        workspace.m_timingPointsTableOpen =
            j.value("m_timingPointsTableOpen", false);
        workspace.m_overlapCheckOpen   = j.value("m_overlapCheckOpen", false);
        workspace.m_metadataEditorOpen = j.value("m_metadataEditorOpen", false);
        workspace.m_noteMetadataEditorOpen =
            j.value("m_noteMetadataEditorOpen", false);
    }
};

/// @brief 项目级的特定偏好设置
/// @details 包含可以覆盖全局配置的可选项，以及最后一次的状态记录
struct ProjectSettings {
    /// @brief 覆盖全局视觉配置 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::VisualConfig> m_visualOverride;

    /// @brief 覆盖全局编辑器行为 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::EditorSettings> m_editorOverride;

    /// @brief 项目中最后一次打开的谱面名称 (BeatmapEntry::m_name)
    std::string m_lastOpenedBeatmap;

    /// @brief 项目级工作区状态。
    ProjectWorkspaceState m_workspace;

    /// @brief 序列化项目设置。
    friend void to_json(nlohmann::json& j, const ProjectSettings& settings)
    {
        j = nlohmann::json{ { "m_visualOverride", settings.m_visualOverride },
                            { "m_editorOverride", settings.m_editorOverride },
                            { "m_lastOpenedBeatmap",
                              settings.m_lastOpenedBeatmap },
                            { "m_workspace", settings.m_workspace } };
    }

    /// @brief 反序列化项目设置，并兼容缺少工作区字段的旧项目。
    friend void from_json(const nlohmann::json& j, ProjectSettings& settings)
    {
        if ( auto it = j.find("m_visualOverride");
             it != j.end() && !it->is_null() ) {
            settings.m_visualOverride = it->get<Config::VisualConfig>();
        } else {
            settings.m_visualOverride = std::nullopt;
        }

        if ( auto it = j.find("m_editorOverride");
             it != j.end() && !it->is_null() ) {
            settings.m_editorOverride = it->get<Config::EditorSettings>();
        } else {
            settings.m_editorOverride = std::nullopt;
        }

        settings.m_lastOpenedBeatmap =
            j.value("m_lastOpenedBeatmap", std::string{});
        settings.m_workspace = j.value("m_workspace", ProjectWorkspaceState{});
    }
};

}  // namespace MMM
