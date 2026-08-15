#pragma once

#include "mmm/Metadata.h"
#include "mmm/annotation/BeatmapAnnotation.h"
#include "mmm/note/Flick.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Note.h"
#include "mmm/note/Polyline.h"
#include "mmm/sample/AudioSample.h"
#include "mmm/timing/Timing.h"
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace MMM
{

/// @brief 谱面加载诊断代码。
enum class BeatmapLoadDiagnosticCode {
    /// @brief 旧 MMM 已完成有损单音频迁移，但旁边仍有可重新导入的原始 Malody
    /// 文件。
    LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,

    /// @brief 自动采样的来源轨道不属于 BGM 区，加载时已迁移到首条 BGM
    /// 轨。
    AUDIO_SAMPLE_TRACK_RELOCATED,
};

/// @brief 谱面加载诊断级别。
enum class BeatmapLoadDiagnosticSeverity {
    BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_INFO,
    BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
    BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_ERROR,
};

/// @brief 加载谱面时产生、可由会话或界面消费的结构化诊断。
struct BeatmapLoadDiagnostic {
    /// @brief 便于调用方稳定分支处理的诊断代码。
    BeatmapLoadDiagnosticCode m_code{
        BeatmapLoadDiagnosticCode::LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE
    };

    /// @brief 诊断级别。
    BeatmapLoadDiagnosticSeverity m_severity{
        BeatmapLoadDiagnosticSeverity::BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING
    };

    /// @brief 面向用户的诊断说明。
    std::string m_message;

    /// @brief 与诊断关联的文件路径。
    std::filesystem::path m_relatedPath;
};

struct NoteData {
    /// @brief 所有普通物件
    std::deque<Note> notes;

    /// @brief 所有长条物件
    std::deque<Hold> holds;

    /// @brief 所有滑键物件
    std::deque<Flick> flicks;

    /// @brief 所有折线物件
    std::deque<Polyline> polylines;
};

class BeatMap
{
public:
    BeatMap();
    BeatMap(BeatMap&&)                 = default;
    BeatMap(const BeatMap&)            = delete;
    BeatMap& operator=(BeatMap&&)      = default;
    BeatMap& operator=(const BeatMap&) = delete;
    ~BeatMap();

    /**
     * @brief 从文件加载谱面
     * @param mapFilePath 谱面文件路径
     */
    static BeatMap loadFromFile(std::filesystem::path mapFilePath);

    /**
     * @brief 保存谱面到文件
     * @param mapFilePath 保存的目标路径
     * @return 是否保存成功
     */
    bool saveToFile(std::filesystem::path mapFilePath) const;

    /// @brief 同步物件引用表 (m_allNotes)
    void sync();

    /// @brief 所有物件引用
    std::vector<std::reference_wrapper<Note>> m_allNotes;

    /// @brief 所有物件数据
    NoteData m_noteData;

    /// @brief 所有时间线
    std::vector<Timing> m_timings;

    /// @brief 所有无需玩家操作即可自动播放的采样对象。
    std::deque<AudioSampleEvent> m_audioSamples;

    /// @brief 所有独立时间戳与物件批注。
    std::vector<BeatmapAnnotation> m_annotations;

    /// @brief 本次加载产生的结构化兼容诊断。
    std::vector<BeatmapLoadDiagnostic> m_loadDiagnostics;

    /// @brief 谱面基本元数据
    BaseMapMeta m_baseMapMetadata;

    /// @brief 所有谱面元数据
    MapMetadata m_metadata;
};

}  // namespace MMM
