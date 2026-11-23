#pragma once

#include <cstdint>

namespace MMM::Common
{

/// @brief 活动谱面的轨道布局，只包含工具栏展示需要的值。
/// @note 草稿与 BGM 保留各自完整整数范围；有效谱面可以没有这些轨道。
struct EditorTrackLayout {
    /// @brief 当前是否已绑定一份可读取轨道布局的谱面。
    bool hasBeatmap{};
    /// @brief 玩家区元数据轨道数。
    std::int32_t playerTracks{};
    /// @brief 项目草稿区轨道数，由当前谱面的草稿组确定。
    std::int32_t draftTracks{};
    /// @brief 自动采样区轨道数，不含玩家区偏移。
    std::int32_t bgmTracks{};

    /// @brief 单独比较布局值，使常态逻辑更新无需再次发布。
    bool operator==(const EditorTrackLayout&) const = default;
};

}  // namespace MMM::Common
