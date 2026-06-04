#pragma once
#include <array>
#include <cstddef>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
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
};

inline void to_json(nlohmann::json& j, const UIAestheticsConfig& c)
{
    j = nlohmann::json{ { "windowRounding", c.windowRounding },
                        { "frameRounding", c.frameRounding },
                        { "windowGap", c.windowGap },
                        { "itemSpacing", c.itemSpacing },
                        { "windowPadding", c.windowPadding } };
}

inline void from_json(const nlohmann::json& j, UIAestheticsConfig& c)
{
    c.windowRounding = j.value("windowRounding", 8.0f);
    c.frameRounding  = j.value("frameRounding", 6.0f);
    c.windowGap      = j.value("windowGap", 8.0f);
    c.itemSpacing    = j.value("itemSpacing", 8.0f);
    c.windowPadding  = j.value("windowPadding", 8.0f);
}

/// @brief 音符调色盘方案中的颜色槽位数量。
inline constexpr std::size_t NOTE_COLOR_PALETTE_SLOT_COUNT = 6;

/// @brief 使用当前皮肤默认音符配色的调色盘方案标识。
inline constexpr const char* NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID =
    "__skin_default__";

/// @brief 用户保存的音符调色盘方案。
struct NoteColorPaletteScheme {
    /// @brief 方案显示名称。
    std::string name{ "Palette" };

    /// @brief 方案颜色，顺序与工具栏音符颜色槽位一致。
    std::vector<std::array<float, 4>> colors;
};

inline void to_json(nlohmann::json& j, const NoteColorPaletteScheme& c)
{
    j = nlohmann::json{ { "name", c.name }, { "colors", c.colors } };
}

inline void from_json(const nlohmann::json& j, NoteColorPaletteScheme& c)
{
    c.name   = j.value("name", std::string("Palette"));
    c.colors = j.value("colors", std::vector<std::array<float, 4>>());
    if ( c.colors.size() > NOTE_COLOR_PALETTE_SLOT_COUNT ) {
        c.colors.resize(NOTE_COLOR_PALETTE_SLOT_COUNT);
    }
}

/// @brief 音符调色盘持久化配置。
struct NoteColorPaletteConfig {
    /// @brief 当前选中的方案索引。
    std::size_t activeSchemeIndex{ 0 };

    /// @brief 用户保存的调色盘方案列表。
    std::vector<NoteColorPaletteScheme> schemes;
};

inline void to_json(nlohmann::json& j, const NoteColorPaletteConfig& c)
{
    j = nlohmann::json{ { "activeSchemeIndex", c.activeSchemeIndex },
                        { "schemes", c.schemes } };
}

