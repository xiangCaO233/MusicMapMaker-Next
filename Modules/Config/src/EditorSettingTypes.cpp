#include "config/AudioPlaybackConfig.h"
#include "config/CreatorIdentity.h"
#include "config/EditorConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace MMM::Config
{
// 本文件集中实现轻量设置类型的 ADL JSON 契约及向后兼容回退。
/// @brief 将持久化打击音效增益限制为音频控制层支持的有限范围。
/// @param gain 配置文件读取的线性增益。
/// @return 0.0~2.0 的有限值；非有限输入按静音增益处理。
float sanitizeHitSfxGain(float gain) noexcept
{
    return std::isfinite(gain) ? std::clamp(gain, 0.0F, 2.0F) : 0.0F;
}

/// @brief 将音频同步算法枚举写为稳定英文标识。
/// @param json 接收枚举标识的 JSON 值。
/// @param mode 当前同步算法。
/// @note 未知枚举值保守写为 None，避免生成无法回读的配置。
void to_json(nlohmann::json& json, const SyncMode& mode)
{
    // 先设置安全默认值，switch 只覆盖已知可持久化枚举。
    json = "None";
    switch ( mode ) {
    case SyncMode::None: json = "None"; break;
    case SyncMode::Integral: json = "Integral"; break;
    case SyncMode::WaterTank: json = "WaterTank"; break;
    }
}

/// @brief 从稳定英文标识恢复音频同步算法。
/// @param json 待读取的 JSON 值。
/// @param mode 接收结果的同步算法；非法值回退到 None。
/// @note 读取端接受缺失或未来未知值，不让旧客户端启用错误算法。
void from_json(const nlohmann::json& json, SyncMode& mode)
{
    // 回退值在类型检查前写入，所有早退都产生确定状态。
    mode = SyncMode::None;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "Integral" ) {
        mode = SyncMode::Integral;
    } else if ( value == "WaterTank" ) {
        mode = SyncMode::WaterTank;
    }
}

/// @brief 序列化同步控制器的算法及调节参数。
/// @param json 接收同步配置对象。
/// @param config 待保存的同步配置。
/// @note 字段名属于用户配置兼容契约，不随成员命名变化调整。
void to_json(nlohmann::json& json, const SyncConfig& config)
{
    // 各算法共用一个对象保存，切换模式后可保留其他模式的调节值。
    json = nlohmann::json{ { "mode", config.mode },
                           { "integralFactor", config.integralFactor },
                           { "waterTankBuffer", config.waterTankBuffer },
                           { "syncInterval", config.syncInterval } };
}

/// @brief 从 JSON 恢复同步控制器配置。
/// @param json 可能来自旧版本的同步配置对象。
/// @param config 接收字段值及逐项默认值。
/// @note 缺失字段采用历史默认参数，维持既有播放同步体验。
void from_json(const nlohmann::json& json, SyncConfig& config)
{
    // mode 的默认值沿用旧配置启用的积分同步，而非枚举自身 None。
    config.mode            = json.value("mode", SyncMode::Integral);
    config.integralFactor  = json.value("integralFactor", 0.1f);
    config.waterTankBuffer = json.value("waterTankBuffer", 0.05f);
    config.syncInterval    = json.value("syncInterval", 10.0);
}

/// @brief 将折线音效策略写为稳定英文标识。
/// @param json 接收策略标识的 JSON 值。
/// @param strategy 待保存的折线音效策略。
/// @note 未知枚举保守写为 Exact，保持每个音符的精确行为。
void to_json(nlohmann::json& json, const PolylineSfxStrategy& strategy)
{
    // Exact 是最完整的表达，作为序列化前的安全默认值。
    json = "Exact";
    switch ( strategy ) {
    case PolylineSfxStrategy::Exact: json = "Exact"; break;
    case PolylineSfxStrategy::InternalAsNormal:
        json = "InternalAsNormal";
        break;
    case PolylineSfxStrategy::OnlyTailExact: json = "OnlyTailExact"; break;
    case PolylineSfxStrategy::AllAsNormal: json = "AllAsNormal"; break;
    }
}

/// @brief 从配置标识恢复折线音效策略。
/// @param json 待读取的 JSON 字符串。
/// @param strategy 接收已知策略；非法值回退到 Exact。
/// @note 对未知标识不进行模糊匹配，避免拼写错误改变声音语义。
void from_json(const nlohmann::json& json, PolylineSfxStrategy& strategy)
{
    // 类型或内容不符合契约时维持最精确的默认策略。
    strategy = PolylineSfxStrategy::Exact;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "InternalAsNormal" ) {
        strategy = PolylineSfxStrategy::InternalAsNormal;
    } else if ( value == "OnlyTailExact" ) {
        strategy = PolylineSfxStrategy::OnlyTailExact;
    } else if ( value == "AllAsNormal" ) {
        strategy = PolylineSfxStrategy::AllAsNormal;
    }
}

/// @brief 序列化编辑器打击音效和声道控制配置。
/// @param json 接收完整音效配置对象。
/// @param config 待保存的音效策略、开关、增益与永久通道状态。
/// @note 增益在写出时再次净化，阻止非有限值进入持久化文件。
void to_json(nlohmann::json& json, const SfxConfig& config)
{
    // 策略与 Flick 宽度缩放字段描述物件类型到音效的映射规则。
    json = nlohmann::json{
        { "polylineStrategy", config.polylineStrategy },
        { "enableFlickWidthVolumeScaling",
          config.enableFlickWidthVolumeScaling },
        { "flickWidthVolumeMultiplier", config.flickWidthVolumeMultiplier },
        // 立体声总开关替代旧版仅针对 Flick 声道的命名。
        { "enableStereoHitEffects", config.enableStereoHitEffects },
        // 永久音量和静音映射按资源标识保存，不依赖加载顺序。
        { "permanentSfxVolumes", config.permanentSfxVolumes },
        { "permanentSfxMutes", config.permanentSfxMutes },
        // 全局播放开关与绑定状态分别持久化，支持独立 UI 控制。
        { "hitSfxSyncSpeed", config.hitSfxSyncSpeed },
        { "enableHitSfx", config.enableHitSfx },
        { "enableUnboundHitSfx", config.enableUnboundHitSfx },
        { "unboundHitSfxGain", sanitizeHitSfxGain(config.unboundHitSfxGain) },
        { "enableBoundHitSfx", config.enableBoundHitSfx },
        { "boundHitSfxGain", sanitizeHitSfxGain(config.boundHitSfxGain) }
    };
}

/// @brief 从 JSON 恢复打击音效配置并迁移旧字段。
/// @param json 用户配置中的音效对象。
/// @param config 接收兼容后的完整配置。
/// @note 缺失字段保持历史默认体验，增益统一限制到有效线性范围。
void from_json(const nlohmann::json& json, SfxConfig& config)
{
    // 策略和宽度缩放采用功能引入时的默认值，旧配置不会突然改变音效。
    config.polylineStrategy =
        json.value("polylineStrategy", PolylineSfxStrategy::Exact);
    config.enableFlickWidthVolumeScaling =
        json.value("enableFlickWidthVolumeScaling", false);
    config.flickWidthVolumeMultiplier =
        json.value("flickWidthVolumeMultiplier", 0.1f);
    // 新字段优先；缺失时读取旧 enableDirectionalFlickChannels 完成迁移。
    config.enableStereoHitEffects =
        json.value("enableStereoHitEffects",
                   json.value("enableDirectionalFlickChannels", true));
    // 按值恢复用户对各资源设置的永久混音状态。
    config.permanentSfxVolumes =
        json.value("permanentSfxVolumes", std::map<std::string, float>());
    config.permanentSfxMutes =
        json.value("permanentSfxMutes", std::map<std::string, bool>());
    config.hitSfxSyncSpeed     = json.value("hitSfxSyncSpeed", true);
    config.enableHitSfx        = json.value("enableHitSfx", true);
    config.enableUnboundHitSfx = json.value("enableUnboundHitSfx", true);
    // 从不受信任 JSON 读取的浮点增益必须同时处理越界与 NaN。
    config.unboundHitSfxGain =
        sanitizeHitSfxGain(json.value("unboundHitSfxGain", 1.0F));
    config.enableBoundHitSfx = json.value("enableBoundHitSfx", true);
    config.boundHitSfxGain =
        sanitizeHitSfxGain(json.value("boundHitSfxGain", 1.0F));
}

/// @brief 序列化文件选择器实现偏好。
/// @param json 接收 Native 或 Unified 标识。
/// @param style 当前文件选择器风格。
/// @note 二值枚举未知状态按 Native 保存，优先使用平台原生路径。
void to_json(nlohmann::json& json, const FilePickerStyle& style)
{
    // 三元表达式保证输出只出现公开支持的两个标识。
    json = style == FilePickerStyle::Unified ? "Unified" : "Native";
}

