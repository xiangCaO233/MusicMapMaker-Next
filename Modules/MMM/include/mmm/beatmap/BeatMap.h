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
#include <nlohmann/json_fwd.hpp>
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

    /// @brief 构建 MC 文档，并报告必须同步添加到主音频开头的静音长度。
    /// @param document 成功时接收含初始参考红绿线的完整 MC 文档。
    /// @param audioLeadInMilliseconds 主音频需要增加的前导静音，单位毫秒。
    /// @return 谱面和 timing 可由 MC 表达时返回 true。
    /// @note 与文件保存共用曲线展开和回卷入口，不修改来源谱面或音频。
    /// @warning 低频导出路径：完整构建 JSON，不用于逻辑或渲染循环。
    [[nodiscard]] bool buildMalodyExportDocument(
        nlohmann::json& document, double& audioLeadInMilliseconds) const;

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

private:
    /// @brief 共享格式准备入口；MC 文档模式只构建 JSON，不执行文件写入。
    /// @param mapFilePath 目标格式与普通保存路径。
    /// @param malodyDocument 非空时接收 MC 文档，为空时执行标准文件保存。
    /// @param audioLeadInMilliseconds MC 文档模式的前导静音输出。
    /// @return 格式准备及目标写出或文档构建成功时返回 true。
    /// @warning 低频路径：曲线展开可以创建导出副本，不在热路径调用。
    bool saveToFileInternal(std::filesystem::path mapFilePath,
                            nlohmann::json*       malodyDocument,
                            double* audioLeadInMilliseconds) const;
};

}  // namespace MMM
