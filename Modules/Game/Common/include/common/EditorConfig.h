#pragma once

namespace MMM::Common
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

struct PreviewAreaConfig {
    /// @brief 预览区视口范围相对主画布的倍率
    /// @note 若主画布的时间范围视口为1000ms,
    /// 则预览区时间范围视口为1000ms*5.f
    float areaRatio{ 5.f };

    /// @brief 预览区留白(px)
    struct AreaMargin {
        float left{ 4.f };
        float top{ 4.f };
        float right{ 4.f };
        float bottom{ 4.f };
    };
    AreaMargin margin;
};

enum class SyncMode {
    None,      ///< 直接同步音频时间 (可能抖动)
    Integral,  ///< 积分制同步 (平滑追踪)
    WaterTank  ///< 水箱制同步 (固定延迟)
};

struct SyncConfig {
    SyncMode mode{ SyncMode::Integral };
    float    integralFactor{ 0.1f };    ///< 积分追踪系数 (0.0~1.0)
    float    waterTankBuffer{ 0.05f };  ///< 水箱缓冲时间 (秒)
    double   syncInterval{
        10.0
    };  ///< 强制同步周期 (秒)，例如 0.02 代表每 20ms 同步一次音频时钟
};

/// @brief 编辑器配置
struct EditorConfig {
    /// @brief 轨道布局
    TrackLayout trackLayout;

    /// @brief 背景配置
    BackgroundConfig background;

    /// @brief 预览区配置
    PreviewAreaConfig previewConfig;

    /// @brief 同步配置
    SyncConfig syncConfig;

    enum class PolylineSfxStrategy {
        Exact,             ///< 策略一: 所有子物件精确按照他们的类型播放对应音效
        InternalAsNormal,  ///< 策略二:
                           ///< 仅"内部"子物件播放普通Note音效(开头和结尾按类型)
        OnlyTailExact,     ///< 策略三:
                           ///< 仅尾部子物件按类型播放，其他播放普通Note音效
        AllAsNormal        ///< 策略四: 全部子物件均播放普通Note音效
    };

    struct SfxConfig {
        /// @brief 折线内部子物件音效播放策略
        PolylineSfxStrategy polylineStrategy{ PolylineSfxStrategy::Exact };

        /// @brief Flick类型音效的播放是否跟随滑动轨道数量进行增益
        bool enableFlickWidthVolumeScaling{ false };

        /// @brief 每增加一个轨道的增益倍率
        float flickWidthVolumeMultiplier{ 0.1f };
    };

    /// @brief 音效配置
    SfxConfig sfxConfig;

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

    /// @brief 视觉偏移量 (秒)
    /// @note 正值代表物件提前显示，负值代表物件延后显示
    float visualOffset{ 0.0f };
};

}  // namespace MMM::Common
