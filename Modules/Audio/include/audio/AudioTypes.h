#pragma once

#include "mmm/project/AudioTrackConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @file AudioTypes.h
/// @brief 音频控制、时间线导入和离线导出共用的值类型。
/// @details 保持枚举底层类型、字段次序和默认值，不持有播放器或后台任务。
/// DTO 独立于 AudioManager，调用方可按值构造快照而不传播管理器私有布局。

namespace MMM::Audio
{
/// @brief 由谱面加载路径提交的音乐暂停描述，单位为秒。
struct AudioTimelineStop {
    /// @brief 不包含暂停等待的音乐时间位置。
    double position{};
    /// @brief 按正常速度计算的暂停秒数。
    double duration{};
};

/**
 * @brief 播放状态
 */
enum class PlaybackStatus {
    /// @brief 已停止播放。
    Stopped,

    /// @brief 正在播放。
    Playing,

    /// @brief 已暂停播放。
    Paused
};

/// @brief 主时间线跳转时需要执行的清理级别。
enum class AudioSeekMode : std::uint8_t {
    Commit,      ///< 完整跳转，并清理拉伸器历史与已调度音效。
    ScrubUpdate  ///< 连续拖动的中间位置，仅更新无锁时间线 seek 邮箱。
};

/**
 * @brief EQ 频段预设
 */
enum class EQPreset {
    /// @brief 不启用 EQ。
    None,

    /// @brief 十段 EQ：31、62、125、250、500、1k、2k、4k、8k、16k。
    TenBand,

    /// @brief 十五段
    /// EQ：25、40、63、100、160、250、400、630、1k、1.6k、2.5k、4k、6.3k、10k、16k。
    FifteenBand
};

/** @brief 双声道输出模式。 */
enum class MixerChannelMode {
    Stereo,           ///< 保持原始立体声输出。
    MuteLeft,         ///< 静音左声道，仅保留右声道。
    MuteRight,        ///< 静音右声道，仅保留左声道。
    CopyLeftToRight,  ///< 将左声道复制到右声道，两侧都播放左声道。
    CopyRightToLeft   ///< 将右声道复制到左声道，两侧都播放右声道。
};

/// @brief 交给 AudioManager 准备的单个自动采样事件。
struct AudioTimelineLoadEvent {
    /// @brief 谱面内稳定且唯一的采样物件标识。
    std::uint64_t eventId{ 0U };

    /// @brief 项目音频资源的稳定标识。
    std::string resourceKey;

    /// @brief 音频资源的绝对文件路径。
    std::string filePath;

    /// @brief 已合并物件 offset 的实际起播时间，单位为秒，允许为负。
    double effectiveStartSeconds{ 0.0 };

    /// @brief 相对玩家轨道区的零基 BGM 轨道索引。
    std::uint32_t bgmTrackIndex{ 0U };

    /// @brief 采样物件自身的线性音量。
    float eventVolume{ 1.0F };

    /// @brief 被引用音频资源的持久化配置。
    AudioTrackConfig resourceConfig;
};

/// @brief 音频时间线加载诊断类型。
enum class AudioTimelineLoadDiagnosticCode : std::uint8_t {
    /// @brief AudioManager 尚未初始化，无法构造音频图。
    AudioSystemUnavailable,

    /// @brief 文件路径为空、无法探测或完整解码结果为空。
    MissingResource,

    /// @brief 起播时间不是有限数值，已按零秒恢复。
    InvalidStartTime
};

/// @brief 单个时间线加载问题及其来源。
struct AudioTimelineLoadDiagnostic {
    /// @brief 诊断类型。
    AudioTimelineLoadDiagnosticCode code{
        AudioTimelineLoadDiagnosticCode::MissingResource
    };

    /// @brief 相关采样物件标识。
    std::uint64_t eventId{ 0U };

    /// @brief 相关项目音频资源标识。
    std::string resourceKey;

    /// @brief 相关资源文件路径。
    std::string filePath;

    /// @brief 可直接展示给用户的诊断说明。
    std::string message;
};

/// @brief 构造复合音频时间线的结果。
struct AudioTimelineLoadResult {
    /// @brief 音频图是否已成功替换；即使没有有效片段也可为 true。
    bool success{ false };

    /// @brief 第一阶段提交给解码池的唯一非空文件路径数量。
    std::size_t requestedSourceCount{ 0U };

    /// @brief 第二阶段成功建立的唯一资源 DSP 结果数量。
    std::size_t preparedResourceCount{ 0U };

    /// @brief 成功载入调度表的采样物件数量。
    std::size_t loadedClipCount{ 0U };

    /// @brief 本次发布的时间线调度代次；未发布时为零。
    std::uint64_t scheduleGeneration{ 0U };

    /// @brief 因资源缺失而未进入调度表的采样物件数量。
    std::size_t missingClipCount{ 0U };

    /// @brief 可展示给用户或写入导入报告的非致命诊断。
    std::vector<AudioTimelineLoadDiagnostic> diagnostics;
};

/// @brief 音频输出设备列表项。
struct AudioOutputDevice {
    /// @brief 后端报告的设备名称；为空时表示系统默认设备。
    std::string name;

    /// @brief 是否为“系统默认设备”占位项。
    bool isDefault{ false };
};

}  // namespace MMM::Audio
