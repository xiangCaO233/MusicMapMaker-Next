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

enum class BackgroundFillMode {
    Stretch,     // 拉伸填充 (不保持比例)
    AspectFit,   // 保持比例缩放以适应 (显示全貌，可能有黑边)
    AspectFill,  // 保持比例缩放并填充 (裁切边缘，无黑边)
    Center       // 原始大小居中
};

struct BackgroundConfig {
    /// @brief 填充模式
    BackgroundFillMode fillMode{ BackgroundFillMode::AspectFill };
    /// @brief 背景暗化比例-0.0代表完全透明，1.0代表完全显示
    /// (与之前命名相反，调整为 opacity 更直观，但用户说 darken_ratio) 用户说:
    /// 背景暗化比例-1.0代表完全黑色. 那就是 0.0 是原图，1.0 是全黑。
    float darken_ratio{ .7f };
    /// @brief 背景不透明度-0.0代表完全透明
    float opaque_ratio{ 1.f };
};

/// @brief 编辑器配置
struct EditorConfig {
    /// @brief 轨道布局
    TrackLayout trackLayout;

    /// @brief 背景配置
    BackgroundConfig background;

    /// @brief 轨道布局包围框线宽(px)
    float trackBoxLineWidth{ 2 };

    /// @brief 判定线y比例位置
    float judgeline_pos{ .85f };

    /// @brief 判定线线宽(px)
    float judgelineWidth{ 4 };

    /// @brief 物件尺寸缩放
    float noteScaleX{ 1.25f };
    float noteScaleY{ 1.25f };

    /// @brief 物件填充模式
    BackgroundFillMode noteFillMode{ BackgroundFillMode::Stretch };
};

}  // namespace MMM::Logic
