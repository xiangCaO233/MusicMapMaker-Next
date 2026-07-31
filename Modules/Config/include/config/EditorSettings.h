#pragma once
#include "config/BeatLinePalette.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

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
};

inline void to_json(nlohmann::json& j, const SyncConfig& c)
{
    j = nlohmann::json{ { "mode", c.mode },
                        { "integralFactor", c.integralFactor },
                        { "waterTankBuffer", c.waterTankBuffer },
                        { "syncInterval", c.syncInterval } };
}

inline void from_json(const nlohmann::json& j, SyncConfig& c)
{
    c.mode            = j.value("mode", SyncMode::Integral);
    c.integralFactor  = j.value("integralFactor", 0.1f);
    c.waterTankBuffer = j.value("waterTankBuffer", 0.05f);
    c.syncInterval    = j.value("syncInterval", 10.0);
}

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

    /// @brief 是否按物件轨道位置分配 HitEffect 左右声道音量。
    bool enableStereoHitEffects{ true };

    /// @brief 皮肤常驻音效的独立音量映射 (Key: 音效ID, Value: 0.0~1.0)
    std::map<std::string, float> permanentSfxVolumes;

    /// @brief 皮肤常驻音效的静音状态 (Key: 音效ID)
    std::map<std::string, bool> permanentSfxMutes;

    /// @brief 皮肤全局打击音效的播放速率是否跟随主音轨
    bool hitSfxSyncSpeed{ true };

    /// @brief 是否启用打击音效
    bool enableHitSfx{ true };
};

inline void to_json(nlohmann::json& j, const SfxConfig& c)
{
    j = nlohmann::json{
        { "polylineStrategy", c.polylineStrategy },
        { "enableFlickWidthVolumeScaling", c.enableFlickWidthVolumeScaling },
        { "flickWidthVolumeMultiplier", c.flickWidthVolumeMultiplier },
        { "enableStereoHitEffects", c.enableStereoHitEffects },
        { "permanentSfxVolumes", c.permanentSfxVolumes },
        { "permanentSfxMutes", c.permanentSfxMutes },
        { "hitSfxSyncSpeed", c.hitSfxSyncSpeed },
        { "enableHitSfx", c.enableHitSfx }
    };
}

inline void from_json(const nlohmann::json& j, SfxConfig& c)
{
    c.polylineStrategy =
        j.value("polylineStrategy", PolylineSfxStrategy::Exact);
    c.enableFlickWidthVolumeScaling =
        j.value("enableFlickWidthVolumeScaling", false);
    c.flickWidthVolumeMultiplier = j.value("flickWidthVolumeMultiplier", 0.1f);
    c.enableStereoHitEffects =
        j.value("enableStereoHitEffects",
                j.value("enableDirectionalFlickChannels", true));
    c.permanentSfxVolumes =
        j.value("permanentSfxVolumes", std::map<std::string, float>());
    c.permanentSfxMutes =
        j.value("permanentSfxMutes", std::map<std::string, bool>());
    c.hitSfxSyncSpeed = j.value("hitSfxSyncSpeed", true);
    c.enableHitSfx    = j.value("enableHitSfx", true);
}

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

struct SoftwareCursorConfig {
    /// @brief 软件光标主图尺寸 (px)
    float cursorSize{ 64.0f };
    /// @brief 拖尾基础尺寸 (px)
    float trailSize{ 48.0f };
    /// @brief 拖尾存活时间 (秒)
    float trailLifeTime{ 0.4f };
    /// @brief 烟雾初始尺寸 (px)
    float smokeSize{ 32.0f };
    /// @brief 烟雾存活时间 (秒)
    float smokeLifeTime{ 0.8f };
    /// @brief 是否根据当前谱面 BPM 自动适配烟雾存活时间 (1拍长度)
    bool enableBpmSyncSmokeLife{ false };
};

inline void to_json(nlohmann::json& j, const SoftwareCursorConfig& c)
{
    j = nlohmann::json{ { "cursorSize", c.cursorSize },
                        { "trailSize", c.trailSize },
                        { "trailLifeTime", c.trailLifeTime },
                        { "smokeSize", c.smokeSize },
                        { "smokeLifeTime", c.smokeLifeTime },
                        { "enableBpmSyncSmokeLife",
                          c.enableBpmSyncSmokeLife } };
}

inline void from_json(const nlohmann::json& j, SoftwareCursorConfig& c)
{
    c.cursorSize             = j.value("cursorSize", 64.0f);
    c.trailSize              = j.value("trailSize", 48.0f);
    c.trailLifeTime          = j.value("trailLifeTime", 0.4f);
    c.smokeSize              = j.value("smokeSize", 32.0f);
    c.smokeLifeTime          = j.value("smokeLifeTime", 0.8f);
    c.enableBpmSyncSmokeLife = j.value("enableBpmSyncSmokeLife", false);
}

struct UIAestheticsConfig {
    /// @brief UI 动画过渡时间下限，避免配置为 0 导致速度计算异常。
    static constexpr float MIN_ANIMATION_TRANSITION_DURATION = 0.02f;

    /// @brief 全局窗口圆角半径 (px, 基准值)
    float windowRounding{ 8.0f };
    /// @brief 全局组件圆角半径 (px, 基准值)
    float frameRounding{ 6.0f };
    /// @brief 窗口间的间隙/边距 (px, 基准值)
    float windowGap{ 8.0f };
    /// @brief 容器内部组件间距 (px, 基准值)
    float itemSpacing{ 8.0f };
    /// @brief 全局窗口内边距 (px, 基准值)
    float windowPadding{ 8.0f };
    /// @brief UI 悬浮、弹窗和面板切换的统一过渡时间，单位秒。
    float animationTransitionDuration{ 0.12f };

