#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <string>

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

    /// @brief 皮肤常驻音效的独立音量映射 (Key: 音效ID, Value: 0.0~1.0)
    std::map<std::string, float> permanentSfxVolumes;

    /// @brief 皮肤常驻音效的静音状态 (Key: 音效ID)
    std::map<std::string, bool> permanentSfxMutes;

    /// @brief 皮肤全局打击音效的播放速率是否跟随主音轨
    bool hitSfxSyncSpeed{ true };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SfxConfig, polylineStrategy,
                                   enableFlickWidthVolumeScaling,
                                   flickWidthVolumeMultiplier,
                                   permanentSfxVolumes, permanentSfxMutes,
                                   hitSfxSyncSpeed)
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SoftwareCursorConfig, cursorSize, trailSize,
                                   trailLifeTime, smokeSize, smokeLifeTime,
                                   enableBpmSyncSmokeLife)
};

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

    /// @brief UI 主题样式
    UITheme theme{ UITheme::Auto };

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

    /// @brief 是否开启垂直同步
    bool vsync{ false };

    /// @brief 界面字体大小倍率 (1.0 代表原始大小)
    float fontSizeMultiplier{ 1.0f };

    /// @brief 界面全局缩放倍率 (1.0 代表原始大小)
    float uiScaleMultiplier{ 1.0f };

    /// @brief 滚动操作时的步长加速倍率 (用于非 Snap 滚动、缩放等)
    float scrollSpeedMultiplier{ 4.0f };

    /// @brief 全局主音量 (0.0 ~ 1.0)
    float globalVolume{ 0.25f };

    /// @brief 框选模式
    SelectionMode selectionMode{ SelectionMode::Intersection };

    /// @brief 框选边框粗细
    float marqueeThickness{ 2.0f };

    /// @brief 框选圆角半径
    float marqueeRounding{ 0.0f };

    /// @brief Ctrl+S 保存偏好
    SaveFormatPreference saveFormatPreference{ SaveFormatPreference::Original };

    /// @brief 上次打开文件的路径 (用于文件对话框记忆)
    std::string lastFilePickerPath{ "." };

    /// @brief 软件光标配置 (仅在 cursorStyle 为 Software 时生效)
    SoftwareCursorConfig softwareCursorConfig;

    /// @brief 绘制物件(按住Shift)时是否屏蔽滚动加速
    bool disableScrollAccelerationWhileDrawing{ true };

    /// @brief 偏好的 ASCII 字体名称
    std::string preferredAsciiFont{ "Default" };

    /// @brief 偏好的 CJK 字体名称
    std::string preferredCjkFont{ "Default" };

    /// @brief 在播放时滚动滚轮则停止播放
    bool stopPlaybackOnScroll{ false };

    /// @brief 吸附向下取整 (总是吸附到早于鼠标位置的分拍线)
    bool snapFloor{ false };
};

inline void to_json(nlohmann::json& j, const EditorSettings& c)
{
    j = nlohmann::json{ { "syncConfig", c.syncConfig },
                        { "sfxConfig", c.sfxConfig },
                        { "filePickerStyle", c.filePickerStyle },
                        { "cursorStyle", c.cursorStyle },
                        { "theme", c.theme },
                        { "beatDivisor", c.beatDivisor },
                        { "reverseScroll", c.reverseScroll },
                        { "scrollSnap", c.scrollSnap },
                        { "recentProjectsLimit", c.recentProjectsLimit },
                        { "language", c.language },
                        { "vsync", c.vsync },
                        { "fontSizeMultiplier", c.fontSizeMultiplier },
                        { "uiScaleMultiplier", c.uiScaleMultiplier },
                        { "scrollSpeedMultiplier", c.scrollSpeedMultiplier },
                        { "globalVolume", c.globalVolume },
                        { "selectionMode", c.selectionMode },
                        { "marqueeThickness", c.marqueeThickness },
                        { "marqueeRounding", c.marqueeRounding },
                        { "saveFormatPreference", c.saveFormatPreference },
                        { "lastFilePickerPath", c.lastFilePickerPath },
                        { "disableScrollAccelerationWhileDrawing",
                          c.disableScrollAccelerationWhileDrawing },
                        { "softwareCursorConfig", c.softwareCursorConfig },
                        { "preferredAsciiFont", c.preferredAsciiFont },
                        { "preferredCjkFont", c.preferredCjkFont },
                        { "stopPlaybackOnScroll", c.stopPlaybackOnScroll },
                        { "snapFloor", c.snapFloor } };
}

inline void from_json(const nlohmann::json& j, EditorSettings& c)
{
    c.syncConfig      = j.value("syncConfig", SyncConfig());
    c.sfxConfig       = j.value("sfxConfig", SfxConfig());
    c.filePickerStyle = j.value("filePickerStyle", FilePickerStyle::Unified);
    c.cursorStyle     = j.value("cursorStyle", CursorStyle::Software);
    c.theme           = j.value("theme", UITheme::Auto);
    c.beatDivisor     = j.value("beatDivisor", 4);
    c.reverseScroll   = j.value("reverseScroll", false);
    c.scrollSnap      = j.value("scrollSnap", false);
    c.recentProjectsLimit   = j.value("recentProjectsLimit", 10);
    c.language              = j.value("language", std::string("zh_cn"));
    c.vsync                 = j.value("vsync", false);
    c.fontSizeMultiplier    = j.value("fontSizeMultiplier", 1.0f);
    c.uiScaleMultiplier     = j.value("uiScaleMultiplier", 1.0f);
    c.scrollSpeedMultiplier = j.value("scrollSpeedMultiplier", 4.0f);
    c.globalVolume          = j.value("globalVolume", 0.25f);
    c.selectionMode    = j.value("selectionMode", SelectionMode::Intersection);
    c.marqueeThickness = j.value("marqueeThickness", 2.0f);
    c.marqueeRounding  = j.value("marqueeRounding", 0.0f);
    c.saveFormatPreference =
        j.value("saveFormatPreference", SaveFormatPreference::Original);
    c.lastFilePickerPath = j.value("lastFilePickerPath", std::string("."));
    c.disableScrollAccelerationWhileDrawing =
        j.value("disableScrollAccelerationWhileDrawing", true);
    c.softwareCursorConfig =
        j.value("softwareCursorConfig", SoftwareCursorConfig());
    c.preferredAsciiFont =
        j.value("preferredAsciiFont", std::string("Default"));
    c.preferredCjkFont = j.value("preferredCjkFont", std::string("Default"));
    c.stopPlaybackOnScroll = j.value("stopPlaybackOnScroll", false);
    c.snapFloor = j.value("snapFloor", false);
}

}  // namespace MMM::Config