/// @brief 恢复文件选择器实现偏好。
/// @param json 待读取的 JSON 值。
/// @param style 接收解析结果；非法值回退到 Native。
/// @note 只有精确 Unified 标识会启用统一选择器。
void from_json(const nlohmann::json& json, FilePickerStyle& style)
{
    // 默认赋值覆盖调用方旧状态，使非字符串输入也具备确定结果。
    style = FilePickerStyle::Native;
    if ( json.is_string() && json.get<std::string>() == "Unified" ) {
        style = FilePickerStyle::Unified;
    }
}

/// @brief 序列化系统或软件光标偏好。
/// @param json 接收稳定光标风格标识。
/// @param style 当前光标风格。
/// @note 非 System 状态按 Software 写出，保证自绘参数继续生效。
void to_json(nlohmann::json& json, const CursorStyle& style)
{
    // 输出标识与设置界面的两个可选项一一对应。
    json = style == CursorStyle::System ? "System" : "Software";
}

/// @brief 从配置恢复光标渲染风格。
/// @param json 待读取的 JSON 值。
/// @param style 接收解析结果；非法值回退到 Software。
/// @note 精确 System 标识之外的内容不会关闭软件光标路径。
void from_json(const nlohmann::json& json, CursorStyle& style)
{
    // Software 是跨平台一致的应用默认值。
    style = CursorStyle::Software;
    if ( json.is_string() && json.get<std::string>() == "System" ) {
        style = CursorStyle::System;
    }
}

/// @brief 序列化软件光标、拖尾和烟雾外观参数。
/// @param json 接收软件光标配置对象。
/// @param config 待保存的尺寸、寿命和 BPM 同步开关。
/// @note 保留所有调节值，即使当前用户选择系统光标。
void to_json(nlohmann::json& json, const SoftwareCursorConfig& config)
{
    // 光标主体、拖尾和烟雾分别持久化，支持独立调节。
    json = nlohmann::json{ { "cursorSize", config.cursorSize },
                           { "trailSize", config.trailSize },
                           { "trailLifeTime", config.trailLifeTime },
                           { "smokeSize", config.smokeSize },
                           { "smokeLifeTime", config.smokeLifeTime },
                           { "enableBpmSyncSmokeLife",
                             config.enableBpmSyncSmokeLife } };
}

/// @brief 从 JSON 恢复软件光标外观参数。
/// @param json 用户配置中的软件光标对象。
/// @param config 接收各字段或历史默认值。
/// @note 缺失 BPM 同步字段按关闭处理，保持旧版固定寿命行为。
void from_json(const nlohmann::json& json, SoftwareCursorConfig& config)
{
    // 默认尺寸与寿命和首次启动配置保持一致。
    config.cursorSize             = json.value("cursorSize", 64.0f);
    config.trailSize              = json.value("trailSize", 48.0f);
    config.trailLifeTime          = json.value("trailLifeTime", 0.4f);
    config.smokeSize              = json.value("smokeSize", 32.0f);
    config.smokeLifeTime          = json.value("smokeLifeTime", 0.8f);
    config.enableBpmSyncSmokeLife = json.value("enableBpmSyncSmokeLife", false);
}

/// @brief 序列化窗口圆角、间距和动画时长等 UI 外观参数。
/// @param json 接收外观配置对象。
/// @param config 待保存的 UI 尺寸参数。
/// @note 这些值不包含主题颜色，主题资源由皮肤系统独立管理。
void to_json(nlohmann::json& json, const UIAestheticsConfig& config)
{
    // 布局间距和过渡时长放在同一对象，便于设置页整体重置。
    json = nlohmann::json{ { "windowRounding", config.windowRounding },
                           { "frameRounding", config.frameRounding },
                           { "windowGap", config.windowGap },
                           { "itemSpacing", config.itemSpacing },
                           { "windowPadding", config.windowPadding },
                           { "animationTransitionDuration",
                             config.animationTransitionDuration } };
}

/// @brief 从 JSON 恢复 UI 外观参数并限制动画最小时长。
/// @param json 用户配置中的 aesthetics 对象。
/// @param config 接收完整外观参数。
/// @note 过短动画会破坏过渡插值，因此读取时执行下界约束。
void from_json(const nlohmann::json& json, UIAestheticsConfig& config)
{
    // 圆角、间距和内边距缺失时使用当前视觉基线。
    config.windowRounding = json.value("windowRounding", 8.0f);
    config.frameRounding  = json.value("frameRounding", 6.0f);
    config.windowGap      = json.value("windowGap", 8.0f);
    config.itemSpacing    = json.value("itemSpacing", 8.0f);
    config.windowPadding  = json.value("windowPadding", 8.0f);
    // 最小值约束同时覆盖旧文件中的零值和负值。
    config.animationTransitionDuration =
        std::max(UIAestheticsConfig::MIN_ANIMATION_TRANSITION_DURATION,
                 json.value("animationTransitionDuration", 0.12f));
}

/// @brief 序列化一个具名音符与分拍线调色方案。
/// @param json 接收方案对象。
/// @param scheme 待保存的名称和两类颜色集合。
/// @note 颜色具体表示由对应轻量类型的 ADL 序列化负责。
void to_json(nlohmann::json& json, const ColorPaletteScheme& scheme)
{
    // 方案名称和颜色数据共同保存，支持独立文件及内嵌配置复用。
    json = nlohmann::json{ { "name", scheme.name },
                           { "noteColors", scheme.noteColors },
                           { "beatLineColors", scheme.beatLineColors } };
}

/// @brief 从 JSON 恢复单个调色方案。
/// @param json 待读取的方案对象。
/// @param scheme 接收名称和颜色集合。
/// @note 缺失名称采用可识别占位文本，缺失颜色保持空或默认调色板。
void from_json(const nlohmann::json& json, ColorPaletteScheme& scheme)
{
    // noteColors 缺失时构造其实际容器类型，避免硬编码实现类型。
    scheme.name       = json.value("name", std::string("Palette"));
    scheme.noteColors = json.value("noteColors", decltype(scheme.noteColors){});
    scheme.beatLineColors =
        json.value("beatLineColors", BeatLineColorPalette{});
}

/// @brief 序列化调色方案列表及当前选择索引。
/// @param json 接收调色配置对象。
/// @param config 待保存的方案集合和活动索引。
/// @note 索引语义依赖同一对象中的 schemes 顺序。
void to_json(nlohmann::json& json, const ColorPaletteConfig& config)
{
    // 活动索引和数组必须原子写入同一 JSON 对象。
    json = nlohmann::json{ { "activeSchemeIndex", config.activeSchemeIndex },
                           { "schemes", config.schemes } };
}

/// @brief 从 JSON 恢复调色方案集合并校正活动索引。
/// @param json 用户配置中的调色对象。
/// @param config 接收方案集合和安全索引。
/// @note 空集合或越界索引统一回退到零，实际默认方案由上层补齐。
void from_json(const nlohmann::json& json, ColorPaletteConfig& config)
{
    // 先读取集合再判断索引范围，避免对旧集合大小作假设。
    config.activeSchemeIndex =
        json.value("activeSchemeIndex", std::size_t{ 0 });
    config.schemes = json.value("schemes", std::vector<ColorPaletteScheme>());
    if ( config.schemes.empty() ||
         config.activeSchemeIndex >= config.schemes.size() ) {
        config.activeSchemeIndex = 0;
    }
}

/// @brief 序列化单个快捷键及修饰键组合。
/// @param json 接收快捷键对象。
/// @param binding 待保存的启用状态、主键和修饰键。
/// @note 主键使用稳定字符串表示，不持久化平台扫描码。
void to_json(nlohmann::json& json, const ShortcutBinding& binding)
{
    // enabled 独立保存，允许暂时关闭绑定而保留用户组合。
    json = nlohmann::json{
        { "enabled", binding.enabled }, { "key", binding.key },
        { "ctrl", binding.ctrl },       { "shift", binding.shift },
        { "alt", binding.alt },         { "super", binding.super }
    };
}

/// @brief 从 JSON 恢复单个快捷键绑定。
/// @param json 待读取的快捷键对象。
/// @param binding 接收组合及最终启用状态。
/// @note 空主键始终强制禁用，避免仅修饰键形成不可执行绑定。
void from_json(const nlohmann::json& json, ShortcutBinding& binding)
{
    // 修饰键逐项默认关闭，旧配置不会凭空获得额外组合条件。
    binding.enabled = json.value("enabled", true);
    binding.key     = json.value("key", std::string());
    binding.ctrl    = json.value("ctrl", false);
    binding.shift   = json.value("shift", false);
    binding.alt     = json.value("alt", false);
    binding.super   = json.value("super", false);
    // enabled=true 不能覆盖空键的不完整配置。
    if ( binding.key.empty() ) binding.enabled = false;
}

