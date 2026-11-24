#pragma once

#include "config/EditorSettings.h"
#include "mmm/project/ProjectWorkspaceState.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{
class Project;
}

namespace MMM::Logic
{

/// @brief 项目设置控件可读取和编辑的持久化字段。
struct ProjectUiSettings {
    /// @brief 空字符串继承全局调色板。
    std::string m_colorPaletteSchemeName;
    /// @brief 空值继承全局自动备份策略。
    std::optional<Config::AutoBackupConfig> m_autoBackupOverride;
};

/// @brief 音频工具一次交互所提交的选择和布局；其余工作区字段按差量处理。
struct ProjectAudioToolLayoutEdit {
    /// @brief 本次交互结束后选中的资源及画笔倍率。
    std::string m_selectedResourceId;
    /// @brief 所选采样用于新物件的音量，独立于源音轨的增益。
    float m_brushVolume{ 1.0F };
    /// @brief 与本次选择同步提交的资源方块布局。
    std::vector<ProjectAudioToolItemPlacement> m_placements;
};

/// @brief 控件实际修改的字段。外层空值保留原设置，内层空值清除覆盖。
struct ProjectUiSettingsEdit {
    /// @brief 设置页面读取的工程代次，关闭后同目录重开也不能复用。
    std::uint64_t m_generation{};
    /// @brief 有值时替换调色板方案。
    std::optional<std::string> m_colorPaletteSchemeName;
    /// @brief 有值时替换或清除备份策略。
    std::optional<std::optional<Config::AutoBackupConfig>> m_autoBackupOverride;
    /// @brief 侧栏完成切换后保存的页签身份。
    std::optional<std::string> m_sidebarActiveTab;
    /// @brief 仅在控件修改时携带音效试听开关。
    std::optional<bool> m_audioToolPreview;
    /// @brief 仅在控件修改时携带布局吸附开关。
    std::optional<bool> m_audioToolSnap;
    /// @brief 仅在控件修改时携带方块音量显示开关。
    std::optional<bool> m_audioToolShowVolume;
    /// @brief 拖动或音量编辑结束时一起提交方块布局和画笔状态。
    std::optional<ProjectAudioToolLayoutEdit> m_audioToolLayout;
};

/// @brief UI 可用于展示或选择的主音频信息，省去解码及混音配置。
struct ProjectUiAudioEntry {
    /// @brief 项目内的稳定资源 ID。
    std::string m_id;
    /// @brief 资源的项目相对路径。
    std::string m_path;
};

/// @brief 已发布的项目视图，只包含新建向导和项目设置所需的数据。
/// @details 生命周期转移及保存时构建，发布后不修改；关闭项目不会使已借用的
/// 结果失效。路径不能充当代次，同目录重开或另存转正都废弃旧写请求。
struct ProjectUiState {
    /// @brief 工程打开实例的代次。
    std::uint64_t m_generation{};
    /// @brief 工程文件的解析根。
    std::filesystem::path m_projectRoot;
    /// @brief 临时缓存工程只允许读取。
    bool m_isTemporaryProject{};
    /// @brief 新谱面可继承的歌曲标题。
    std::string m_title;
    /// @brief 新谱面可继承的曲作者。
    std::string m_artist;
    /// @brief 新谱面可继承的谱面作者。
    std::string m_mapper;
    /// @brief 用户可以在向导中选择的主音轨。
    std::vector<ProjectUiAudioEntry> m_audioResources;
    /// @brief 仅用于用户创建前的重名提示。
    std::vector<std::string> m_beatmapNames;
    /// @brief 项目设置页呈现的配置。
    ProjectUiSettings m_settings;
    /// @brief 为项目音频、图集和打包发布的独立只读投影。
    /// @note 仅包含资源、谱面目录及相关工作区字段，不复制草稿轨或运行时对象。
    std::shared_ptr<const Project> m_details;
};

}  // namespace MMM::Logic