inline void from_json(const nlohmann::json& j, NoteColorPaletteConfig& c)
{
    c.activeSchemeIndex = j.value("activeSchemeIndex", std::size_t{ 0 });
    c.schemes = j.value("schemes", std::vector<NoteColorPaletteScheme>());
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

enum class UITheme {
    Auto,
    DeepDark,
    Dark,
    Light,
    Classic,
    Microsoft,
    Darcula,
    Photoshop,
    Unreal,
    Gold,
    RoundedVisualStudio,
    SonicRiders,
    DarkRuda,
    SoftCherry,
    Enemymouse,
    DiscordDark,
    Comfy,
    PurpleComfy,
    FutureDark,
    CleanDark,
    Moonlight,
    MmmDefault,  ///< 基于 mmm-default 塞西莉娅配色的 Moonlight 派生主题。
    ComfortableLight,
    HazyDark,
    Everforest,
    Windark,
    Rest,
    ComfortableDarkCyan,
    KazamCherry,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    UITheme, {
                 { UITheme::Auto, "Auto" },
                 { UITheme::DeepDark, "DeepDark" },
                 { UITheme::Dark, "Dark" },
                 { UITheme::Light, "Light" },
                 { UITheme::Classic, "Classic" },
                 { UITheme::Microsoft, "Microsoft" },
                 { UITheme::Darcula, "Darcula" },
                 { UITheme::Photoshop, "Photoshop" },
                 { UITheme::Unreal, "Unreal" },
                 { UITheme::Gold, "Gold" },
                 { UITheme::RoundedVisualStudio, "RoundedVisualStudio" },
                 { UITheme::SonicRiders, "SonicRiders" },
                 { UITheme::DarkRuda, "DarkRuda" },
                 { UITheme::SoftCherry, "SoftCherry" },
                 { UITheme::Enemymouse, "Enemymouse" },
                 { UITheme::DiscordDark, "DiscordDark" },
                 { UITheme::Comfy, "Comfy" },
                 { UITheme::PurpleComfy, "PurpleComfy" },
                 { UITheme::FutureDark, "FutureDark" },
                 { UITheme::CleanDark, "CleanDark" },
                 { UITheme::Moonlight, "Moonlight" },
                 { UITheme::MmmDefault, "MmmDefault" },
                 { UITheme::ComfortableLight, "ComfortableLight" },
                 { UITheme::HazyDark, "HazyDark" },
                 { UITheme::Everforest, "Everforest" },
                 { UITheme::Windark, "Windark" },
                 { UITheme::Rest, "Rest" },
                 { UITheme::ComfortableDarkCyan, "ComfortableDarkCyan" },
                 { UITheme::KazamCherry, "KazamCherry" },
             })

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

    /// @brief UI 主题样式
    UITheme theme{ UITheme::Auto };

    /// @brief 节拍切分/分拍数 (例如 4 代表四分音符)
    int beatDivisor{ 4 };

    /// @brief 重叠物件检测的时间窗口，单位毫秒。
    float overlapTimeWindowMs{ 5.0f };

    /// @brief 是否反转鼠标滚动方向
    bool reverseScroll{ false };

    /// @brief 是否开启滚动吸附
    bool scrollSnap{ false };

    /// @brief 最近打开项目的显示上限
    int recentProjectsLimit{ 10 };

    /// @brief 语言设置 (zh_cn, en_us)
    std::string language{ "zh_cn" };

    /// @brief 帧数限制模式偏好
    FrameLimitPreference frameLimit{ FrameLimitPreference::Refresh2x };

    /// @brief 音频播放后端偏好。
    AudioPlaybackBackend audioPlaybackBackend{ AudioPlaybackBackend::SDL };

    /// @brief OpenAL 后端空间化输出配置。
    OpenALSpatialConfig openALSpatialConfig;

    /// @brief 是否每隔固定时间输出渲染阶段平均耗时日志
    bool renderProfileLogging{ false };

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

    /// @brief 框选模式
    SelectionMode selectionMode{ SelectionMode::Intersection };

    /// @brief 框选边框粗细
    float marqueeThickness{ 2.0f };

    /// @brief 框选圆角半径
    float marqueeRounding{ 0.0f };

    /// @brief Ctrl+S 保存偏好
    SaveFormatPreference saveFormatPreference{ SaveFormatPreference::ForceMMM };

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

    /// @brief 粘贴后是否清空旧选择并选中新粘贴出的物件
    bool selectPastedObjects{ false };

    /// @brief 偏好的 ASCII 字体名称
    std::string preferredAsciiFont{ "Default" };

    /// @brief 偏好的 CJK 字体名称
    std::string preferredCjkFont{ "Default" };

    /// @brief 在播放时滚动滚轮则停止播放
    bool stopPlaybackOnScroll{ false };

    /// @brief 吸附向下取整 (总是吸附到早于鼠标位置的分拍线)
    bool snapFloor{ false };

    /// @brief UI 审美/视觉表现配置
    UIAestheticsConfig aesthetics;

    /// @brief 音符调色盘方案配置。
    NoteColorPaletteConfig noteColorPalettes;

    /// @brief 打开项目时默认应用的音符调色盘方案名称。
    std::string defaultNoteColorPaletteSchemeName{
        NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID
    };

    /// @brief 编辑器自定义快捷键配置。
    ShortcutConfig shortcutConfig;
};

inline void to_json(nlohmann::json& j, const EditorSettings& c)
{
    j = nlohmann::json{ { "syncConfig", c.syncConfig },
                        { "sfxConfig", c.sfxConfig },
                        { "filePickerStyle", c.filePickerStyle },
                        { "cursorStyle", c.cursorStyle },
                        { "theme", c.theme },
                        { "beatDivisor", c.beatDivisor },
                        { "overlapTimeWindowMs", c.overlapTimeWindowMs },
                        { "reverseScroll", c.reverseScroll },
                        { "scrollSnap", c.scrollSnap },
                        { "recentProjectsLimit", c.recentProjectsLimit },
                        { "language", c.language },
                        { "frameLimit", c.frameLimit },
                        { "audioPlaybackBackend", c.audioPlaybackBackend },
                        { "openALSpatialConfig", c.openALSpatialConfig },
                        { "renderProfileLogging", c.renderProfileLogging },
                        { "fontSizeMultiplier", c.fontSizeMultiplier },
                        { "uiScaleMultiplier", c.uiScaleMultiplier },
                        { "scrollSpeedMultiplier", c.scrollSpeedMultiplier },
                        { "globalVolume", c.globalVolume },
                        { "globalMuted", c.globalMuted },
                        { "bgmGain", c.bgmGain },
                        { "bgmGainMuted", c.bgmGainMuted },
                        { "sfxGain", c.sfxGain },
                        { "sfxGainMuted", c.sfxGainMuted },
                        { "selectionMode", c.selectionMode },
                        { "marqueeThickness", c.marqueeThickness },
                        { "marqueeRounding", c.marqueeRounding },
                        { "saveFormatPreference", c.saveFormatPreference },
                        { "timeFormatPreference", c.timeFormatPreference },
                        { "lastFilePickerPath", c.lastFilePickerPath },
                        { "disableScrollAccelerationWhileDrawing",
                          c.disableScrollAccelerationWhileDrawing },
                        { "removeObjectsOnPolylinePath",
                          c.removeObjectsOnPolylinePath },
                        { "selectPastedObjects", c.selectPastedObjects },
                        { "softwareCursorConfig", c.softwareCursorConfig },
                        { "preferredAsciiFont", c.preferredAsciiFont },
                        { "preferredCjkFont", c.preferredCjkFont },
                        { "stopPlaybackOnScroll", c.stopPlaybackOnScroll },
                        { "snapFloor", c.snapFloor },
                        { "aesthetics", c.aesthetics },
                        { "noteColorPalettes", c.noteColorPalettes },
                        { "defaultNoteColorPaletteSchemeName",
                          c.defaultNoteColorPaletteSchemeName },
                        { "shortcutConfig", c.shortcutConfig } };
}