/// @brief 序列化编辑器全部可配置快捷键。
/// @param json 接收按动作名组织的快捷键对象。
/// @param config 待保存的工具、编辑和播放动作绑定。
/// @note 动作字段名是持久化和设置界面之间的稳定标识。
void to_json(nlohmann::json& json, const ShortcutConfig& config)
{
    // 工具切换动作保持在列表前部，便于人工阅读配置文件。
    json = nlohmann::json{
        { "toolMove", config.toolMove },
        { "toolMarquee", config.toolMarquee },
        { "toolDraw", config.toolDraw },
        { "toolColorBrush", config.toolColorBrush },
        { "toolColorEraser", config.toolColorEraser },
        // 变换和编辑动作紧随工具组，字段顺序不影响 JSON 读取。
        { "mirror", config.mirror },
        { "mirrorPaste", config.mirrorPaste },
        { "editSelectedVolume", config.editSelectedVolume },
        { "addSelectedAnnotation", config.addSelectedAnnotation },
        { "deleteSelected", config.deleteSelected },
        // 剩余字段为播放及视图状态切换，不保存瞬时 UI 状态。
        { "togglePlayback", config.togglePlayback },
        { "toggleReverseScroll", config.toggleReverseScroll },
        { "toggleScrollSnap", config.toggleScrollSnap },
        { "toggleSnapFloor", config.toggleSnapFloor },
        { "toggleScrollTimingMapping", config.toggleScrollTimingMapping },
        { "toggleBeatLines", config.toggleBeatLines },
        { "toggleStopPlaybackOnScroll", config.toggleStopPlaybackOnScroll },
        { "toggleHitSfx", config.toggleHitSfx },
        { "toggleHitEffects", config.toggleHitEffects },
        { "toggleSyncSameMainAudio", config.toggleSyncSameMainAudio }
    };
}

/// @brief 从 JSON 恢复全部快捷键并迁移旧版冲突绑定。
/// @param json 用户配置中的 shortcutConfig 对象。
/// @param config 接收逐动作兼容后的快捷键集合。
/// @note 每个缺失动作从当前默认配置取得，新增动作无需迁移旧文件。
void from_json(const nlohmann::json& json, ShortcutConfig& config)
{
    // 单一 defaults 快照保证同一次读取中的所有回退值来自同一版本。
    const ShortcutConfig defaults;
    config.toolMove    = json.value("toolMove", defaults.toolMove);
    config.toolMarquee = json.value("toolMarquee", defaults.toolMarquee);
    config.toolDraw    = json.value("toolDraw", defaults.toolDraw);
    config.toolColorBrush =
        json.value("toolColorBrush", defaults.toolColorBrush);
    config.toolColorEraser =
        json.value("toolColorEraser", defaults.toolColorEraser);
    config.mirror      = json.value("mirror", defaults.mirror);
    config.mirrorPaste = json.value("mirrorPaste", defaults.mirrorPaste);
    config.editSelectedVolume =
        json.value("editSelectedVolume", defaults.editSelectedVolume);
    config.addSelectedAnnotation =
        json.value("addSelectedAnnotation", defaults.addSelectedAnnotation);
    // 旧版注释快捷键与现有动作冲突时恢复新默认，避免升级后双重触发。
    const ShortcutBinding legacyAnnotationShortcut{ true,  "A",  true,
                                                    false, true, false };
    // 使用行为级冲突比较而非 JSON 原文，覆盖字段缺失后的等价组合。
    if ( shortcutBindingsConflict(config.addSelectedAnnotation,
                                  legacyAnnotationShortcut) ) {
        config.addSelectedAnnotation = defaults.addSelectedAnnotation;
    }
    config.deleteSelected =
        json.value("deleteSelected", defaults.deleteSelected);
    config.togglePlayback =
        json.value("togglePlayback", defaults.togglePlayback);
    config.toggleReverseScroll =
        json.value("toggleReverseScroll", defaults.toggleReverseScroll);
    config.toggleScrollSnap =
        json.value("toggleScrollSnap", defaults.toggleScrollSnap);
    config.toggleSnapFloor =
        json.value("toggleSnapFloor", defaults.toggleSnapFloor);
    config.toggleScrollTimingMapping = json.value(
        "toggleScrollTimingMapping", defaults.toggleScrollTimingMapping);
    config.toggleBeatLines =
        json.value("toggleBeatLines", defaults.toggleBeatLines);
    config.toggleStopPlaybackOnScroll = json.value(
        "toggleStopPlaybackOnScroll", defaults.toggleStopPlaybackOnScroll);
    config.toggleHitSfx = json.value("toggleHitSfx", defaults.toggleHitSfx);
    config.toggleHitEffects =
        json.value("toggleHitEffects", defaults.toggleHitEffects);
    config.toggleSyncSameMainAudio =
        json.value("toggleSyncSameMainAudio", defaults.toggleSyncSameMainAudio);
}

/// @brief 将音频播放后端写为稳定英文标识。
/// @param json 接收后端名称的 JSON 值。
/// @param backend 当前音频播放后端。
/// @note 未知枚举按 SDL 写出，保持跨平台默认后端。
void to_json(nlohmann::json& json, const AudioPlaybackBackend& backend)
{
    // 先设置 SDL 默认值，switch 只发布明确支持的后端。
    json = "SDL";
    switch ( backend ) {
    case AudioPlaybackBackend::SDL: json = "SDL"; break;
    case AudioPlaybackBackend::OpenAL: json = "OpenAL"; break;
    }
}

/// @brief 从配置恢复音频播放后端。
/// @param json 待读取的后端标识。
/// @param backend 接收 SDL 或 OpenAL；非法值回退到 SDL。
/// @note 精确匹配避免未来后端名被旧客户端错误解释。
void from_json(const nlohmann::json& json, AudioPlaybackBackend& backend)
{
    // 默认值在类型判断之前建立，所有早退路径结果一致。
    backend = AudioPlaybackBackend::SDL;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "OpenAL" ) {
        backend = AudioPlaybackBackend::OpenAL;
    } else if ( value == "SDL" ) {
        backend = AudioPlaybackBackend::SDL;
    }
}

/// @brief 序列化 OpenAL 空间声场参数。
/// @param json 接收空间音频配置对象。
/// @param config 待保存的方向、距离和衰减参数。
/// @note 即使 OpenAL 当前未启用也保留用户调节值。
void to_json(nlohmann::json& json, const OpenALSpatialConfig& config)
{
    // 启用开关与向量、距离模型参数共同形成完整后端配置。
    json = nlohmann::json{ { "enabled", config.enabled },
                           { "directionX", config.directionX },
                           { "directionY", config.directionY },
                           { "directionZ", config.directionZ },
                           { "distance", config.distance },
                           { "referenceDistance", config.referenceDistance },
                           { "maxDistance", config.maxDistance },
                           { "rolloffFactor", config.rolloffFactor } };
}

/// @brief 从 JSON 恢复 OpenAL 空间声场参数。
/// @param json 用户配置中的空间音频对象。
/// @param config 接收各字段或历史默认值。
/// @note 默认方向朝向负 Z，匹配编辑器既有声场坐标系。
void from_json(const nlohmann::json& json, OpenALSpatialConfig& config)
{
    // 旧配置缺少 enabled 时保持关闭，不意外改变音频输出。
    config.enabled           = json.value("enabled", false);
    config.directionX        = json.value("directionX", 0.0f);
    config.directionY        = json.value("directionY", 0.0f);
    config.directionZ        = json.value("directionZ", -1.0f);
    config.distance          = json.value("distance", 1.0f);
    config.referenceDistance = json.value("referenceDistance", 1.0f);
    config.maxDistance       = json.value("maxDistance", 100.0f);
    config.rolloffFactor     = json.value("rolloffFactor", 1.0f);
}

/// @brief 将帧率限制偏好写为刷新率倍数标识。
/// @param json 接收帧率策略字符串。
/// @param preference 当前帧率限制偏好。
/// @note VSync 作为未知枚举的保守输出，避免无上限占用资源。
void to_json(nlohmann::json& json, const FrameLimitPreference& preference)
{
    // 所有倍率以语义标识保存，不固化当前显示器的具体刷新率。
    json = "VSync";
    switch ( preference ) {
    case FrameLimitPreference::VSync: json = "VSync"; break;
    case FrameLimitPreference::Refresh2x: json = "Refresh2x"; break;
    case FrameLimitPreference::Refresh4x: json = "Refresh4x"; break;
    case FrameLimitPreference::Refresh8x: json = "Refresh8x"; break;
    case FrameLimitPreference::Unlimited: json = "Unlimited"; break;
    }
}