    /// @brief 获取统一 UI 过渡动画速度。
    /// @return 每秒推进的线性动画进度。
    float animationTransitionSpeed() const
    {
        return 1.0f / std::max(MIN_ANIMATION_TRANSITION_DURATION,
                               animationTransitionDuration);
    }
};

inline void to_json(nlohmann::json& j, const UIAestheticsConfig& c)
{
    j = nlohmann::json{ { "windowRounding", c.windowRounding },
                        { "frameRounding", c.frameRounding },
                        { "windowGap", c.windowGap },
                        { "itemSpacing", c.itemSpacing },
                        { "windowPadding", c.windowPadding },
                        { "animationTransitionDuration",
                          c.animationTransitionDuration } };
}

inline void from_json(const nlohmann::json& j, UIAestheticsConfig& c)
{
    c.windowRounding = j.value("windowRounding", 8.0f);
    c.frameRounding  = j.value("frameRounding", 6.0f);
    c.windowGap      = j.value("windowGap", 8.0f);
    c.itemSpacing    = j.value("itemSpacing", 8.0f);
    c.windowPadding  = j.value("windowPadding", 8.0f);
    c.animationTransitionDuration =
        std::max(UIAestheticsConfig::MIN_ANIMATION_TRANSITION_DURATION,
                 j.value("animationTransitionDuration", 0.12f));
}

/// @brief 音符调色盘方案中的颜色槽位数量。
inline constexpr std::size_t NOTE_COLOR_PALETTE_SLOT_COUNT = 6;

/// @brief 使用当前皮肤完整默认配色的调色盘方案标识。
inline constexpr const char* COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID =
    "__skin_default__";

/// @brief 用户保存的调色盘方案。
struct ColorPaletteScheme {
    /// @brief 方案显示名称。
    std::string name{ "Palette" };

    /// @brief 完整物件颜色，顺序与工具栏物件颜色槽位一致。
    std::array<std::array<float, 4>, NOTE_COLOR_PALETTE_SLOT_COUNT>
        noteColors{};

    /// @brief 完整分拍线颜色。
    BeatLineColorPalette beatLineColors{};
};

inline void to_json(nlohmann::json& j, const ColorPaletteScheme& c)
{
    j = nlohmann::json{ { "name", c.name },
                        { "noteColors", c.noteColors },
                        { "beatLineColors", c.beatLineColors } };
}

inline void from_json(const nlohmann::json& j, ColorPaletteScheme& c)
{
    c.name           = j.value("name", std::string("Palette"));
    c.noteColors     = j.value("noteColors", decltype(c.noteColors){});
    c.beatLineColors = j.value("beatLineColors", BeatLineColorPalette{});
}

/// @brief 调色盘持久化配置。
struct ColorPaletteConfig {
    /// @brief 当前选中的方案索引。
    std::size_t activeSchemeIndex{ 0 };

    /// @brief 用户保存的调色盘方案列表。
    std::vector<ColorPaletteScheme> schemes;
};

inline void to_json(nlohmann::json& j, const ColorPaletteConfig& c)
{
    j = nlohmann::json{ { "activeSchemeIndex", c.activeSchemeIndex },
                        { "schemes", c.schemes } };
}

inline void from_json(const nlohmann::json& j, ColorPaletteConfig& c)
{
    c.activeSchemeIndex = j.value("activeSchemeIndex", std::size_t{ 0 });
    c.schemes           = j.value("schemes", std::vector<ColorPaletteScheme>());
    if ( c.schemes.empty() ) {
        c.activeSchemeIndex = 0;
    } else if ( c.activeSchemeIndex >= c.schemes.size() ) {
        c.activeSchemeIndex = 0;
    }
}

/// @brief 单个自定义快捷键绑定。
struct ShortcutBinding {
    /// @brief 是否启用该快捷键；关闭时该动作没有键盘入口。
    bool enabled{ true };

    /// @brief 稳定按键名称，由 UI 层映射到 ImGuiKey。
    std::string key;

    /// @brief 是否要求 Ctrl 修饰键。
    bool ctrl{ false };

    /// @brief 是否要求 Shift 修饰键。
    bool shift{ false };

    /// @brief 是否要求 Alt 修饰键。
    bool alt{ false };

    /// @brief 是否要求 Super/Command 修饰键。
    bool super{ false };
};

inline void to_json(nlohmann::json& j, const ShortcutBinding& c)
{
    j = nlohmann::json{ { "enabled", c.enabled }, { "key", c.key },
                        { "ctrl", c.ctrl },       { "shift", c.shift },
                        { "alt", c.alt },         { "super", c.super } };
}

inline void from_json(const nlohmann::json& j, ShortcutBinding& c)
{
    c.enabled = j.value("enabled", true);
    c.key     = j.value("key", std::string());
    c.ctrl    = j.value("ctrl", false);
    c.shift   = j.value("shift", false);
    c.alt     = j.value("alt", false);
    c.super   = j.value("super", false);
    if ( c.key.empty() ) {
        c.enabled = false;
    }
}

/// @brief 编辑器可自定义快捷键配置。
struct ShortcutConfig {
    /// @brief 切换到移动工具。
    ShortcutBinding toolMove{ true, "1", false, false, false, false };

    /// @brief 切换到框选工具。
    ShortcutBinding toolMarquee{ true, "2", false, false, false, false };

    /// @brief 切换到绘制工具。
    ShortcutBinding toolDraw{ true, "3", false, false, false, false };

    /// @brief 切换到配色笔刷工具。
    ShortcutBinding toolColorBrush{ true, "4", false, false, false, false };

