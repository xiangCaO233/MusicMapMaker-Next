#pragma once

#include "mmm/project/AudioResource.h"
#include <cstddef>
#include <string>
#include <vector>

namespace MMM::Logic
{
/// @brief 谱面中音频资源引用的用途。
enum class BeatmapAudioReferenceKind {
    /// @brief 歌曲文件提示，用于识别主音轨。
    SongFileHint,
    /// @brief 玩家物件触发的音效绑定。
    NoteSampleBinding,
    /// @brief 按时间自动播放的采样事件。
    AudioSampleEvent,
};

/// @brief 一个可追溯到具体谱面和字段用途的音频资源引用。
struct BeatmapAudioReference {
    /// @brief 引用所在谱面的项目相对路径。
    std::string m_beatmapPath;

    /// @brief 谱面字段中保存的资源 ID 或路径。
    std::string m_audioReference;

    /// @brief 引用用途。
    BeatmapAudioReferenceKind m_kind{
        BeatmapAudioReferenceKind::AudioSampleEvent
    };
};

/// @brief 单个内存谱面在音频资源移动后的引用重映射结果。
struct BeatmapAudioReferenceRemapResult {
    /// @brief 匹配到目标资源的玩家物件绑定数量。
    std::size_t m_noteBindingReferenceCount{ 0U };

    /// @brief 匹配到目标资源的自动采样数量。
    std::size_t m_audioSampleReferenceCount{ 0U };

    /// @brief 匹配到目标资源的歌曲路径提示数量。
    std::size_t m_songFileHintReferenceCount{ 0U };

    /// @brief 实际从旧路径写法改成资源 ID 或新提示路径的字段数量。
    std::size_t m_changedReferenceCount{ 0U };

    /// @brief 谱面是否以任意受支持字段引用了目标资源。
    /// @return 至少存在一个匹配引用时返回 true。
    [[nodiscard]] bool referencesResource() const
    {
        // 匹配不等于改写：已使用稳定 ID 的引用也应参与资源占用判断。
        return m_noteBindingReferenceCount > 0U ||
               m_audioSampleReferenceCount > 0U ||
               m_songFileHintReferenceCount > 0U;
    }

    /// @brief 谱面字段是否实际发生了重写。
    /// @return 至少一个字段发生变化时返回 true。
    [[nodiscard]] bool changed() const { return m_changedReferenceCount > 0U; }
};

/// @brief 项目谱面中的音频资源 ID 事务重写结果。
struct ProjectBeatmapAudioIdRemapResult {
    /// @brief 全部相关谱面均完成提交。
    bool m_success{ false };

    /// @brief 实际改写过引用的谱面数量。
    std::size_t m_changedBeatmapCount{ 0U };

    /// @brief 失败时可直接展示的原因。
    std::string m_errorMessage;
};

/// @brief 保存前 song_file_hint 的选取来源。
enum class BeatmapSongFileHintSource {
    /// @brief 没有可解析的主音轨提示。
    None,
    /// @brief 沿用已有且可解析的提示。
    ExistingHint,
    /// @brief 从最早主音轨采样生成提示。
    EarliestMainSample,
};

/// @brief 保存前刷新 song_file_hint 的结果。
struct BeatmapSongFileHintUpdateResult {
    /// @brief 最终提示的选取来源。
    BeatmapSongFileHintSource m_source{ BeatmapSongFileHintSource::None };

    /// @brief 最终提示解析到的稳定资源 ID；没有提示时为空。
    std::string m_audioResourceId;

    /// @brief song_file_hint 或旧 main_audio_path 是否发生变化。
    bool m_changed{ false };
};

/// @brief 项目目录资源同步后的结果。
struct DirectorySyncResult {
    /// @brief 目录扫描是否成功并完成资源同步。
    bool m_scanSucceeded{ false };

    /// @brief 本次同步是否改变了项目资源列表或资源类型。
    bool m_changed{ false };

    /// @brief 新发现且需要登记到音频引擎的按需加载音效资源列表。
    std::vector<AudioResource> m_effectResourcesToRegister;
};

/// @brief 旧项目单主音轨字段迁移结果。
struct LegacyAudioMigrationResult {
    /// @brief 成功物化自动采样并写回的旧版 MMM 谱面数量。
    std::size_t m_migratedBeatmapCount{ 0 };

    /// @brief 无法定位资源、读取或写回的旧版 MMM 谱面路径。
    std::vector<std::string> m_failedBeatmapPaths;
};

}  // namespace MMM::Logic