/// @brief 从稳定标识恢复帧率限制偏好。
/// @param json 待读取的 JSON 字符串。
/// @param preference 接收已知策略；非法值回退到 VSync。
/// @note 旧版布尔 vsync 的迁移由 EditorSettings 读取层处理。
void from_json(const nlohmann::json& json, FrameLimitPreference& preference)
{
    // 类型错误和未知字符串都保留同步刷新这一安全默认值。
    preference = FrameLimitPreference::VSync;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "VSync" ) {
        preference = FrameLimitPreference::VSync;
    } else if ( value == "Refresh2x" ) {
        preference = FrameLimitPreference::Refresh2x;
    } else if ( value == "Refresh4x" ) {
        preference = FrameLimitPreference::Refresh4x;
    } else if ( value == "Refresh8x" ) {
        preference = FrameLimitPreference::Refresh8x;
    } else if ( value == "Unlimited" ) {
        preference = FrameLimitPreference::Unlimited;
    }
}

/// @brief 序列化框选包含判定模式。
/// @param json 接收 Intersection 或 Strict 标识。
/// @param mode 当前选择判定模式。
/// @note 二值枚举未知状态按 Strict 写出，避免扩大选择范围。
void to_json(nlohmann::json& json, const SelectionMode& mode)
{
    // 标识描述几何语义，不依赖界面翻译文本。
    json = mode == SelectionMode::Intersection ? "Intersection" : "Strict";
}

/// @brief 从配置恢复框选包含判定模式。
/// @param json 待读取的模式标识。
/// @param mode 接收解析结果；非法值回退到 Strict。
/// @note 只有精确 Intersection 标识允许相交即选中。
void from_json(const nlohmann::json& json, SelectionMode& mode)
{
    // Strict 默认值保证损坏配置不会意外选中边界外物件。
    mode = SelectionMode::Strict;
    if ( json.is_string() && json.get<std::string>() == "Intersection" ) {
        mode = SelectionMode::Intersection;
    }
}

/// @brief 序列化谱面保存格式偏好。
/// @param json 接收 Original 或 ForceMMM 标识。
/// @param preference 当前保存格式策略。
/// @note 非 ForceMMM 状态按保留原格式写出。
void to_json(nlohmann::json& json, const SaveFormatPreference& preference)
{
    // 输出语义标识而不是扩展名，避免与具体格式列表耦合。
    json =
        preference == SaveFormatPreference::ForceMMM ? "ForceMMM" : "Original";
}

/// @brief 从配置恢复谱面保存格式偏好。
/// @param json 待读取的策略标识。
/// @param preference 接收解析结果；非法值回退到 Original。
/// @note 只有显式 ForceMMM 才允许保存时转换格式。
void from_json(const nlohmann::json& json, SaveFormatPreference& preference)
{
    // 回退到 Original 可避免未知配置触发不可逆格式转换。
    preference = SaveFormatPreference::Original;
    if ( json.is_string() && json.get<std::string>() == "ForceMMM" ) {
        preference = SaveFormatPreference::ForceMMM;
    }
}

/// @brief 将自动保存模式写为稳定英文标识。
/// @param json 接收 Disabled、Timed 或 EventTriggered 标识。
/// @param mode 当前自动保存模式。
/// @note default 分支按 Disabled 写出，未知状态不触发后台保存。
void to_json(nlohmann::json& json, const AutoSaveMode& mode)
{
    // 显式 switch 保证持久化标识与设置项保持一一对应。
    switch ( mode ) {
    case AutoSaveMode::Timed: json = "Timed"; break;
    case AutoSaveMode::EventTriggered: json = "EventTriggered"; break;
    case AutoSaveMode::Disabled:
    default: json = "Disabled"; break;
    }
}

/// @brief 从配置恢复自动保存或自动备份触发模式。
/// @param json 待读取的模式标识。
/// @param mode 接收已知模式；非法值回退到 Disabled。
/// @note 保存与备份复用同一枚举，但各自拥有独立配置对象。
void from_json(const nlohmann::json& json, AutoSaveMode& mode)
{
    // 未识别值不启动定时器或事件监听，等待用户重新选择。
    mode = AutoSaveMode::Disabled;
    if ( !json.is_string() ) return;

    const auto value = json.get<std::string>();
    if ( value == "Timed" ) {
        mode = AutoSaveMode::Timed;
    } else if ( value == "EventTriggered" ) {
        mode = AutoSaveMode::EventTriggered;
    }
}

/// @brief 序列化自动保存间隔单位。
/// @param json 接收 Seconds 或 Minutes 标识。
/// @param unit 当前间隔单位。
/// @note 二值未知状态按 Seconds 写出，维持最小时间粒度。
void to_json(nlohmann::json& json, const AutoSaveIntervalUnit& unit)
{
    // 数值与单位分开保存，避免升级时对已有 intervalValue 重解释。
    json = unit == AutoSaveIntervalUnit::Minutes ? "Minutes" : "Seconds";
}

/// @brief 从配置恢复自动保存间隔单位。
/// @param json 待读取的单位标识。
/// @param unit 接收解析结果；非法值回退到 Seconds。
/// @note 自动备份读取层可为缺失字段指定不同的 Minutes 默认值。
void from_json(const nlohmann::json& json, AutoSaveIntervalUnit& unit)
{
    // 此类型级默认值为 Seconds，具体配置可在 json.value 处覆盖。
    unit = AutoSaveIntervalUnit::Seconds;
    if ( json.is_string() && json.get<std::string>() == "Minutes" ) {
        unit = AutoSaveIntervalUnit::Minutes;
    }
}

/// @brief 序列化自动保存模式、间隔和事件触发开关。
/// @param json 接收自动保存配置对象。
/// @param config 待保存的自动保存策略。
/// @note 间隔值写出时限制为 5 至 60，阻止持久化越界状态。
void to_json(nlohmann::json& json, const AutoSaveConfig& config)
{
    // 定时字段和四类事件开关共同保存，切换模式不会丢失偏好。
    json = nlohmann::json{
        { "mode", config.mode },
        { "intervalUnit", config.intervalUnit },
        { "intervalValue", std::clamp(config.intervalValue, 5, 60) },
        { "onObjectModified", config.onObjectModified },
        { "onBeatmapSwitch", config.onBeatmapSwitch },
        { "onImGuiWindowFocusLost", config.onImGuiWindowFocusLost },
        { "onNativeWindowFocusLost", config.onNativeWindowFocusLost },
    };
}

/// @brief 从 JSON 恢复自动保存配置并校正间隔范围。
/// @param json 用户配置中的 autoSave 对象。
/// @param config 接收模式、间隔及事件开关。
/// @note 旧配置缺少事件字段时全部默认启用，延续自动保护行为。
void from_json(const nlohmann::json& json, AutoSaveConfig& config)
{
    // 自动保存默认关闭，但保留可立即启用的 30 秒参数。
    config.mode = json.value("mode", AutoSaveMode::Disabled);
    config.intervalUnit =
        json.value("intervalUnit", AutoSaveIntervalUnit::Seconds);
    config.intervalValue = std::clamp(json.value("intervalValue", 30), 5, 60);
    // 各事件开关独立读取，部分配置不会覆盖其他默认触发点。
    config.onObjectModified       = json.value("onObjectModified", true);
    config.onBeatmapSwitch        = json.value("onBeatmapSwitch", true);
    config.onImGuiWindowFocusLost = json.value("onImGuiWindowFocusLost", true);
    config.onNativeWindowFocusLost =
        json.value("onNativeWindowFocusLost", true);
}

/// @brief 序列化自动备份触发策略和保留数量。
/// @param json 接收自动备份配置对象。
/// @param config 待保存的备份模式、间隔、事件和数量上限。
/// @note 间隔及最大备份数在写出时再次限制到支持范围。
void to_json(nlohmann::json& json, const AutoBackupConfig& config)
{
    // 备份复用自动保存触发字段，同时额外保存轮转数量上限。
    json = nlohmann::json{
        { "mode", config.mode },
        { "intervalUnit", config.intervalUnit },
        { "intervalValue", std::clamp(config.intervalValue, 5, 60) },
        { "onObjectModified", config.onObjectModified },
        { "onBeatmapSwitch", config.onBeatmapSwitch },
        { "onImGuiWindowFocusLost", config.onImGuiWindowFocusLost },
        { "onNativeWindowFocusLost", config.onNativeWindowFocusLost },
        // 保留数量使用共享常量，保持设置页与持久化边界一致。
        { "maxBackupCount",
          std::clamp(config.maxBackupCount,
                     AUTO_BACKUP_COUNT_MIN,
                     AUTO_BACKUP_COUNT_MAX) },
    };
}

