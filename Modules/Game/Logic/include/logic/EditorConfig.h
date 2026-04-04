#pragma once

namespace MMM::Logic
{

struct TrackLayout {
    /// @brief 左侧分隔比例位置
    float left{ .2f };
    /// @brief 顶部分隔比例位置
    float top{ .05f };
    /// @brief 右侧分隔比例位置
    float right{ .8f };
    /// @brief 底部分隔比例位置
    float bottom{ .95f };
};

/// @brief 编辑器配置
struct EditorConfig {
    /// @brief 轨道布局
    TrackLayout trackLayout;

    /// @brief 轨道布局包围框线宽(px)
    float trackBoxLineWidth{ 2 };

    /// @brief 判定线y比例位置
    float judgeline_pos{ .85f };

    /// @brief 判定线线宽(px)
    float judgelineWidth{ 4 };

    /// @brief 物件尺寸缩放
    float noteScaleX{ .975f };
    float noteScaleY{ .975f };
};

}  // namespace MMM::Logic
