#pragma once

#include "logic/ProjectTypes.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{
/// @brief 新建谱面命令结果。
struct CreateBeatmapResult {
    /// @brief 是否成功创建并登记谱面。
    bool m_created{ false };

    /// @brief 新建的谱面实例，用于交给会话立即打开。
    std::shared_ptr<BeatMap> m_beatmap;

    /// @brief 新建谱面打开到画布时使用的显示名称。
    std::string m_displayName;
};

/// @brief 导入音频命令结果。
struct ImportAudioResult {
    /// @brief 是否成功导入音频并登记资源。
    bool m_imported{ false };

    /// @brief 新导入资源若为音效，则需要由引擎登记。
    std::optional<AudioRegistrationRequest> m_effectRegistration;
};

/// @brief 更新音频资源命令结果。
struct UpdateAudioResourceResult {
    /// @brief 是否成功找到并更新音频资源。
    bool m_updated{ false };

    /// @brief 阻止 Effect 转 Main 的 Note 绑定所在谱面列表。
    std::vector<std::string> m_blockingBeatmapPaths;

    /// @brief 更新后若资源变成音效，则需要由引擎登记。
    std::optional<AudioRegistrationRequest> m_effectRegistration;

    /// @brief 更新前若资源是音效且现已变成主音轨，需要卸载的音效 ID。
    std::optional<std::string> m_effectResourceIdToUnload;
};

/// @brief 删除音频资源命令结果。
struct RemoveAudioResourceResult {
    /// @brief 是否从项目资源列表中删除了音频资源。
    bool m_removed{ false };

    /// @brief 阻止删除的 Note 或自动采样引用所在谱面列表。
    std::vector<std::string> m_blockingBeatmapPaths;

    /// @brief 被删除资源若为音效，则需要由引擎卸载音效缓存。
    std::optional<std::string> m_effectResourceIdToUnload;

    /// @brief 删除失败时供调用方显示的具体原因。
    std::string m_errorMessage;
};

/// @brief 项目资源列表变更结果。
struct ProjectMutationResult {
    /// @brief 本次操作是否改变了项目配置。
    bool m_changed{ false };
};

}  // namespace MMM::Logic