/// @brief 从 JSON 恢复自动备份配置并校正数量范围。
/// @param json 用户配置中的 autoBackup 对象。
/// @param config 接收完整备份策略。
/// @note 缺失间隔单位默认 Minutes，与自动保存的 Seconds 默认不同。
void from_json(const nlohmann::json& json, AutoBackupConfig& config)
{
    // 自动备份默认关闭，预设五分钟便于用户启用后直接使用。
    config.mode = json.value("mode", AutoSaveMode::Disabled);
    config.intervalUnit =
        json.value("intervalUnit", AutoSaveIntervalUnit::Minutes);
    config.intervalValue    = std::clamp(json.value("intervalValue", 5), 5, 60);
    config.onObjectModified = json.value("onObjectModified", true);
    config.onBeatmapSwitch  = json.value("onBeatmapSwitch", true);
    config.onImGuiWindowFocusLost = json.value("onImGuiWindowFocusLost", true);
    config.onNativeWindowFocusLost =
        json.value("onNativeWindowFocusLost", true);
    // 轮转数量同时设置最小值，避免启用备份却不能保留任何版本。
    config.maxBackupCount = std::clamp(json.value("maxBackupCount", 10),
                                       AUTO_BACKUP_COUNT_MIN,
                                       AUTO_BACKUP_COUNT_MAX);
}

/// @brief 将时间显示偏好写为稳定英文标识。
/// @param json 接收 Clock、Seconds、Milliseconds 或 Beat 标识。
/// @param preference 当前时间显示格式。
/// @note Clock 作为未知枚举的安全输出，保持人类可读形式。
void to_json(nlohmann::json& json, const TimeFormatPreference& preference)
{
    // 显示格式只影响 UI，不改变谱面内部时间表示。
    json = "Clock";
    switch ( preference ) {
    case TimeFormatPreference::Clock: json = "Clock"; break;
    case TimeFormatPreference::Seconds: json = "Seconds"; break;
    case TimeFormatPreference::Milliseconds: json = "Milliseconds"; break;
    case TimeFormatPreference::Beat: json = "Beat"; break;
    }
}

/// @brief 从配置恢复时间显示格式。
/// @param json 待读取的格式标识。
/// @param preference 接收已知格式；非法值回退到 Clock。
/// @note Beat 格式依赖谱面时序，但这里仅恢复用户偏好。
void from_json(const nlohmann::json& json, TimeFormatPreference& preference)
{
    // 先建立 Clock 回退，再按精确字符串覆盖。
    preference = TimeFormatPreference::Clock;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "Seconds" ) {
        preference = TimeFormatPreference::Seconds;
    } else if ( value == "Milliseconds" ) {
        preference = TimeFormatPreference::Milliseconds;
    } else if ( value == "Beat" ) {
        preference = TimeFormatPreference::Beat;
    }
}

/// @brief 序列化复制粘贴使用的时间基准。
/// @param json 接收 Timestamp 或 Beat 标识。
/// @param basis 当前复制粘贴时间基准。
/// @note 未知状态按 Timestamp 写出，保留绝对时间位置。
void to_json(nlohmann::json& json, const CopyPasteTimeBasis& basis)
{
    // 二值标识直接对应粘贴时的偏移计算分支。
    json = basis == CopyPasteTimeBasis::Beat ? "Beat" : "Timestamp";
}

/// @brief 从配置恢复复制粘贴时间基准。
/// @param json 待读取的基准标识。
/// @param basis 接收解析结果；非法值回退到 Timestamp。
/// @note 只有精确 Beat 标识会启用节拍相对粘贴。
void from_json(const nlohmann::json& json, CopyPasteTimeBasis& basis)
{
    // Timestamp 对缺少 BPM 上下文的谱面仍具备明确语义。
    basis = CopyPasteTimeBasis::Timestamp;
    if ( json.is_string() && json.get<std::string>() == "Beat" ) {
        basis = CopyPasteTimeBasis::Beat;
    }
}

/// @brief 序列化物件放置吸附模式。
/// @param json 接收 CurrentBeatDivisor 或 CommonBeatDivisors 标识。
/// @param mode 当前吸附候选模式。
/// @note 未知状态按当前分拍器模式写出。
void to_json(nlohmann::json& json, const ObjectPlacementSnapMode& mode)
{
    // 标识描述候选集合来源，不持久化瞬时吸附结果。
    json = mode == ObjectPlacementSnapMode::CommonBeatDivisors
               ? "CommonBeatDivisors"
               : "CurrentBeatDivisor";
}

/// @brief 从配置恢复物件放置吸附模式。
/// @param json 待读取的模式标识。
/// @param mode 接收解析结果；非法值回退到当前分拍器模式。
/// @note 常用分拍集合的具体位掩码由 EditorSettings 单独读取。
void from_json(const nlohmann::json& json, ObjectPlacementSnapMode& mode)
{
    // 只有明确标识才启用多分拍候选搜索。
    mode = ObjectPlacementSnapMode::CurrentBeatDivisor;
    if ( json.is_string() && json.get<std::string>() == "CommonBeatDivisors" ) {
        mode = ObjectPlacementSnapMode::CommonBeatDivisors;
    }
}

/// @brief 序列化 BPM 测量工具的标记和视野偏好。
/// @param json 接收测量工具配置对象。
/// @param preferences 待保存的标记宽度、分拍器及视野范围。
/// @note 视野中心和半宽按秒保存，不依赖当前谱面 BPM。
void to_json(nlohmann::json&                      json,
             const BpmMeasurementToolPreferences& preferences)
{
    // 四个字段共同恢复测量窗口，但不保存任何谱面分析结果。
    json =
        nlohmann::json{ { "markerWidthMs", preferences.markerWidthMs },
                        { "beatDivisor", preferences.beatDivisor },
                        { "viewCenterSeconds", preferences.viewCenterSeconds },
                        { "viewHalfWidthSeconds",
                          preferences.viewHalfWidthSeconds } };
}

/// @brief 从 JSON 恢复 BPM 测量工具偏好。
/// @param json 用户配置中的测量工具对象。
/// @param preferences 接收各字段或当前默认值。
/// @note beatDivisor 缺失时使用四分拍，与编辑器基础默认一致。
void from_json(const nlohmann::json&          json,
               BpmMeasurementToolPreferences& preferences)
{
    // 视野默认覆盖中心前后八秒，便于首次打开时观察多个拍点。
    preferences.markerWidthMs        = json.value("markerWidthMs", 80.0);
    preferences.beatDivisor          = json.value("beatDivisor", 4);
    preferences.viewCenterSeconds    = json.value("viewCenterSeconds", 0.0);
    preferences.viewHalfWidthSeconds = json.value("viewHalfWidthSeconds", 8.0);
}

/// @brief 序列化协作成员视野的渲染方式。
/// @param json 接收 Filled、Outline 或 TrackEdge 标识。
/// @param mode 当前远端视野显示模式。
/// @note 未知枚举按 Filled 写出，保持最明显的协作提示。
void to_json(nlohmann::json& json, const CollaborationViewportRenderMode& mode)
{
    // default 与 Filled 合并，确保无未初始化 JSON 输出。
    switch ( mode ) {
    case CollaborationViewportRenderMode::Outline: json = "Outline"; break;
    case CollaborationViewportRenderMode::TrackEdge: json = "TrackEdge"; break;
    case CollaborationViewportRenderMode::Filled:
    default: json = "Filled"; break;
    }
}

/// @brief 从配置恢复协作视野渲染方式。
/// @param json 待读取的模式标识。
/// @param mode 接收已知模式；非法值回退到 Filled。
/// @note 渲染模式只影响本地展示，不参与网络同步协议。
void from_json(const nlohmann::json&            json,
               CollaborationViewportRenderMode& mode)
{
    // 未知或非字符串输入保持填充模式，不隐藏远端视野。
    mode = CollaborationViewportRenderMode::Filled;
    if ( !json.is_string() ) return;

    const std::string value = json.get<std::string>();
    if ( value == "Outline" ) {
        mode = CollaborationViewportRenderMode::Outline;
    } else if ( value == "TrackEdge" ) {
        mode = CollaborationViewportRenderMode::TrackEdge;
    }
}

/// @brief 序列化互斥状态工具组的按钮可见性。
/// @param json 接收状态工具可见性对象。
/// @param visibility 待保存的移动、框选、绘制、颜色和布局开关。
/// @note 这里只控制显示，工具功能和快捷键仍独立存在。
void to_json(nlohmann::json& json, const ToolbarStateToolVisibility& visibility)
{
    // 字段与工具栏稳定动作 ID 对齐，允许逐按钮兼容读取。
    json = nlohmann::json{ { "move", visibility.move },
                           { "marquee", visibility.marquee },
                           { "draw", visibility.draw },
                           { "colorBrush", visibility.colorBrush },
                           { "colorEraser", visibility.colorEraser },
                           { "layout", visibility.layout } };
}

