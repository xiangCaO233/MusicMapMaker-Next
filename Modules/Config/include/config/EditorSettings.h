#pragma once
#include "config/AudioPlaybackConfig.h"
#include "config/BeatLinePalette.h"
#include "config/FrameLimitPreference.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
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

/// @brief 将同步模式序列化为稳定文本。
void to_json(nlohmann::json& json, const SyncMode& mode);
/// @brief 从稳定文本读取同步模式。
void from_json(const nlohmann::json& json, SyncMode& mode);

struct SyncConfig {
    SyncMode mode{ SyncMode::Integral };
    float    integralFactor{ 0.1f };    ///< 积分追踪系数 (0.0~1.0)
    float    waterTankBuffer{ 0.05f };  ///< 水箱缓冲时间 (秒)
    double   syncInterval{ 10.0 };      ///< 强制同步周期 (秒)
};

/// @brief 将同步配置序列化为 JSON。
void to_json(nlohmann::json& json, const SyncConfig& config);
/// @brief 从 JSON 读取同步配置。
void from_json(const nlohmann::json& json, SyncConfig& config);

enum class PolylineSfxStrategy {
    Exact,             ///< 策略一: 所有子物件精确按照他们的类型播放对应音效
    InternalAsNormal,  ///< 策略二: 仅"内部"子物件播放普通Note音效
    OnlyTailExact,     ///< 策略三: 仅尾部子物件按类型播放
    AllAsNormal        ///< 策略四: 全部子物件均播放普通Note音效
};

/// @brief 将折线音效策略序列化为稳定文本。
void to_json(nlohmann::json& json, const PolylineSfxStrategy& strategy);
/// @brief 从稳定文本读取折线音效策略。
void from_json(const nlohmann::json& json, PolylineSfxStrategy& strategy);

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

    /// @brief 是否播放未绑定音效文件的物件所使用的默认打击音效。
    bool enableUnboundHitSfx{ true };

    /// @brief 未绑定音效文件的默认打击音效线性增益，范围为 0.0~2.0。
    float unboundHitSfxGain{ 1.0F };

    /// @brief 是否播放已经绑定音效文件的物件打击音效。
    bool enableBoundHitSfx{ true };

    /// @brief 已绑定音效文件的物件打击音效线性增益，范围为 0.0~2.0。
    float boundHitSfxGain{ 1.0F };
};

/// @brief 将打击音效线性增益规范到持久化与实时混音的共同范围。
/// @param gain 待规范的线性增益。
/// @return 0.0~2.0 的有限值；非有限输入按静音增益处理。
[[nodiscard]] float sanitizeHitSfxGain(float gain) noexcept;

/// @brief 将音效配置序列化为 JSON。
void to_json(nlohmann::json& json, const SfxConfig& config);
/// @brief 从 JSON 读取音效配置。
void from_json(const nlohmann::json& json, SfxConfig& config);

enum class FilePickerStyle {
    Native,  ///< 系统原生对话框 (nfd-extended)
    Unified  ///< 统一风格对话框 (ImGuiFileDialog)
};

/// @brief 将文件选择器样式序列化为稳定文本。
void to_json(nlohmann::json& json, const FilePickerStyle& style);
/// @brief 从稳定文本读取文件选择器样式。
void from_json(const nlohmann::json& json, FilePickerStyle& style);

enum class CursorStyle {
    Software,  ///< 软件内置光标 (CursorManager)
    System     ///< 系统原生光标
};

/// @brief 将光标样式序列化为稳定文本。
void to_json(nlohmann::json& json, const CursorStyle& style);
/// @brief 从稳定文本读取光标样式。
void from_json(const nlohmann::json& json, CursorStyle& style);

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

/// @brief 将软件光标配置序列化为 JSON。
void to_json(nlohmann::json& json, const SoftwareCursorConfig& config);
/// @brief 从 JSON 读取软件光标配置。
void from_json(const nlohmann::json& json, SoftwareCursorConfig& config);

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

/// @brief 将 UI 审美配置序列化为 JSON。
void to_json(nlohmann::json& json, const UIAestheticsConfig& config);
/// @brief 从 JSON 读取 UI 审美配置。
void from_json(const nlohmann::json& json, UIAestheticsConfig& config);

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

/// @brief 将调色盘方案序列化为 JSON。
void to_json(nlohmann::json& json, const ColorPaletteScheme& scheme);
/// @brief 从 JSON 读取调色盘方案。
void from_json(const nlohmann::json& json, ColorPaletteScheme& scheme);

/// @brief 调色盘持久化配置。
struct ColorPaletteConfig {
    /// @brief 当前选中的方案索引。
    std::size_t activeSchemeIndex{ 0 };

    /// @brief 用户保存的调色盘方案列表。
    std::vector<ColorPaletteScheme> schemes;
};

/// @brief 将调色盘配置序列化为 JSON。
void to_json(nlohmann::json& json, const ColorPaletteConfig& config);
/// @brief 从 JSON 读取调色盘配置。
void from_json(const nlohmann::json& json, ColorPaletteConfig& config);

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