inline void from_json(const nlohmann::json& j, EditorSettings& c)
{
    c.syncConfig          = j.value("syncConfig", SyncConfig());
    c.sfxConfig           = j.value("sfxConfig", SfxConfig());
    c.filePickerStyle     = j.value("filePickerStyle", FilePickerStyle::Native);
    c.cursorStyle         = j.value("cursorStyle", CursorStyle::Software);
    c.theme               = j.value("theme", UITheme::Auto);
    c.beatDivisor         = j.value("beatDivisor", 4);
    c.overlapTimeWindowMs = j.value("overlapTimeWindowMs", 5.0f);
    c.reverseScroll       = j.value("reverseScroll", false);
    c.scrollSnap          = j.value("scrollSnap", false);
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
    c.openALSpatialConfig =
        j.value("openALSpatialConfig", OpenALSpatialConfig());
    c.renderProfileLogging  = j.value("renderProfileLogging", false);
    c.fontSizeMultiplier    = j.value("fontSizeMultiplier", 1.15f);
    c.uiScaleMultiplier     = j.value("uiScaleMultiplier", 1.0f);
    c.scrollSpeedMultiplier = j.value("scrollSpeedMultiplier", 4.0f);
    c.globalVolume          = j.value("globalVolume", 0.25f);
    c.globalMuted           = j.value("globalMuted", false);
    c.bgmGain               = j.value("bgmGain", 1.0f);
    c.bgmGainMuted          = j.value("bgmGainMuted", false);
    c.sfxGain               = j.value("sfxGain", 1.0f);
    c.sfxGainMuted          = j.value("sfxGainMuted", false);
    c.selectionMode    = j.value("selectionMode", SelectionMode::Intersection);
    c.marqueeThickness = j.value("marqueeThickness", 2.0f);
    c.marqueeRounding  = j.value("marqueeRounding", 0.0f);
    c.saveFormatPreference =
        j.value("saveFormatPreference", SaveFormatPreference::ForceMMM);
    c.timeFormatPreference =
        j.value("timeFormatPreference", TimeFormatPreference::Seconds);
    c.lastFilePickerPath = j.value("lastFilePickerPath", std::string("."));
    c.disableScrollAccelerationWhileDrawing =
        j.value("disableScrollAccelerationWhileDrawing", true);
    c.removeObjectsOnPolylinePath =
        j.value("removeObjectsOnPolylinePath", false);
    c.selectPastedObjects = j.value("selectPastedObjects", false);
    c.softwareCursorConfig =
        j.value("softwareCursorConfig", SoftwareCursorConfig());
    c.preferredAsciiFont =
        j.value("preferredAsciiFont", std::string("Default"));
    c.preferredCjkFont = j.value("preferredCjkFont", std::string("Default"));
    c.stopPlaybackOnScroll = j.value("stopPlaybackOnScroll", false);
    c.snapFloor            = j.value("snapFloor", false);
    c.aesthetics           = j.value("aesthetics", UIAestheticsConfig());
    c.noteColorPalettes =
        j.value("noteColorPalettes", NoteColorPaletteConfig());
    c.defaultNoteColorPaletteSchemeName =
        j.value("defaultNoteColorPaletteSchemeName",
                std::string(NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID));
    c.shortcutConfig = j.value("shortcutConfig", ShortcutConfig());
}

}  // namespace MMM::Config