/// @brief 从 JSON 恢复状态工具组的按钮可见性。
/// @param json 用户配置中的 stateTools 对象。
/// @param visibility 接收逐按钮开关。
/// @note 缺失字段从当前默认结构获取，新增工具在旧配置中仍可见。
void from_json(const nlohmann::json&       json,
               ToolbarStateToolVisibility& visibility)
{
    // defaults 避免在每个字段处重复硬编码产品默认值。
    const ToolbarStateToolVisibility defaults;
    visibility.move        = json.value("move", defaults.move);
    visibility.marquee     = json.value("marquee", defaults.marquee);
    visibility.draw        = json.value("draw", defaults.draw);
    visibility.colorBrush  = json.value("colorBrush", defaults.colorBrush);
    visibility.colorEraser = json.value("colorEraser", defaults.colorEraser);
    visibility.layout      = json.value("layout", defaults.layout);
}

/// @brief 序列化非互斥工具栏按钮的可见性。
/// @param json 接收独立按钮可见性对象。
/// @param visibility 待保存的调色板、磁吸、播放和数值控件开关。
/// @note 独立按钮不属于状态工具选择，因此单独分组持久化。
void to_json(nlohmann::json&                           json,
             const ToolbarIndependentButtonVisibility& visibility)
{
    // 配置对象保持与界面分组一致，便于设置页按组重置。
    json = nlohmann::json{ { "notePalette", visibility.notePalette },
                           { "magnet", visibility.magnet },
                           { "scrollTimingMapping",
                             visibility.scrollTimingMapping },
                           { "beatLineDisplay", visibility.beatLineDisplay },
                           { "soundEffectTool", visibility.soundEffectTool },
                           { "playback", visibility.playback },
                           { "playbackSpeed", visibility.playbackSpeed },
                           { "trackCount", visibility.trackCount },
                           { "beatDivisor", visibility.beatDivisor } };
}

/// @brief 从 JSON 恢复非互斥工具栏按钮可见性。
/// @param json 用户配置中的 independentButtons 对象。
/// @param visibility 接收逐按钮开关。
/// @note 部分旧配置缺失的新按钮使用当前默认值，不继承相邻字段。
void from_json(const nlohmann::json&               json,
               ToolbarIndependentButtonVisibility& visibility)
{
    // 每个按钮独立读取，手工编辑的部分对象不会重置其他按钮。
    const ToolbarIndependentButtonVisibility defaults;
    visibility.notePalette = json.value("notePalette", defaults.notePalette);
    visibility.magnet      = json.value("magnet", defaults.magnet);
    visibility.scrollTimingMapping =
        json.value("scrollTimingMapping", defaults.scrollTimingMapping);
    visibility.beatLineDisplay =
        json.value("beatLineDisplay", defaults.beatLineDisplay);
    visibility.soundEffectTool =
        json.value("soundEffectTool", defaults.soundEffectTool);
    visibility.playback = json.value("playback", defaults.playback);
    visibility.playbackSpeed =
        json.value("playbackSpeed", defaults.playbackSpeed);
    visibility.trackCount  = json.value("trackCount", defaults.trackCount);
    visibility.beatDivisor = json.value("beatDivisor", defaults.beatDivisor);
}

/// @brief 序列化工具栏可见性的两个职责分组。
/// @param json 接收工具栏可见性根对象。
/// @param visibility 待保存的状态工具与独立按钮配置。
/// @note 分组结构为后续新增按钮保留逐层默认能力。
void to_json(nlohmann::json& json, const ToolbarVisibilityConfig& visibility)
{
    // 根对象只组合子结构，具体字段由各自 ADL 函数维护。
    json = nlohmann::json{ { "stateTools", visibility.stateTools },
                           { "independentButtons",
                             visibility.independentButtons } };
}

/// @brief 从 JSON 恢复完整工具栏可见性配置。
/// @param json 用户配置中的 toolbarVisibility 对象。
/// @param visibility 接收两个分组或各自默认结构。
/// @note 任一分组缺失都不会影响另一个分组的持久化值。
void from_json(const nlohmann::json& json, ToolbarVisibilityConfig& visibility)
{
    // 子结构的 from_json 继续负责字段级兼容和默认值。
    visibility.stateTools =
        json.value("stateTools", ToolbarStateToolVisibility{});
    visibility.independentButtons =
        json.value("independentButtons", ToolbarIndependentButtonVisibility{});
}

/// @brief 序列化协作信令服务器端点设置。
/// @param json 接收地址、端口和 TLS 开关对象。
/// @param settings 待保存的协作服务器设置。
/// @note 不保存凭据或连接状态，配置仅描述公开端点。
void to_json(nlohmann::json& json, const CollaborationServerSettings& settings)
{
    // 端点三元组共同决定连接目标，必须位于同一对象中更新。
    json = nlohmann::json{ { "address", settings.address },
                           { "signalingPort", settings.signalingPort },
                           { "useTls", settings.useTls } };
}

/// @brief 从 JSON 恢复并校验协作信令服务器设置。
/// @param json 可能被手工编辑的服务器配置对象。
/// @param settings 接收合法字段；非法字段保留结构默认值。
/// @note 地址限制长度，端口限制为有效 TCP 范围，TLS 只接受布尔值。
void from_json(const nlohmann::json&        json,
               CollaborationServerSettings& settings)
{
    // 先整体重置，非对象输入不会保留调用方此前的半旧状态。
    settings = CollaborationServerSettings{};
    if ( !json.is_object() ) return;

    // 地址允许域名或 IP 文本，但拒绝空值和异常超长内容。
    if ( const auto address = json.find("address");
         address != json.end() && address->is_string() ) {
        const auto value = address->get<std::string>();
        if ( !value.empty() && value.size() <= 255U ) {
            settings.address = value;
        }
    }
    // 只接受无符号 JSON 数字，负数不会经过整数转换产生环绕。
    if ( const auto port = json.find("signalingPort");
         port != json.end() && port->is_number_unsigned() ) {
        const auto value = port->get<std::uint64_t>();
        if ( value > 0U && value <= 65535U ) {
            settings.signalingPort = static_cast<std::uint16_t>(value);
        }
    }
    // TLS 类型错误时沿用默认值，不把字符串 truthy 语义引入配置。
    if ( const auto useTls = json.find("useTls");
         useTls != json.end() && useTls->is_boolean() ) {
        settings.useTls = useTls->get<bool>();
    }
}

/// @brief 在应用项目级设置时保留仅属于全局界面的工具栏显示偏好。
/// @param target 即将被项目设置覆盖后继续使用的目标设置。
/// @param source 提供全局工具栏显示字段的当前软件设置。
/// @note 该函数只复制显示偏好，不混入项目可携带的编辑行为字段。
void preserveGlobalToolbarDisplaySettings(EditorSettings&       target,
                                          const EditorSettings& source)
{
    // 标签和固定窗口属于用户工作区布局，不应随谱面项目切换。
    target.showToolLabels    = source.showToolLabels;
    target.fixedToolWindow   = source.fixedToolWindow;
    target.showManagerLabels = source.showManagerLabels;
    // 分组按钮可见性整体复制，保证工具栏状态内部一致。
    target.toolbarVisibility = source.toolbarVisibility;
}

