#pragma once

#include <cstdint>

namespace MMM::Common
{

/// @brief 移动播放控件读取的活动谱面状态，不借用会话或音频对象。
struct EditorPlaybackState {
    /// @brief 活动目标代次，切换或替换谱面时使旧弹层失效。
    std::uint64_t generation{};
    /// @brief 玩家轨道数，零表示当前没有已载入谱面。
    std::int32_t playerTracks{};
    /// @brief 活动谱面正在控制播放；后台跟随和等待音频准备不计入。
    bool playing{};

    /// @brief 用于避免重复发布未变化的播放状态。
    bool operator==(const EditorPlaybackState&) const = default;
};

}  // namespace MMM::Common