    /// @brief 切换到配色橡皮工具。
    ShortcutBinding toolColorEraser{ true, "5", false, false, false, false };

    /// @brief 镜像当前选中物件。
    ShortcutBinding mirror{ true, "M", true, false, false, false };

    /// @brief 镜像粘贴剪贴板物件。
    ShortcutBinding mirrorPaste{ true, "V", true, true, false, false };

    /// @brief 删除当前选中物件。
    ShortcutBinding deleteSelected{
        true, "Delete", false, false, false, false
    };

    /// @brief 切换反转滚动方向。
    ShortcutBinding toggleReverseScroll{
        false, "", false, false, false, false
    };

    /// @brief 切换滚动磁吸。
    ShortcutBinding toggleScrollSnap{ false, "", false, false, false, false };

    /// @brief 切换吸附向下取整。
    ShortcutBinding toggleSnapFloor{ false, "", false, false, false, false };

    /// @brief 切换 SCROLLTIMING 视觉映射。
    ShortcutBinding toggleScrollTimingMapping{ false, "",    false,
                                               false, false, false };

    /// @brief 切换分拍线显示。
    ShortcutBinding toggleBeatLines{ false, "", false, false, false, false };

    /// @brief 切换播放时滚动停止播放。
    ShortcutBinding toggleStopPlaybackOnScroll{ false, "",    false,
                                                false, false, false };

    /// @brief 切换打击音效。
    ShortcutBinding toggleHitSfx{ false, "", false, false, false, false };

    /// @brief 切换打击特效。
    ShortcutBinding toggleHitEffects{ false, "", false, false, false, false };

    /// @brief 切换同主音轨画布同步。
    ShortcutBinding toggleSyncSameMainAudio{ false, "",    false,
                                             false, false, false };
};

inline void to_json(nlohmann::json& j, const ShortcutConfig& c)
{
    j = nlohmann::json{
        { "toolMove", c.toolMove },
        { "toolMarquee", c.toolMarquee },
        { "toolDraw", c.toolDraw },
        { "toolColorBrush", c.toolColorBrush },
        { "toolColorEraser", c.toolColorEraser },
        { "mirror", c.mirror },
        { "mirrorPaste", c.mirrorPaste },
        { "deleteSelected", c.deleteSelected },
        { "toggleReverseScroll", c.toggleReverseScroll },
        { "toggleScrollSnap", c.toggleScrollSnap },
        { "toggleSnapFloor", c.toggleSnapFloor },
        { "toggleScrollTimingMapping", c.toggleScrollTimingMapping },
        { "toggleBeatLines", c.toggleBeatLines },
        { "toggleStopPlaybackOnScroll", c.toggleStopPlaybackOnScroll },
        { "toggleHitSfx", c.toggleHitSfx },
        { "toggleHitEffects", c.toggleHitEffects },
        { "toggleSyncSameMainAudio", c.toggleSyncSameMainAudio }
    };
}

inline void from_json(const nlohmann::json& j, ShortcutConfig& c)
{
    c.toolMove    = j.value("toolMove", ShortcutConfig().toolMove);
    c.toolMarquee = j.value("toolMarquee", ShortcutConfig().toolMarquee);
    c.toolDraw    = j.value("toolDraw", ShortcutConfig().toolDraw);
    c.toolColorBrush =
        j.value("toolColorBrush", ShortcutConfig().toolColorBrush);
    c.toolColorEraser =
        j.value("toolColorEraser", ShortcutConfig().toolColorEraser);
    c.mirror      = j.value("mirror", ShortcutConfig().mirror);
    c.mirrorPaste = j.value("mirrorPaste", ShortcutConfig().mirrorPaste);
    c.deleteSelected =
        j.value("deleteSelected", ShortcutConfig().deleteSelected);
    c.toggleReverseScroll =
        j.value("toggleReverseScroll", ShortcutConfig().toggleReverseScroll);
    c.toggleScrollSnap =
        j.value("toggleScrollSnap", ShortcutConfig().toggleScrollSnap);
    c.toggleSnapFloor =
        j.value("toggleSnapFloor", ShortcutConfig().toggleSnapFloor);
    c.toggleScrollTimingMapping =
        j.value("toggleScrollTimingMapping",
                ShortcutConfig().toggleScrollTimingMapping);
    c.toggleBeatLines =
        j.value("toggleBeatLines", ShortcutConfig().toggleBeatLines);
    c.toggleStopPlaybackOnScroll =
        j.value("toggleStopPlaybackOnScroll",
                ShortcutConfig().toggleStopPlaybackOnScroll);
    c.toggleHitSfx = j.value("toggleHitSfx", ShortcutConfig().toggleHitSfx);
    c.toggleHitEffects =
        j.value("toggleHitEffects", ShortcutConfig().toggleHitEffects);
    c.toggleSyncSameMainAudio = j.value(
        "toggleSyncSameMainAudio", ShortcutConfig().toggleSyncSameMainAudio);
}

enum class FrameLimitPreference {
    VSync,
    Refresh2x,
    Refresh4x,
    Refresh8x,
    Unlimited
};

NLOHMANN_JSON_SERIALIZE_ENUM(FrameLimitPreference,
                             { { FrameLimitPreference::VSync, "VSync" },
                               { FrameLimitPreference::Refresh2x, "Refresh2x" },
                               { FrameLimitPreference::Refresh4x, "Refresh4x" },
                               { FrameLimitPreference::Refresh8x, "Refresh8x" },
                               { FrameLimitPreference::Unlimited,
                                 "Unlimited" } })

/// @brief 音频播放后端偏好。
enum class AudioPlaybackBackend {
    SDL,    ///< SDL 音频后端。
    OpenAL  ///< OpenAL Soft 音频后端。
};

