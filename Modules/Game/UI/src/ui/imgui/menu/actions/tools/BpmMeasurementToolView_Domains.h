#pragma once

#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include "audio/AudioMarkerService.h"
#include "ui/imgui/menu/actions/tools/BpmAutoDetector.h"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

namespace MMM::UI
{
/// @brief 频谱贴图分块数据。
struct BpmMeasurementToolView::TextureChunkData {
    /// @brief RGBA 像素数据。
    std::vector<unsigned char> pixels;

    /// @brief 分块宽度，单位为像素。
    uint32_t width{ 0 };

    /// @brief 分块高度，单位为像素。
    uint32_t height{ 0 };
};

/// @brief 后台音频分析结果。
struct BpmMeasurementToolView::AnalysisResult {
    /// @brief 波形采样点时间，单位为秒。
    std::vector<double> waveTimes;

    /// @brief 波形下包络。
    std::vector<double> waveMin;

    /// @brief 波形上包络。
    std::vector<double> waveMax;

    /// @brief 待上传的频谱贴图分块。
    std::vector<TextureChunkData> spectrumChunks;

    /// @brief 音频总时长，单位为秒。
    double duration{ 0.0 };

    /// @brief 频谱时间分辨率，单位为段/秒。
    double spectrumSegmentsPerSecond{ 0.0 };

    /// @brief 频谱总段数。
    int spectrumSegmentCount{ 0 };

    /// @brief 频谱频率 bin 数。
    int spectrumBinCount{ 0 };

    /// @brief 本次分析是否请求自动 BPM/offset 测量。
    bool autoTimingRequested{ false };

    /// @brief 自动 BPM/offset 测量结果。
    std::optional<BpmAutoTimingResult> autoTimingResult;

    /// @brief 本次分析是否失败。
    bool failed{ false };

    /// @brief 探测发现路径不可读或已经消失。
    bool sourceMissing{ false };

    /// @brief 新音轨的内嵌标记读取快照，普通重新分析不覆盖手工测量。
    std::optional<Audio::AudioMarkerReadResult> markers;
};

/// @brief 一次用户操作前后的 BPM 工具本地状态快照。
struct BpmMeasurementToolView::MeasurementHistorySnapshot {
    /// @brief 当前音轨身份，用于阻止跨音轨还原。
    std::string audioTrackId;
    /// @brief 用户可编辑 BPM 段落完整列表。
    std::vector<BpmTimingSegment> timingSegments;
    /// @brief 与第一段同步的兼容 BPM 字段。
    double bpm{ 120.0 };
    /// @brief 与 BPM 对应的单拍秒数。
    double beatLengthSeconds{ 0.5 };
    /// @brief 第一段首拍时间。
    double firstBeatTime{ 0.0 };
};

/// @brief 本地手势历史只随视图与已选音轨存活。
struct BpmMeasurementToolView::MeasurementHistory {
    /// @brief 一次可撤销手工操作的前后状态与副作用标识。
    struct MeasurementHistoryEntry {
        /// @brief 操作前状态。
        MeasurementHistorySnapshot before;
        /// @brief 操作后状态。
        MeasurementHistorySnapshot after;
    };