/// @brief 判断两个已启用快捷键绑定是否使用同一按键组合。
/// @param lhs 第一个快捷键绑定。
/// @param rhs 第二个快捷键绑定。
/// @return 两者均有效且按键及全部修饰键相同时返回 true。
/// @warning 设置页热路径：仅比较两个短字符串和四个修饰键标志。
[[nodiscard]] constexpr bool shortcutBindingsConflict(
    const ShortcutBinding& lhs, const ShortcutBinding& rhs)
{
    return lhs.enabled && rhs.enabled && !lhs.key.empty() && !rhs.key.empty() &&
           lhs.key == rhs.key && lhs.ctrl == rhs.ctrl &&
           lhs.shift == rhs.shift && lhs.alt == rhs.alt &&
           lhs.super == rhs.super;
}

/// @brief 将快捷键绑定序列化为 JSON。
void to_json(nlohmann::json& json, const ShortcutBinding& binding);
/// @brief 从 JSON 读取快捷键绑定。
void from_json(const nlohmann::json& json, ShortcutBinding& binding);

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

    /// @brief 打开选中物件批量音量编辑器。
    ShortcutBinding editSelectedVolume{ false, "", false, false, false, false };

    /// @brief 删除当前选中物件。
    ShortcutBinding deleteSelected{
        true, "Delete", false, false, false, false
    };

    /// @brief 切换谱面播放与暂停状态。
    ShortcutBinding togglePlayback{ true, "Space", false, false, false, false };

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

/// @brief 将快捷键配置序列化为 JSON。
void to_json(nlohmann::json& json, const ShortcutConfig& config);
/// @brief 从 JSON 读取快捷键配置。
void from_json(const nlohmann::json& json, ShortcutConfig& config);

/// @brief 自动主题选择使用的稳定 ID。
inline constexpr std::string_view UI_THEME_AUTO_ID = "Auto";

enum class SelectionMode {
    Strict,       ///< 严格模式 (必须完全包含)
    Intersection  ///< 相交模式 (只要相交即选中)
};

/// @brief 将框选模式序列化为稳定文本。
void to_json(nlohmann::json& json, const SelectionMode& mode);
/// @brief 从稳定文本读取框选模式。
void from_json(const nlohmann::json& json, SelectionMode& mode);

enum class SaveFormatPreference {
    Original,  ///< 保持原始格式 (例如 .osu, .mc 等)
    ForceMMM   ///< 强制保存为 .mmm 格式 (内置 JSON 格式)
};

/// @brief 将保存格式偏好序列化为稳定文本。
void to_json(nlohmann::json& json, const SaveFormatPreference& preference);
/// @brief 从稳定文本读取保存格式偏好。
void from_json(const nlohmann::json& json, SaveFormatPreference& preference);

/// @brief 自动保存调度模式。
enum class AutoSaveMode {
    Disabled,       ///< 不启用通用自动保存。
    Timed,          ///< 按固定时间间隔保存。
    EventTriggered  ///< 在启用的编辑器事件发生后保存。
};

/// @brief 将自动保存模式序列化为稳定文本。
void to_json(nlohmann::json& json, const AutoSaveMode& mode);
/// @brief 从稳定文本读取自动保存模式。
void from_json(const nlohmann::json& json, AutoSaveMode& mode);

/// @brief 自动保存定时间隔单位。
enum class AutoSaveIntervalUnit {
    Seconds,  ///< 秒。
    Minutes   ///< 分钟。
};

/// @brief 将自动保存间隔单位序列化为稳定文本。
void to_json(nlohmann::json& json, const AutoSaveIntervalUnit& unit);
/// @brief 从稳定文本读取自动保存间隔单位。
void from_json(const nlohmann::json& json, AutoSaveIntervalUnit& unit);

/// @brief 软件全局自动保存配置。
struct AutoSaveConfig {
    /// @brief 自动保存调度模式。
    AutoSaveMode mode{ AutoSaveMode::Disabled };

    /// @brief 定时间隔单位。
    AutoSaveIntervalUnit intervalUnit{ AutoSaveIntervalUnit::Seconds };

    /// @brief 定时间隔数值；读取配置时限制为 5~60。
    int intervalValue{ 30 };

    /// @brief 任意谱面物件修改提交后是否触发自动保存。
    bool onObjectModified{ true };

    /// @brief 切换活动谱面时是否触发自动保存。
    bool onBeatmapSwitch{ true };

    /// @brief ImGui 根窗口丢失焦点时是否触发自动保存。
    bool onImGuiWindowFocusLost{ true };

    /// @brief 程序原生窗口丢失焦点或最小化时是否触发自动保存。
    bool onNativeWindowFocusLost{ true };

    /// @brief 将配置的 5~60 定时间隔换算为秒。
    /// @return 可供逻辑调度器使用的秒数。
    [[nodiscard]] double intervalSeconds() const
    {
        const int safeValue = std::clamp(intervalValue, 5, 60);
        return intervalUnit == AutoSaveIntervalUnit::Minutes
                   ? static_cast<double>(safeValue) * 60.0
                   : static_cast<double>(safeValue);
    }
};