/// @brief 序列化全局编辑设置，仅写入共用的专业模式字段。
/// @param json 接收完整编辑设置对象。
/// @param settings 待保存的软件级编辑、音频、界面和协作偏好。
/// @note 字段名兼顾历史配置兼容，新增字段必须提供对应读取默认值。
void to_json(nlohmann::json& json, const EditorSettings& settings)
{
    // 同步、音效和文件选择器构成不依赖具体谱面的基础行为设置。
    json = nlohmann::json{
        { "syncConfig", settings.syncConfig },
        { "sfxConfig", settings.sfxConfig },
        { "filePickerStyle", settings.filePickerStyle },
        { "cursorStyle", settings.cursorStyle },
        // 主题、插件和皮肤选择共同决定启动时加载的外观资源。
        { "theme", settings.theme },
        { "disabledPluginIds", settings.disabledPluginIds },
        { "selectedSkinDirectory", settings.selectedSkinDirectory },
        // 分拍、吸附和滚动字段描述编辑交互，不保存瞬时视野位置。
        { "beatDivisor", settings.beatDivisor },
        { "enableToolbarValueWheelAdjustment",
          settings.enableToolbarValueWheelAdjustment },
        { "overlapTimeWindowMs", settings.overlapTimeWindowMs },
        { "reverseScroll", settings.reverseScroll },
        { "scrollSnap", settings.scrollSnap },
        { "objectPlacementSnap", settings.objectPlacementSnap },
        { "objectPlacementSnapMode", settings.objectPlacementSnapMode },
        { "commonBeatDivisorMask", settings.commonBeatDivisorMask },
        // 启动与语言身份字段属于应用级偏好，不随单个项目迁移。
        { "recentProjectsLimit", settings.recentProjectsLimit },
        { "showWelcomeOnStartup", settings.m_showWelcomeOnStartup },
        { "language", settings.language },
        { "defaultCreator", normalizeCreatorIdentity(settings.defaultCreator) },
        // 渲染帧率和音频后端字段在下次启动时用于初始化底层设备。
        { "frameLimit", settings.frameLimit },
        { "audioPlaybackBackend", settings.audioPlaybackBackend },
        { "audioDecodingMode",
          settings.audioDecodingMode == AudioDecodingMode::Streaming
              ? "streaming"
              : "cached" },
        { "sdlAudioOutputDeviceName", settings.sdlAudioOutputDeviceName },
        { "openALAudioOutputDeviceName", settings.openALAudioOutputDeviceName },
        { "openALSpatialConfig", settings.openALSpatialConfig },
        // 诊断和协作字段只保存用户授权及显示偏好，不保存会话状态。
        { "renderProfileLogging", settings.renderProfileLogging },
        { "rtcDiagnosticLogging", settings.rtcDiagnosticLogging },
        { "collaborationViewportRenderMode",
          settings.collaborationViewportRenderMode },
        { "collaborationServer", settings.collaborationServer },
        { "autoUploadPgoProfiles", settings.autoUploadPgoProfiles },
        { "pgoProfileUploadConsentAsked",
          settings.pgoProfileUploadConsentAsked },
        // UI 尺度与音量按线性值保存，具体运行时限制由消费层处理。
        { "fontSizeMultiplier", settings.fontSizeMultiplier },
        { "uiScaleMultiplier", settings.uiScaleMultiplier },
        { "scrollSpeedMultiplier", settings.scrollSpeedMultiplier },
        { "globalVolume", settings.globalVolume },
        { "globalMuted", settings.globalMuted },
        { "bgmGain", settings.bgmGain },
        { "bgmGainMuted", settings.bgmGainMuted },
        { "sfxGain", settings.sfxGain },
        { "sfxGainMuted", settings.sfxGainMuted },
        { "interactionSfxGain", settings.interactionSfxGain },
        { "interactionSfxGainMuted", settings.interactionSfxGainMuted },
        // 选择、保存和自动保护策略组成编辑数据安全相关偏好。
        { "selectionMode", settings.selectionMode },
        { "marqueeThickness", settings.marqueeThickness },
        { "marqueeRounding", settings.marqueeRounding },
        { "saveFormatPreference", settings.saveFormatPreference },
        { "autoSave", settings.autoSave },
        { "autoBackup", settings.autoBackup },
        { "autoAddStoreModeExtForMalodyExport",
          settings.autoAddStoreModeExtForMalodyExport },
        // 时间显示、文件选择器历史和绘制限制属于编辑工作流偏好。
        { "timeFormatPreference", settings.timeFormatPreference },
        { "lastFilePickerPath", settings.lastFilePickerPath },
        { "disableScrollAccelerationWhileDrawing",
          settings.disableScrollAccelerationWhileDrawing },
        { "disableVerticalObjectDrag", settings.disableVerticalObjectDrag },
        { "removeObjectsOnPolylinePath", settings.removeObjectsOnPolylinePath },
        // 实验性编辑能力逐项持久化，升级时可为缺失字段选择安全默认值。
        { "enablePolylineEditing", settings.enablePolylineEditing },
        { "enableBmsEditing", settings.enableBmsEditing },
        { "selectPastedObjects", settings.selectPastedObjects },
        { "copyPasteTimeBasis", settings.copyPasteTimeBasis },
        { "timelineSelectionIncludesBpm",
          settings.timelineSelectionIncludesBpm },
        // BPM 工具、光标和字体字段描述本机交互呈现，不写入谱面数据。
        { "bpmMeasurementToolPreferences",
          settings.bpmMeasurementToolPreferences },
        { "softwareCursorConfig", settings.softwareCursorConfig },
        { "preferredAsciiFont", settings.preferredAsciiFont },
        { "preferredCjkFont", settings.preferredCjkFont },
        // 窗口开关与工具栏显示属于全局工作区，项目覆盖时需显式保留。
        { "stopPlaybackOnScroll", settings.stopPlaybackOnScroll },
        { "snapFloor", settings.snapFloor },
        { "showTimelineWindow", settings.showTimelineWindow },
        { "professionalMode", settings.professionalMode },
        { "showPreviewWindow", settings.showPreviewWindow },
        { "showAnnotationDetails", settings.showAnnotationDetails },
        { "showToolLabels", settings.showToolLabels },
        { "toolbarVisibility", settings.toolbarVisibility },
        { "fixedToolWindow", settings.fixedToolWindow },
        { "showManagerLabels", settings.showManagerLabels },
        // 外观、调色板和快捷键使用独立子对象降低根层字段耦合。
        { "aesthetics", settings.aesthetics },
        { "colorPalettes", settings.colorPalettes },
        { "defaultColorPaletteSchemeName",
          settings.defaultColorPaletteSchemeName },
        { "shortcutConfig", settings.shortcutConfig }
    };
}