NLOHMANN_JSON_SERIALIZE_ENUM(AudioPlaybackBackend,
                             {
                                 { AudioPlaybackBackend::SDL, "SDL" },
                                 { AudioPlaybackBackend::OpenAL, "OpenAL" },
                             })

/// @brief OpenAL 空间化输出配置。
struct OpenALSpatialConfig {
    /// @brief 是否启用 OpenAL 空间化输出。
    bool enabled{ false };

    /// @brief 声源方向 X 分量。
    float directionX{ 0.0f };

    /// @brief 声源方向 Y 分量。
    float directionY{ 0.0f };

    /// @brief 声源方向 Z 分量。
    float directionZ{ -1.0f };

    /// @brief 声源距离。
    float distance{ 1.0f };

    /// @brief OpenAL 参考距离。
    float referenceDistance{ 1.0f };

    /// @brief OpenAL 最大距离。
    float maxDistance{ 100.0f };

    /// @brief OpenAL 距离衰减倍率。
    float rolloffFactor{ 1.0f };
};

inline void to_json(nlohmann::json& j, const OpenALSpatialConfig& c)
{
    j = nlohmann::json{ { "enabled", c.enabled },
                        { "directionX", c.directionX },
                        { "directionY", c.directionY },
                        { "directionZ", c.directionZ },
                        { "distance", c.distance },
                        { "referenceDistance", c.referenceDistance },
                        { "maxDistance", c.maxDistance },
                        { "rolloffFactor", c.rolloffFactor } };
}

inline void from_json(const nlohmann::json& j, OpenALSpatialConfig& c)
{
    c.enabled           = j.value("enabled", false);
    c.directionX        = j.value("directionX", 0.0f);
    c.directionY        = j.value("directionY", 0.0f);
    c.directionZ        = j.value("directionZ", -1.0f);
    c.distance          = j.value("distance", 1.0f);
    c.referenceDistance = j.value("referenceDistance", 1.0f);
    c.maxDistance       = j.value("maxDistance", 100.0f);
    c.rolloffFactor     = j.value("rolloffFactor", 1.0f);
}

/// @brief 自动主题选择使用的稳定 ID。
inline constexpr std::string_view UI_THEME_AUTO_ID = "Auto";

enum class SelectionMode {
    Strict,       ///< 严格模式 (必须完全包含)
    Intersection  ///< 相交模式 (只要相交即选中)
};

NLOHMANN_JSON_SERIALIZE_ENUM(SelectionMode,
                             {
                                 { SelectionMode::Strict, "Strict" },
                                 { SelectionMode::Intersection,
                                   "Intersection" },
                             })

enum class SaveFormatPreference {
    Original,  ///< 保持原始格式 (例如 .osu, .mc 等)
    ForceMMM   ///< 强制保存为 .mmm 格式 (内置 JSON 格式)
};

NLOHMANN_JSON_SERIALIZE_ENUM(SaveFormatPreference,
                             {
                                 { SaveFormatPreference::Original, "Original" },
                                 { SaveFormatPreference::ForceMMM, "ForceMMM" },
                             })

/// @brief 画布时间戳显示格式偏好
enum class TimeFormatPreference {
    Clock,         ///< 时:分:秒.毫秒
    Seconds,       ///< 秒，保留三位小数
    Milliseconds,  ///< 纯毫秒
    Beat           ///< 拍号 + 分拍位
};

NLOHMANN_JSON_SERIALIZE_ENUM(TimeFormatPreference,
                             {
                                 { TimeFormatPreference::Clock, "Clock" },
                                 { TimeFormatPreference::Seconds, "Seconds" },
                                 { TimeFormatPreference::Milliseconds,
                                   "Milliseconds" },
                                 { TimeFormatPreference::Beat, "Beat" },
                             })

/// @brief 复制粘贴时用于计算相对偏移的时间基准。
enum class CopyPasteTimeBasis {
    Timestamp,  ///< 按时间戳秒数保持相对偏移
    Beat        ///< 按 BPM 分拍位置保持相对偏移
};

NLOHMANN_JSON_SERIALIZE_ENUM(CopyPasteTimeBasis,
                             {
                                 { CopyPasteTimeBasis::Timestamp, "Timestamp" },
                                 { CopyPasteTimeBasis::Beat, "Beat" },
                             })

/// @brief 物件放置磁吸使用的分拍线来源。
enum class ObjectPlacementSnapMode {
    CurrentBeatDivisor,  ///< 仅使用当前分拍策略生成的分拍线。
    CommonBeatDivisors   ///< 使用用户选中的常用分拍线集合。
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ObjectPlacementSnapMode,
    {
        { ObjectPlacementSnapMode::CurrentBeatDivisor, "CurrentBeatDivisor" },
        { ObjectPlacementSnapMode::CommonBeatDivisors, "CommonBeatDivisors" },
    })

/// @brief 常用分拍选择允许的最小分母。
inline constexpr int COMMON_BEAT_DIVISOR_MIN = 2;

/// @brief 常用分拍选择允许的最大分母。
inline constexpr int COMMON_BEAT_DIVISOR_MAX = 24;

/// @brief 常用分拍选择的有效位数量。
inline constexpr int COMMON_BEAT_DIVISOR_COUNT =
    COMMON_BEAT_DIVISOR_MAX - COMMON_BEAT_DIVISOR_MIN + 1;

/// @brief 1/2 至 1/24 常用分拍线的全部有效位。
inline constexpr std::uint32_t COMMON_BEAT_DIVISOR_MASK_ALL =
    (std::uint32_t{ 1 } << COMMON_BEAT_DIVISOR_COUNT) - 1U;