/// @brief 将自动保存配置序列化为 JSON。
void to_json(nlohmann::json& json, const AutoSaveConfig& config);
/// @brief 从 JSON 读取自动保存配置并约束定时间隔。
void from_json(const nlohmann::json& json, AutoSaveConfig& config);

/// @brief 画布时间戳显示格式偏好
enum class TimeFormatPreference {
    Clock,         ///< 时:分:秒.毫秒
    Seconds,       ///< 秒，保留三位小数
    Milliseconds,  ///< 纯毫秒
    Beat           ///< 拍号 + 分拍位
};

/// @brief 将时间格式偏好序列化为稳定文本。
void to_json(nlohmann::json& json, const TimeFormatPreference& preference);
/// @brief 从稳定文本读取时间格式偏好。
void from_json(const nlohmann::json& json, TimeFormatPreference& preference);

/// @brief 复制粘贴时用于计算相对偏移的时间基准。
enum class CopyPasteTimeBasis {
    Timestamp,  ///< 按时间戳秒数保持相对偏移
    Beat        ///< 按 BPM 分拍位置保持相对偏移
};

/// @brief 将复制粘贴时间基准序列化为稳定文本。
void to_json(nlohmann::json& json, const CopyPasteTimeBasis& basis);
/// @brief 从稳定文本读取复制粘贴时间基准。
void from_json(const nlohmann::json& json, CopyPasteTimeBasis& basis);

/// @brief 物件放置磁吸使用的分拍线来源。
enum class ObjectPlacementSnapMode {
    CurrentBeatDivisor,  ///< 仅使用当前分拍策略生成的分拍线。
    CommonBeatDivisors   ///< 使用用户选中的常用分拍线集合。
};

/// @brief 将物件放置磁吸模式序列化为稳定文本。
void to_json(nlohmann::json& json, const ObjectPlacementSnapMode& mode);
/// @brief 从稳定文本读取物件放置磁吸模式。
void from_json(const nlohmann::json& json, ObjectPlacementSnapMode& mode);

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
void to_json(nlohmann::json&                      json,
             const BpmMeasurementToolPreferences& preferences);
/// @brief 从 JSON 恢复 BPM 测量工具用户偏好。
void from_json(const nlohmann::json&          json,
               BpmMeasurementToolPreferences& preferences);

/// @brief 多人协作中远端视野范围的本地绘制模式。
enum class CollaborationViewportRenderMode : std::uint8_t {
    Filled,    ///< 绘制半透明填充和完整外框。
    Outline,   ///< 仅绘制完整外框。
    TrackEdge  ///< 仅在轨道边缘绘制类似左方括号的范围标记。
};

/// @brief 将协作视野绘制模式序列化为稳定文本。
void to_json(nlohmann::json& json, const CollaborationViewportRenderMode& mode);
/// @brief 从稳定文本读取协作视野绘制模式。
void from_json(const nlohmann::json&            json,
               CollaborationViewportRenderMode& mode);

/// @brief 公网协作目录与信令服务器的客户端连接配置。
struct CollaborationServerSettings {
    /// @brief 服务器域名或地址，不包含协议、端口和路径。
    std::string address{ "xiang233.top" };

    /// @brief WebSocket 信令端口。
    std::uint16_t signalingPort{ 443 };

    /// @brief 是否通过 TLS/WSS 连接服务器。
    bool useTls{ true };
};

/// @brief 将协作服务器配置序列化为 JSON。
void to_json(nlohmann::json& json, const CollaborationServerSettings& settings);
/// @brief 从 JSON 读取协作服务器配置并约束端口和地址边界。
void from_json(const nlohmann::json&        json,
               CollaborationServerSettings& settings);

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

    /// @brief 新建谱面的默认作者，同时作为联机写谱的客户端展示身份。
    std::string defaultCreator;

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

    /// @brief 是否将 libdatachannel 的 WebRTC/ICE Debug 日志写入应用日志。
    bool rtcDiagnosticLogging{ false };

    /// @brief 多人协作中远端视野范围的本地绘制模式。
    CollaborationViewportRenderMode collaborationViewportRenderMode{
        CollaborationViewportRenderMode::Filled
    };

    /// @brief 公网协作目录与信令服务器连接配置。
    CollaborationServerSettings collaborationServer;

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

    /// @brief 对所有项目生效的软件全局自动保存配置。
    AutoSaveConfig autoSave;

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

    /// @brief 抓取工具整体移动物件时是否锁定时间，仅允许横向换轨。
    bool disableVerticalObjectDrag{ false };

    /// @brief 移除折线路径上的物件
    bool removeObjectsOnPolylinePath{ false };

    /// @brief 是否允许编辑 Flick、Polyline 及折线子物件。
    bool enablePolylineEditing{ true };

    /// @brief 是否显示并允许编辑 BGM 轨道及自动采样。
    bool enableBmsEditing{ true };

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

/// @brief 将编辑器设置序列化为 JSON。
void to_json(nlohmann::json& json, const EditorSettings& settings);
/// @brief 从 JSON 读取编辑器设置并兼容旧字段。
void from_json(const nlohmann::json& json, EditorSettings& settings);

}  // namespace MMM::Config