/// @brief 读取全局编辑设置，并兼容旧版时间线专业模式字段。
/// @param json 用户配置中的 settings 对象。
/// @param settings 接收所有已知字段及逐项默认值。
/// @note 迁移只发生在读取路径，后续保存会统一写出当前字段名。
void from_json(const nlohmann::json& json, EditorSettings& settings)
{
    // 基础子配置先通过各自 ADL 入口恢复，保持类型职责独立。
    settings.syncConfig = json.value("syncConfig", SyncConfig());
    settings.sfxConfig  = json.value("sfxConfig", SfxConfig());
    settings.filePickerStyle =
        json.value("filePickerStyle", FilePickerStyle::Native);
    settings.cursorStyle = json.value("cursorStyle", CursorStyle::Software);
    // 主题需要区分缺失、空字符串和旧内置名称，不能用单次 value 处理。
    if ( auto themeIterator = json.find("theme");
         themeIterator != json.end() && themeIterator->is_string() ) {
        settings.theme = themeIterator->get<std::string>();
        // 空主题迁移为自动选择，旧 MmmDefault 名称迁移到 Cecilia。
        if ( settings.theme.empty() ) {
            settings.theme = UI_THEME_AUTO_ID;
        } else if ( settings.theme == "MmmDefault" ) {
            settings.theme = "Cecilia";
        }
    } else {
        settings.theme = UI_THEME_AUTO_ID;
    }
    // 插件与皮肤选择缺失时恢复空禁用列表和内置默认皮肤。
    settings.disabledPluginIds =
        json.value("disabledPluginIds", std::vector<std::string>());
    settings.selectedSkinDirectory =
        json.value("selectedSkinDirectory", std::string("mmm-default"));
    // 编辑交互字段保留历史默认值，避免升级后基础吸附行为突变。
    settings.beatDivisor = json.value("beatDivisor", 4);
    settings.enableToolbarValueWheelAdjustment =
        json.value("enableToolbarValueWheelAdjustment", false);
    settings.overlapTimeWindowMs = json.value("overlapTimeWindowMs", 5.0f);
    settings.reverseScroll       = json.value("reverseScroll", false);
    settings.scrollSnap          = json.value("scrollSnap", false);
    // 新放置吸附开关缺失时继承旧 scrollSnap，完成行为迁移。
    settings.objectPlacementSnap =
        json.value("objectPlacementSnap", settings.scrollSnap);
    settings.objectPlacementSnapMode = json.value(
        "objectPlacementSnapMode", ObjectPlacementSnapMode::CurrentBeatDivisor);
    // 位掩码只保留当前支持位，忽略未来或损坏配置中的高位。
    settings.commonBeatDivisorMask =
        json.value("commonBeatDivisorMask", COMMON_BEAT_DIVISOR_MASK_DEFAULT) &
        COMMON_BEAT_DIVISOR_MASK_ALL;
    settings.recentProjectsLimit = json.value("recentProjectsLimit", 10);
    // 旧配置缺失或类型错误时继续显示欢迎页，避免无项目空白启动。
    const auto welcome = json.find("showWelcomeOnStartup");
    settings.m_showWelcomeOnStartup =
        welcome == json.end() || !welcome->is_boolean() || welcome->get<bool>();
    settings.language = json.value("language", std::string("zh_cn"));
    // 创作者字符串在进入内存前统一规范化，持久化两端语义对称。
    settings.defaultCreator =
        normalizeCreatorIdentity(json.value("defaultCreator", std::string()));
    // 新 frameLimit 缺失时迁移旧 vsync 布尔，再回退到刷新率二倍。
    settings.frameLimit = json.value(
        "frameLimit",
        json.contains("vsync")
            ? (json.value("vsync", false) ? FrameLimitPreference::VSync
                                          : FrameLimitPreference::Unlimited)
            : FrameLimitPreference::Refresh2x);
    // 未知值沿用完整缓存，确保旧配置及未来扩展都不会意外启用流式。
    settings.audioDecodingMode =
        json.value("audioDecodingMode", std::string("cached")) == "streaming"
            ? AudioDecodingMode::Streaming
            : AudioDecodingMode::Cached;
    // 解码模式解析后恢复设备后端和各后端独立的设备名称。
    settings.audioPlaybackBackend =
        json.value("audioPlaybackBackend", AudioPlaybackBackend::SDL);
    settings.sdlAudioOutputDeviceName =
        json.value("sdlAudioOutputDeviceName", std::string());
    settings.openALAudioOutputDeviceName =
        json.value("openALAudioOutputDeviceName", std::string());
    settings.openALSpatialConfig =
        json.value("openALSpatialConfig", OpenALSpatialConfig());
    // 诊断日志默认关闭，必须由用户显式开启。
    settings.renderProfileLogging = json.value("renderProfileLogging", false);
    settings.rtcDiagnosticLogging = json.value("rtcDiagnosticLogging", false);
    settings.collaborationViewportRenderMode =
        json.value("collaborationViewportRenderMode",
                   CollaborationViewportRenderMode::Filled);
    // 协作端点由子结构校验，视野模式仅控制本地渲染。
    settings.collaborationServer =
        json.value("collaborationServer", CollaborationServerSettings{});
    settings.autoUploadPgoProfiles = json.value("autoUploadPgoProfiles", false);
    // 旧版已存在上传开关即视为询问过授权，避免升级后重复弹窗。
    settings.pgoProfileUploadConsentAsked = json.value(
        "pgoProfileUploadConsentAsked", json.contains("autoUploadPgoProfiles"));
    // 尺度和滚动倍率使用产品基线回退，运行层再应用设备相关换算。
    settings.fontSizeMultiplier    = json.value("fontSizeMultiplier", 1.15f);
    settings.uiScaleMultiplier     = json.value("uiScaleMultiplier", 1.0f);
    settings.scrollSpeedMultiplier = json.value("scrollSpeedMultiplier", 4.0f);
    // 总线音量和静音状态分开恢复，静音不会丢失用户设置的增益。
    settings.globalVolume = json.value("globalVolume", 0.25f);
    settings.globalMuted  = json.value("globalMuted", false);
    settings.bgmGain      = json.value("bgmGain", 1.0f);
    settings.bgmGainMuted = json.value("bgmGainMuted", false);
    settings.sfxGain      = json.value("sfxGain", 1.0f);
    settings.sfxGainMuted = json.value("sfxGainMuted", false);
    // 交互音效拥有独立增益和静音开关，不继承打击音效总线状态。
    settings.interactionSfxGain = json.value("interactionSfxGain", 1.0f);
    settings.interactionSfxGainMuted =
        json.value("interactionSfxGainMuted", false);
    // 选择框外观与包含模式共同恢复，缺失时采用当前 UI 默认。
    settings.selectionMode =
        json.value("selectionMode", SelectionMode::Intersection);
    settings.marqueeThickness = json.value("marqueeThickness", 2.0f);
    settings.marqueeRounding  = json.value("marqueeRounding", 0.0f);
    settings.saveFormatPreference =
        json.value("saveFormatPreference", SaveFormatPreference::ForceMMM);
    // 自动保存与自动备份分开读取，任何一侧缺失都不会影响另一侧。
    settings.autoSave   = json.value("autoSave", AutoSaveConfig{});
    settings.autoBackup = json.value("autoBackup", AutoBackupConfig{});
    settings.autoAddStoreModeExtForMalodyExport =
        json.value("autoAddStoreModeExtForMalodyExport", false);
    settings.timeFormatPreference =
        json.value("timeFormatPreference", TimeFormatPreference::Seconds);
    // 文件选择历史缺失时使用当前目录，但不在反序列化阶段访问文件系统。
    settings.lastFilePickerPath =
        json.value("lastFilePickerPath", std::string("."));
    // 绘制和拖拽限制逐项读取，部分旧配置可以安全获得新字段默认值。
    settings.disableScrollAccelerationWhileDrawing =
        json.value("disableScrollAccelerationWhileDrawing", true);
    settings.disableVerticalObjectDrag =
        json.value("disableVerticalObjectDrag", false);
    settings.removeObjectsOnPolylinePath =
        json.value("removeObjectsOnPolylinePath", false);
    // 编辑能力开关只恢复偏好，不在配置层判断谱面格式是否支持。
    settings.enablePolylineEditing = json.value("enablePolylineEditing", true);
    settings.enableBmsEditing      = json.value("enableBmsEditing", true);
    settings.selectPastedObjects   = json.value("selectPastedObjects", false);
    // 粘贴时间基准与时间线 BPM 选择范围分别控制两种编辑语义。
    settings.copyPasteTimeBasis =
        json.value("copyPasteTimeBasis", CopyPasteTimeBasis::Timestamp);
    settings.timelineSelectionIncludesBpm =
        json.value("timelineSelectionIncludesBpm", false);
    // 旧配置没有测量工具分拍值时继承根级 beatDivisor 保持操作习惯。
    BpmMeasurementToolPreferences bpmMeasurementToolPreferencesFallback;
    bpmMeasurementToolPreferencesFallback.beatDivisor = settings.beatDivisor;
    settings.bpmMeasurementToolPreferences            = json.value(
        "bpmMeasurementToolPreferences", bpmMeasurementToolPreferencesFallback);
    // 光标和字体配置保持纯值读取，字体文件可用性由验证器另行检查。
    settings.softwareCursorConfig =
        json.value("softwareCursorConfig", SoftwareCursorConfig());
    settings.preferredAsciiFont =
        json.value("preferredAsciiFont", std::string("Default"));
    settings.preferredCjkFont =
        json.value("preferredCjkFont", std::string("Default"));
    // 播放交互与窗口开关采用当前功能默认，不依赖窗口实际存在状态。
    settings.stopPlaybackOnScroll = json.value("stopPlaybackOnScroll", false);
    settings.snapFloor            = json.value("snapFloor", false);
    settings.showTimelineWindow   = json.value("showTimelineWindow", true);
    // 当前 professionalMode 优先，缺失时读取旧 timelineProfessionalMode。
    settings.professionalMode =
        json.contains("professionalMode")
            ? json.value("professionalMode", false)
            : json.value("timelineProfessionalMode", false);
    settings.showPreviewWindow     = json.value("showPreviewWindow", true);
    settings.showAnnotationDetails = json.value("showAnnotationDetails", false);
    // 工具栏显示字段属于全局设置，但仍在统一对象中完成持久化。
    settings.showToolLabels = json.value("showToolLabels", false);
    settings.toolbarVisibility =
        json.value("toolbarVisibility", ToolbarVisibilityConfig{});
    settings.fixedToolWindow   = json.value("fixedToolWindow", true);
    settings.showManagerLabels = json.value("showManagerLabels", true);
    // 外观、调色板和快捷键子结构各自负责内部字段兼容。
    settings.aesthetics    = json.value("aesthetics", UIAestheticsConfig());
    settings.colorPalettes = json.value("colorPalettes", ColorPaletteConfig());
    settings.defaultColorPaletteSchemeName =
        json.value("defaultColorPaletteSchemeName",
                   std::string(COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID));
    settings.shortcutConfig = json.value("shortcutConfig", ShortcutConfig());
}

/// @brief 序列化编辑器配置根对象。
/// @param json 接收视觉设置、编辑设置和最近项目列表。
/// @param config 待保存的完整编辑器配置。
/// @note 根对象只负责组合子结构，不重复处理其内部迁移规则。
void to_json(nlohmann::json& json, const EditorConfig& config)
{
    // 三个顶层职责分开保存，允许项目设置只选取其中需要的部分。
    json = nlohmann::json{ { "visual", config.visual },
                           { "settings", config.settings },
                           { "recentProjects", config.recentProjects } };
}

/// @brief 从 JSON 恢复编辑器配置根对象。
/// @param json 用户配置文件中的根对象。
/// @param config 接收视觉、设置和最近项目数据。
/// @note 缺失顶层字段构造对应默认对象，支持从空配置启动。
void from_json(const nlohmann::json& json, EditorConfig& config)
{
    // 子对象通过各自 from_json 处理版本兼容，根层仅提供缺失默认值。
    config.visual   = json.value("visual", VisualConfig());
    config.settings = json.value("settings", EditorSettings());
    config.recentProjects =
        json.value("recentProjects", std::vector<std::string>());
}

}  // namespace MMM::Config