/// @brief 默认启用的常用分拍集合。
inline constexpr std::uint32_t COMMON_BEAT_DIVISOR_MASK_DEFAULT =
    (std::uint32_t{ 1 } << (2 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (3 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (4 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (6 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (8 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (12 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (16 - COMMON_BEAT_DIVISOR_MIN)) |
    (std::uint32_t{ 1 } << (24 - COMMON_BEAT_DIVISOR_MIN));

/// @brief 判断常用分拍选择中是否启用了指定分母。
/// @param mask 常用分拍选择位掩码。
/// @param divisor 待查询的分母。
/// @return 分母处于 2 至 24 且对应选择位开启时返回 true。
inline constexpr bool isCommonBeatDivisorEnabled(std::uint32_t mask,
                                                 int           divisor)
{
    if ( divisor < COMMON_BEAT_DIVISOR_MIN ||
         divisor > COMMON_BEAT_DIVISOR_MAX ) {
        return false;
    }
    const auto bit = static_cast<unsigned>(divisor - COMMON_BEAT_DIVISOR_MIN);
    return (mask & (std::uint32_t{ 1 } << bit)) != 0U;
}

/// @brief 修改常用分拍选择中的指定分母状态。
/// @param mask 待修改的常用分拍选择位掩码。
/// @param divisor 待修改的分母。
/// @param enabled 是否启用该分母。
inline constexpr void setCommonBeatDivisorEnabled(std::uint32_t& mask,
                                                  int divisor, bool enabled)
{
    if ( divisor < COMMON_BEAT_DIVISOR_MIN ||
         divisor > COMMON_BEAT_DIVISOR_MAX ) {
        return;
    }
    const auto bit  = static_cast<unsigned>(divisor - COMMON_BEAT_DIVISOR_MIN);
    const auto flag = std::uint32_t{ 1 } << bit;
    if ( enabled ) {
        mask |= flag;
    } else {
        mask &= ~flag;
    }
}

/// @brief BPM 测量工具中不依赖具体项目或音轨的用户偏好。
struct BpmMeasurementToolPreferences {
    /// @brief 黄色拍框宽度，单位为毫秒。
    double markerWidthMs{ 80.0 };

    /// @brief 分拍线切分数量。
    int beatDivisor{ 4 };

    /// @brief 最近一次由用户调整的视图中心，单位为秒。
    double viewCenterSeconds{ 0.0 };

    /// @brief 分析视图在中心点单侧显示的时间跨度，单位为秒。
    double viewHalfWidthSeconds{ 8.0 };
};

/// @brief 将 BPM 测量工具用户偏好序列化为 JSON。
/// @param j 输出 JSON 对象。
/// @param c 待序列化的 BPM 测量工具偏好。
inline void to_json(nlohmann::json& j, const BpmMeasurementToolPreferences& c)
{
    j = nlohmann::json{ { "markerWidthMs", c.markerWidthMs },
                        { "beatDivisor", c.beatDivisor },
                        { "viewCenterSeconds", c.viewCenterSeconds },
                        { "viewHalfWidthSeconds", c.viewHalfWidthSeconds } };
}

/// @brief 从 JSON 恢复 BPM 测量工具用户偏好。
/// @param j 输入 JSON 对象。
/// @param c 接收结果的 BPM 测量工具偏好。
inline void from_json(const nlohmann::json& j, BpmMeasurementToolPreferences& c)
{
    c.markerWidthMs        = j.value("markerWidthMs", 80.0);
    c.beatDivisor          = j.value("beatDivisor", 4);
    c.viewCenterSeconds    = j.value("viewCenterSeconds", 0.0);
    c.viewHalfWidthSeconds = j.value("viewHalfWidthSeconds", 8.0);
}

/// @brief 编辑器行为与功能相关的配置
struct EditorSettings {
    /// @brief 渲染同步配置
    SyncConfig syncConfig;

    /// @brief 编辑器音效触发配置
    SfxConfig sfxConfig;

    /// @brief 文件选择器样式
    FilePickerStyle filePickerStyle{ FilePickerStyle::Native };

    /// @brief 光标样式
    CursorStyle cursorStyle{ CursorStyle::Software };

    /// @brief UI 主题稳定 ID；Auto 表示用户未手动指定，跟随系统与皮肤亮暗绑定。
    /// @details 旧版配置中的 Auto 即“跟随皮肤”，加载后继续视为未手动修改；
    /// 任何非 Auto 值均可引用内置或 Lua 插件主题实例。
    std::string theme{ UI_THEME_AUTO_ID };

    /// @brief 已禁用插件的配置根目录相对 ID。
    /// @details 当前主题插件使用 themes/<相对 Lua 路径>，移动配置根目录不会
    /// 改变开关状态；移动插件文件会被视为新插件。
    std::vector<std::string> disabledPluginIds;

    /// @brief 当前选择的皮肤目录名，位于 AppPaths::skinsRootPath() 下。
    std::string selectedSkinDirectory{ "mmm-default" };

    /// @brief 节拍切分/分拍数 (例如 4 代表四分音符)
    int beatDivisor{ 4 };

    /// @brief 重叠物件检测的时间窗口，单位毫秒。
    float overlapTimeWindowMs{ 5.0f };

    /// @brief 是否反转鼠标滚动方向
    bool reverseScroll{ false };

    /// @brief 是否开启滚动吸附
    bool scrollSnap{ false };

    /// @brief 是否开启物件放置磁吸。
    bool objectPlacementSnap{ false };

    /// @brief 物件放置磁吸使用当前分拍策略或常用分拍集合。
    ObjectPlacementSnapMode objectPlacementSnapMode{
        ObjectPlacementSnapMode::CurrentBeatDivisor
    };

    /// @brief 1/2 至 1/24 常用分拍线的选择位。
    std::uint32_t commonBeatDivisorMask{ COMMON_BEAT_DIVISOR_MASK_DEFAULT };

    /// @brief 最近打开项目的显示上限
    int recentProjectsLimit{ 10 };

    /// @brief 语言设置 (zh_cn, en_us)
    std::string language{ "zh_cn" };

    /// @brief 帧数限制模式偏好
    FrameLimitPreference frameLimit{ FrameLimitPreference::Refresh2x };

    /// @brief 音频播放后端偏好。
    AudioPlaybackBackend audioPlaybackBackend{ AudioPlaybackBackend::SDL };

    /// @brief SDL 音频后端的输出设备名称，空字符串表示默认设备。
    std::string sdlAudioOutputDeviceName;

    /// @brief OpenAL 音频后端的输出设备名称，空字符串表示默认设备。
    std::string openALAudioOutputDeviceName;

    /// @brief OpenAL 后端空间化输出配置。
    OpenALSpatialConfig openALSpatialConfig;

    /// @brief 是否每隔固定时间输出渲染阶段平均耗时日志
    bool renderProfileLogging{ false };

    /// @brief 是否允许退出时自动上传 PGO 性能热点原始数据。
    bool autoUploadPgoProfiles{ false };

    /// @brief 是否已经向用户询问过 PGO 性能数据上传授权。
    bool pgoProfileUploadConsentAsked{ false };

    /// @brief 界面字体大小倍率 (1.0 代表原始大小)
    float fontSizeMultiplier{ 1.15f };

    /// @brief 界面全局缩放倍率 (1.0 代表原始大小)
    float uiScaleMultiplier{ 1.0f };

    /// @brief 滚动操作时的步长加速倍率 (用于非 Snap 滚动、缩放等)
    float scrollSpeedMultiplier{ 4.0f };

    /// @brief 全局主音量 (0.0 ~ 1.0)
    float globalVolume{ 0.25f };

    /// @brief 全局静音
    bool globalMuted{ false };

    /// @brief BGM 全局增益 (0.0 ~ 2.0)
    float bgmGain{ 1.0f };

    /// @brief BGM 全局静音
    bool bgmGainMuted{ false };

    /// @brief SFX 全局增益 (0.0 ~ 2.0)
    float sfxGain{ 1.0f };

    /// @brief SFX 全局静音
    bool sfxGainMuted{ false };

    /// @brief 交互音效全局音量 (0.0 ~ 1.0)
    float interactionSfxGain{ 1.0f };

    /// @brief 交互音效全局静音
    bool interactionSfxGainMuted{ false };

    /// @brief 框选模式
    SelectionMode selectionMode{ SelectionMode::Intersection };

    /// @brief 框选边框粗细
    float marqueeThickness{ 2.0f };

    /// @brief 框选圆角半径
    float marqueeRounding{ 0.0f };

    /// @brief Ctrl+S 保存偏好
    SaveFormatPreference saveFormatPreference{ SaveFormatPreference::ForceMMM };

    /// @brief 导出 MC/打包 MCZ 时是否自动写入上架皮肤 mode_ext。
    bool autoAddStoreModeExtForMalodyExport{ false };

    /// @brief 画布时间戳显示格式偏好
    TimeFormatPreference timeFormatPreference{ TimeFormatPreference::Seconds };

    /// @brief 上次打开文件的路径 (用于文件对话框记忆)
    std::string lastFilePickerPath{ "." };

    /// @brief 软件光标配置 (仅在 cursorStyle 为 Software 时生效)
    SoftwareCursorConfig softwareCursorConfig;

    /// @brief 绘制物件(按住Shift)时是否屏蔽滚动加速
    bool disableScrollAccelerationWhileDrawing{ true };

    /// @brief 移除折线路径上的物件
    bool removeObjectsOnPolylinePath{ false };

    /// @brief 是否允许编辑 Flick、Polyline 及折线子物件。
    bool enablePolylineEditing{ true };

    /// @brief 粘贴后是否清空旧选择并选中新粘贴出的物件
    bool selectPastedObjects{ false };

    /// @brief 复制粘贴时按时间戳或分拍位置计算相对偏移。
    CopyPasteTimeBasis copyPasteTimeBasis{ CopyPasteTimeBasis::Timestamp };

    /// @brief 时间线窗口多选是否允许选中 BPM 红线
    bool timelineSelectionIncludesBpm{ false };

    /// @brief BPM 测量工具的全局用户偏好。
    BpmMeasurementToolPreferences bpmMeasurementToolPreferences;

    /// @brief 偏好的 ASCII 字体名称
    std::string preferredAsciiFont{ "Default" };

    /// @brief 偏好的 CJK 字体名称
    std::string preferredCjkFont{ "Default" };

    /// @brief 在播放时滚动滚轮则停止播放
    bool stopPlaybackOnScroll{ false };

    /// @brief 吸附向下取整 (总是吸附到早于鼠标位置的分拍线)
    bool snapFloor{ false };

    /// @brief 是否显示时间线窗口。
    bool showTimelineWindow{ true };

    /// @brief 时间线窗口是否启用专业分轨显示模式。
    bool timelineProfessionalMode{ false };

    /// @brief 是否显示预览窗口。
    bool showPreviewWindow{ true };

    /// @brief 是否在工具栏图标下方显示简短标签。
    bool showToolLabels{ false };

    /// @brief 是否将工具窗口固定在主窗口右侧。
    bool fixedToolWindow{ true };

    /// @brief 是否在左侧管理器图标下方显示简短标签。
    bool showManagerLabels{ true };

    /// @brief UI 审美/视觉表现配置
    UIAestheticsConfig aesthetics;

    /// @brief 调色盘方案配置。
    ColorPaletteConfig colorPalettes;

    /// @brief 打开项目时默认应用的调色盘方案名称。
    std::string defaultColorPaletteSchemeName{
        COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID
    };

    /// @brief 编辑器自定义快捷键配置。
    ShortcutConfig shortcutConfig;
};

inline void to_json(nlohmann::json& j, const EditorSettings& c)
{
    j = nlohmann::json{
        { "syncConfig", c.syncConfig },
        { "sfxConfig", c.sfxConfig },
        { "filePickerStyle", c.filePickerStyle },
        { "cursorStyle", c.cursorStyle },
        { "theme", c.theme },
        { "disabledPluginIds", c.disabledPluginIds },
        { "selectedSkinDirectory", c.selectedSkinDirectory },
        { "beatDivisor", c.beatDivisor },
        { "overlapTimeWindowMs", c.overlapTimeWindowMs },
        { "reverseScroll", c.reverseScroll },
        { "scrollSnap", c.scrollSnap },
        { "objectPlacementSnap", c.objectPlacementSnap },
        { "objectPlacementSnapMode", c.objectPlacementSnapMode },
        { "commonBeatDivisorMask", c.commonBeatDivisorMask },
        { "recentProjectsLimit", c.recentProjectsLimit },
        { "language", c.language },
        { "frameLimit", c.frameLimit },
        { "audioPlaybackBackend", c.audioPlaybackBackend },
        { "sdlAudioOutputDeviceName", c.sdlAudioOutputDeviceName },
        { "openALAudioOutputDeviceName", c.openALAudioOutputDeviceName },
        { "openALSpatialConfig", c.openALSpatialConfig },
        { "renderProfileLogging", c.renderProfileLogging },
        { "autoUploadPgoProfiles", c.autoUploadPgoProfiles },
        { "pgoProfileUploadConsentAsked", c.pgoProfileUploadConsentAsked },
        { "fontSizeMultiplier", c.fontSizeMultiplier },
        { "uiScaleMultiplier", c.uiScaleMultiplier },
        { "scrollSpeedMultiplier", c.scrollSpeedMultiplier },
        { "globalVolume", c.globalVolume },
        { "globalMuted", c.globalMuted },
        { "bgmGain", c.bgmGain },
        { "bgmGainMuted", c.bgmGainMuted },
        { "sfxGain", c.sfxGain },
        { "sfxGainMuted", c.sfxGainMuted },
        { "interactionSfxGain", c.interactionSfxGain },
        { "interactionSfxGainMuted", c.interactionSfxGainMuted },
        { "selectionMode", c.selectionMode },
        { "marqueeThickness", c.marqueeThickness },
        { "marqueeRounding", c.marqueeRounding },
        { "saveFormatPreference", c.saveFormatPreference },
        { "autoAddStoreModeExtForMalodyExport",
          c.autoAddStoreModeExtForMalodyExport },
        { "timeFormatPreference", c.timeFormatPreference },
        { "lastFilePickerPath", c.lastFilePickerPath },
        { "disableScrollAccelerationWhileDrawing",
          c.disableScrollAccelerationWhileDrawing },
        { "removeObjectsOnPolylinePath", c.removeObjectsOnPolylinePath },
        { "enablePolylineEditing", c.enablePolylineEditing },
        { "selectPastedObjects", c.selectPastedObjects },
        { "copyPasteTimeBasis", c.copyPasteTimeBasis },
        { "timelineSelectionIncludesBpm", c.timelineSelectionIncludesBpm },
        { "bpmMeasurementToolPreferences", c.bpmMeasurementToolPreferences },
        { "softwareCursorConfig", c.softwareCursorConfig },
        { "preferredAsciiFont", c.preferredAsciiFont },
        { "preferredCjkFont", c.preferredCjkFont },
        { "stopPlaybackOnScroll", c.stopPlaybackOnScroll },
        { "snapFloor", c.snapFloor },
        { "showTimelineWindow", c.showTimelineWindow },
        { "timelineProfessionalMode", c.timelineProfessionalMode },
        { "showPreviewWindow", c.showPreviewWindow },
        { "showToolLabels", c.showToolLabels },
        { "fixedToolWindow", c.fixedToolWindow },
        { "showManagerLabels", c.showManagerLabels },
        { "aesthetics", c.aesthetics },
        { "colorPalettes", c.colorPalettes },
        { "defaultColorPaletteSchemeName", c.defaultColorPaletteSchemeName },
        { "shortcutConfig", c.shortcutConfig }
    };
}

inline void from_json(const nlohmann::json& j, EditorSettings& c)
{
    c.syncConfig      = j.value("syncConfig", SyncConfig());
    c.sfxConfig       = j.value("sfxConfig", SfxConfig());
    c.filePickerStyle = j.value("filePickerStyle", FilePickerStyle::Native);
    c.cursorStyle     = j.value("cursorStyle", CursorStyle::Software);
    if ( auto themeIt = j.find("theme");
         themeIt != j.end() && themeIt->is_string() ) {
        c.theme = themeIt->get<std::string>();
        if ( c.theme.empty() ) {
            c.theme = UI_THEME_AUTO_ID;
        } else if ( c.theme == "MmmDefault" ) {
            c.theme = "Cecilia";
        }
    } else {
        c.theme = UI_THEME_AUTO_ID;
    }
    c.disabledPluginIds =
        j.value("disabledPluginIds", std::vector<std::string>());
    c.selectedSkinDirectory =
        j.value("selectedSkinDirectory", std::string("mmm-default"));
    c.beatDivisor             = j.value("beatDivisor", 4);
    c.overlapTimeWindowMs     = j.value("overlapTimeWindowMs", 5.0f);
    c.reverseScroll           = j.value("reverseScroll", false);
    c.scrollSnap              = j.value("scrollSnap", false);
    c.objectPlacementSnap     = j.value("objectPlacementSnap", c.scrollSnap);
    c.objectPlacementSnapMode = j.value(
        "objectPlacementSnapMode", ObjectPlacementSnapMode::CurrentBeatDivisor);
    c.commonBeatDivisorMask =
        j.value("commonBeatDivisorMask", COMMON_BEAT_DIVISOR_MASK_DEFAULT) &
        COMMON_BEAT_DIVISOR_MASK_ALL;
    c.recentProjectsLimit = j.value("recentProjectsLimit", 10);
    c.language            = j.value("language", std::string("zh_cn"));
    c.frameLimit =
        j.value("frameLimit",
                j.contains("vsync") ? (j.value("vsync", false)
                                           ? FrameLimitPreference::VSync
                                           : FrameLimitPreference::Unlimited)
                                    : FrameLimitPreference::Refresh2x);
    c.audioPlaybackBackend =
        j.value("audioPlaybackBackend", AudioPlaybackBackend::SDL);
    c.sdlAudioOutputDeviceName =
        j.value("sdlAudioOutputDeviceName", std::string());
    c.openALAudioOutputDeviceName =
        j.value("openALAudioOutputDeviceName", std::string());
    c.openALSpatialConfig =
        j.value("openALSpatialConfig", OpenALSpatialConfig());
    c.renderProfileLogging         = j.value("renderProfileLogging", false);
    c.autoUploadPgoProfiles        = j.value("autoUploadPgoProfiles", false);
    c.pgoProfileUploadConsentAsked = j.value(
        "pgoProfileUploadConsentAsked", j.contains("autoUploadPgoProfiles"));
    c.fontSizeMultiplier      = j.value("fontSizeMultiplier", 1.15f);
    c.uiScaleMultiplier       = j.value("uiScaleMultiplier", 1.0f);
    c.scrollSpeedMultiplier   = j.value("scrollSpeedMultiplier", 4.0f);
    c.globalVolume            = j.value("globalVolume", 0.25f);
    c.globalMuted             = j.value("globalMuted", false);
    c.bgmGain                 = j.value("bgmGain", 1.0f);
    c.bgmGainMuted            = j.value("bgmGainMuted", false);
    c.sfxGain                 = j.value("sfxGain", 1.0f);
    c.sfxGainMuted            = j.value("sfxGainMuted", false);
    c.interactionSfxGain      = j.value("interactionSfxGain", 1.0f);
    c.interactionSfxGainMuted = j.value("interactionSfxGainMuted", false);
    c.selectionMode    = j.value("selectionMode", SelectionMode::Intersection);
    c.marqueeThickness = j.value("marqueeThickness", 2.0f);
    c.marqueeRounding  = j.value("marqueeRounding", 0.0f);
    c.saveFormatPreference =
        j.value("saveFormatPreference", SaveFormatPreference::ForceMMM);
    c.autoAddStoreModeExtForMalodyExport =
        j.value("autoAddStoreModeExtForMalodyExport", false);
    c.timeFormatPreference =
        j.value("timeFormatPreference", TimeFormatPreference::Seconds);
    c.lastFilePickerPath = j.value("lastFilePickerPath", std::string("."));
    c.disableScrollAccelerationWhileDrawing =
        j.value("disableScrollAccelerationWhileDrawing", true);
    c.removeObjectsOnPolylinePath =
        j.value("removeObjectsOnPolylinePath", false);
    c.enablePolylineEditing = j.value("enablePolylineEditing", true);
    c.selectPastedObjects   = j.value("selectPastedObjects", false);
    c.copyPasteTimeBasis =
        j.value("copyPasteTimeBasis", CopyPasteTimeBasis::Timestamp);
    c.timelineSelectionIncludesBpm =
        j.value("timelineSelectionIncludesBpm", false);
    BpmMeasurementToolPreferences bpmMeasurementToolPreferencesFallback;
    bpmMeasurementToolPreferencesFallback.beatDivisor = c.beatDivisor;
    c.bpmMeasurementToolPreferences                   = j.value(
        "bpmMeasurementToolPreferences", bpmMeasurementToolPreferencesFallback);
    c.softwareCursorConfig =
        j.value("softwareCursorConfig", SoftwareCursorConfig());
    c.preferredAsciiFont =
        j.value("preferredAsciiFont", std::string("Default"));
    c.preferredCjkFont = j.value("preferredCjkFont", std::string("Default"));
    c.stopPlaybackOnScroll     = j.value("stopPlaybackOnScroll", false);
    c.snapFloor                = j.value("snapFloor", false);
    c.showTimelineWindow       = j.value("showTimelineWindow", true);
    c.timelineProfessionalMode = j.value("timelineProfessionalMode", false);
    c.showPreviewWindow        = j.value("showPreviewWindow", true);
    c.showToolLabels           = j.value("showToolLabels", false);
    c.fixedToolWindow          = j.value("fixedToolWindow", true);
    c.showManagerLabels        = j.value("showManagerLabels", true);
    c.aesthetics               = j.value("aesthetics", UIAestheticsConfig());
    c.colorPalettes            = j.value("colorPalettes", ColorPaletteConfig());
    c.defaultColorPaletteSchemeName =
        j.value("defaultColorPaletteSchemeName",
                std::string(COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID));
    c.shortcutConfig = j.value("shortcutConfig", ShortcutConfig());
}

}  // namespace MMM::Config
