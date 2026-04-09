#pragma once
#include <nlohmann/json.hpp>

namespace MMM::Config
{

enum class SyncMode {
    None,      ///< 直接同步音频时间 (可能抖动)
    Integral,  ///< 积分制同步 (平滑追踪)
    WaterTank  ///< 水箱制同步 (固定延迟)
};

NLOHMANN_JSON_SERIALIZE_ENUM(SyncMode, {
                                           { SyncMode::None, "None" },
                                           { SyncMode::Integral, "Integral" },
                                           { SyncMode::WaterTank, "WaterTank" },
                                       })

struct SyncConfig {
    SyncMode mode{ SyncMode::Integral };
    float    integralFactor{ 0.1f };    ///< 积分追踪系数 (0.0~1.0)
    float    waterTankBuffer{ 0.05f };  ///< 水箱缓冲时间 (秒)
    double   syncInterval{ 10.0 };      ///< 强制同步周期 (秒)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SyncConfig, mode, integralFactor,
                                   waterTankBuffer, syncInterval)
};

enum class PolylineSfxStrategy {
    Exact,             ///< 策略一: 所有子物件精确按照他们的类型播放对应音效
    InternalAsNormal,  ///< 策略二: 仅"内部"子物件播放普通Note音效
    OnlyTailExact,     ///< 策略三: 仅尾部子物件按类型播放
    AllAsNormal        ///< 策略四: 全部子物件均播放普通Note音效
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    PolylineSfxStrategy,
    {
        { PolylineSfxStrategy::Exact, "Exact" },
        { PolylineSfxStrategy::InternalAsNormal, "InternalAsNormal" },
        { PolylineSfxStrategy::OnlyTailExact, "OnlyTailExact" },
        { PolylineSfxStrategy::AllAsNormal, "AllAsNormal" },
    })

struct SfxConfig {
    /// @brief 折线内部子物件音效播放策略
    PolylineSfxStrategy polylineStrategy{ PolylineSfxStrategy::Exact };

    /// @brief Flick类型音效的播放是否跟随滑动轨道数量进行增益
    bool enableFlickWidthVolumeScaling{ false };

    /// @brief 每增加一个轨道的增益倍率
    float flickWidthVolumeMultiplier{ 0.1f };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SfxConfig, polylineStrategy,
                                   enableFlickWidthVolumeScaling,
                                   flickWidthVolumeMultiplier)
};

enum class FilePickerStyle {
    Native,  ///< 系统原生对话框 (nfd-extended)
    Unified  ///< 统一风格对话框 (ImGuiFileDialog)
};

NLOHMANN_JSON_SERIALIZE_ENUM(FilePickerStyle,
                             {
                                 { FilePickerStyle::Native, "Native" },
                                 { FilePickerStyle::Unified, "Unified" },
                             })

enum class CursorStyle {
    Software,  ///< 软件内置光标 (CursorManager)
    System     ///< 系统原生光标
};

NLOHMANN_JSON_SERIALIZE_ENUM(CursorStyle,
                             {
                                 { CursorStyle::Software, "Software" },
                                 { CursorStyle::System, "System" },
                             })

/// @brief 编辑器行为与功能相关的配置
struct EditorSettings {
    /// @brief 渲染同步配置
    SyncConfig syncConfig;

    /// @brief 编辑器音效触发配置
    SfxConfig sfxConfig;

    /// @brief 文件选择器样式
    FilePickerStyle filePickerStyle{ FilePickerStyle::Unified };

    /// @brief 光标样式
    CursorStyle cursorStyle{ CursorStyle::Software };

    /// @brief 节拍切分/分拍数 (例如 4 代表四分音符)
    int beatDivisor{ 4 };

    /// @brief 是否反转鼠标滚动方向
    bool reverseScroll{ false };

    /// @brief 是否开启滚动吸附
    bool scrollSnap{ false };

    /// @brief 最近打开项目的显示上限
    int recentProjectsLimit{ 10 };

    /// @brief 语言设置 (zh_cn, en_us)
    std::string language{ "zh_cn" };

    // TODO: 后续可在此添加自动保存(AutoSave)等配置

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(EditorSettings, syncConfig, sfxConfig,
                                   filePickerStyle, cursorStyle, beatDivisor,
                                   reverseScroll, scrollSnap,
                                   recentProjectsLimit, language)
};

}  // namespace MMM::Config