    /// @brief 已提交的用户手工操作历史，尾部为最近一次。
    std::vector<MeasurementHistoryEntry> m_measurementUndoHistory;
    /// @brief 最近撤销的操作历史，尾部优先重做。
    std::vector<MeasurementHistoryEntry> m_measurementRedoHistory;
    /// @brief 当前连续手势开始时的快照；空值表示未记录手势。
    std::optional<MeasurementHistorySnapshot> m_measurementGestureBefore;
    /// @brief 还原历史期间屏蔽递归记录。
    bool m_restoringMeasurementHistory{ false };
};

/// @brief 独占标记文件任务及其反馈。后台任务只借用自己的路径和值快照。
struct BpmMeasurementToolView::MarkerFileOperations {
    /// @brief 从音频恢复的命名章节，与 BPM 段保持独立。
    std::vector<Audio::AudioChapter> m_audioChapters;
    /// @brief 最近读取标记的音轨身份，防止重新分析覆盖手工修改。
    std::string m_markerImportKey;
    /// @brief 离线导出 future 只持有结果，任务不访问视图生命周期。
    std::future<Audio::AudioMarkerExportResult> m_markerExportFuture;
    /// @brief 导出原格式、WAV、MP3 的下拉选择。
    int m_markerExportFormat{ 0 };
    /// @brief 是否为所有整拍额外生成外部可见的标记。
    bool m_exportBeatMarkers{ false };
    /// @brief 独立于分析状态的标记读取、写入反馈。
    std::string m_markerStatus;
    /// @brief 独立段落文件结果，后台任务只持有路径和数值快照。
    std::future<Audio::AudioMarkerReadResult> m_bpmFileImport;
    /// @brief 导出的写入或发布错误，空字符串表示成功。
    std::future<std::string> m_bpmFileExport;
    /// @brief 导入发起时的音频身份，用来丢弃切歌后返回的结果。
    std::string m_bpmFileImportAudioKey;
    /// @brief 原生选择器存活令牌，视图析构后回调不能读取视图状态。
    std::shared_ptr<bool> m_bpmFilePickerLifetime;
};

/// @brief 独占一个分析工作者和结果槽。视图只在析构边界阻塞等待工作者。
struct BpmMeasurementToolView::AnalysisJob {
    /// @brief 析构前撤销交付资格，并保证任务不会再借用视图。
    /// @warning 低频关闭路径，可等待后台退出。
    void stopAndWait();

    /// @brief 回收已经结束的工作者，无等待。取消或换源后丢弃旧快照。
    /// @warning 每帧可调用，任务未完成时只检查 future。
    bool reclaimCompleted(const std::string& selection);

    /// @brief 请求发出时冻结的分析来源与参数，仅在 UI 线程访问。
    struct AnalysisRequest {
        /// @brief 已按项目根解析的音频路径。
        std::filesystem::path path;
        /// @brief 选择身份，用于丢弃换曲之后才就绪的结果。
        std::string key;
        /// @brief 当前配置的频谱分辨率。
        Config::SpectrumDetailProfile spectrumProfile;
        /// @brief 是否在分析后估算 BPM 和首拍。
        bool autoMeasure;
    };

    /// @brief 等待独占结果槽的最新请求，连续切换覆盖旧请求。
    std::optional<AnalysisRequest> m_analysisRequest;

    /// @brief 当前后台任务的身份，只由 UI 在派发和消费时访问。
    std::string m_activeAnalysisKey;

    /// @brief 后台分析在线程池中的任务句柄。
    /// @warning 生命周期路径：仅由重新分析或窗口销毁触发写入；任务体不在 UI
    /// 热路径执行。
    std::future<void> m_analysisFuture;

    /// @brief 后台分析停止请求源。
    /// @warning 跨线程停止信号：UI
    /// 线程低频写入，后台分析任务分段读取，只承载取消标志。
    std::stop_source m_analysisStopSource;

    /// @brief 后台分析结果互斥锁。
    mutable std::mutex m_pendingResultMutex;

    /// @brief 待消费的后台分析结果。
    std::optional<AnalysisResult> m_pendingResult;

    /// @brief 是否正在后台分析。
    /// @warning UI 热路径原子：UI
    /// 每帧读取，后台线程写入；仅承载分析状态脏位，使用 relaxed 顺序即可。
    std::atomic<bool> m_analysisRunning{ false };

    /// @brief 后台分析是否完成。
    /// @warning UI 热路径原子：UI
    /// 每帧读取，后台线程写入；仅承载结果可消费标记，使用 acquire/release。
    std::atomic<bool> m_analysisFinished{ false };

    /// @brief 后台分析进度。
    /// @warning UI 热路径原子：UI
    /// 每帧读取，后台线程写入；只用于进度显示，不参与同步数据所有权。
    std::atomic<float> m_analysisProgress{ 0.0f };
};

}  // namespace MMM::UI
