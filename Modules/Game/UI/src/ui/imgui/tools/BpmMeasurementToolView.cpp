#include "ui/imgui/tools/BpmMeasurementToolView.h"
#include "audio/AudioManager.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fftw3.h>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <numeric>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

namespace MMM::UI
{
namespace
{
/// @brief 播放指针三角手柄半宽，单位为像素。
constexpr float PLAYBACK_CURSOR_HANDLE_HALF_WIDTH = 7.0f;

/// @brief 播放指针三角手柄高度，单位为像素。
constexpr float PLAYBACK_CURSOR_HANDLE_HEIGHT = 11.0f;

/// @brief 整拍线三角手柄半宽，单位为像素。
constexpr float BEAT_MARKER_HANDLE_HALF_WIDTH = 6.0f;

/// @brief 整拍线三角手柄高度，单位为像素。
constexpr float BEAT_MARKER_HANDLE_HEIGHT = 10.0f;

/// @brief BPM 工具普通拍节拍器音效 key。
constexpr const char* BPM_METRONOME_LOW_KEY = "metronome.beat_low";

/// @brief BPM 工具四拍重音节拍器音效 key。
constexpr const char* BPM_METRONOME_HIGH_KEY = "metronome.downbeat_high";

/// @brief BPM 工具节拍器播放时的额外音量倍率。
constexpr float BPM_METRONOME_VOLUME_FACTOR = 2.0f;

/// @brief 节拍器音效提前调度窗口，单位为秒。
constexpr double BPM_METRONOME_SCHEDULE_LOOKAHEAD_SECONDS = 0.2;

/// @brief 已越过拍点但仍允许立即补响的时间窗口，单位为秒。
constexpr double BPM_METRONOME_PAST_TRIGGER_WINDOW = 0.05;

/// @brief 单帧最多触发的节拍器音效数量，避免跳转后爆发播放。
constexpr int BPM_METRONOME_MAX_TRIGGERED_PER_FRAME = 8;

/// @brief 获取 FFTW 计划互斥锁，保护全局 planner 状态。
std::mutex& fftwPlanMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// @brief 生成音频资源在下拉框中的显示文本。
/// @param resource 音频资源。
/// @return 适合 UI 展示的标签。
std::string makeAudioResourceLabel(const AudioResource& resource)
{
    const char* typeText =
        resource.m_type == AudioTrackType::Main ? "Main" : "Effect";
    return resource.m_id + " [" + typeText + "] - " + resource.m_path;
}

/// @brief ImGui 分拍线样式。
struct BeatLineStyle {
    /// @brief ImGui RGBA 颜色。
    ImU32 color{ IM_COL32(255, 255, 255, 255) };

    /// @brief 线宽，单位为像素。
    float width{ 2.0f };
};

/// @brief 音频控制器同款同步播放时间快照。
struct PlaybackTimelineState {
    /// @brief 音频时间，单位为秒。
    double audioTime{ 0.0 };

    /// @brief 叠加视觉偏移后的视觉时间，单位为秒。
    double visualTime{ 0.0 };

    /// @brief 音频总时长，单位为秒。
    double totalTime{ 0.0 };

    /// @brief 当前视觉偏移，单位为秒。
    double visualOffset{ 0.0 };

    /// @brief 当前逻辑播放状态。
    bool isPlaying{ false };
};

/// @brief 将皮肤颜色转换为 ImGui 颜色。
/// @param color 皮肤颜色。
/// @param alphaScale 额外透明度倍率。
/// @return ImGui RGBA 颜色。
ImU32 toImColor(const Config::Color& color, float alphaScale)
{
    return IM_COL32(static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(
                        std::clamp(color.a * alphaScale, 0.0f, 1.0f) * 255.0f));
}

/// @brief 获取 BPM 工具允许的最早首拍时间。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @return 允许首拍略早于音频起点，最多早一拍。
double firstBeatMinSeconds(double beatLengthSeconds)
{
    return -std::max(0.0, beatLengthSeconds);
}

/// @brief 将首拍时间限制到 BPM 工具可编辑范围。
/// @param firstBeatTime 首拍时间，单位为秒。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @param canvasDuration 当前音频画布时长，单位为秒。
/// @return 限制后的首拍时间。
double clampFirstBeatTime(double firstBeatTime, double beatLengthSeconds,
                          double canvasDuration)
{
    return std::clamp<double>(firstBeatTime,
                              firstBeatMinSeconds(beatLengthSeconds),
                              std::max(0.0, canvasDuration));
}

/// @brief 将毫秒残差格式化为相对拍长的近似分数。
/// @param inaccuracyMs 残差 RMS，单位为毫秒。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @return 形如 1/64 的拍内分数。
std::string formatBeatFractionInaccuracy(double inaccuracyMs,
                                         double beatLengthSeconds)
{
    const double beatMs = beatLengthSeconds * 1000.0;
    if ( !(beatMs > 0.0) || !std::isfinite(beatMs) ||
         !std::isfinite(inaccuracyMs) ) {
        return "0";
    }

    const double fraction = std::max(0.0, inaccuracyMs / beatMs);
    constexpr std::array<int, 15> denominators{ 1,  2,  3,  4,  6,  8,   12, 16,
                                                24, 32, 48, 64, 96, 128, 192 };

    int    bestNumerator   = 0;
    int    bestDenominator = denominators.back();
    double bestError       = std::numeric_limits<double>::infinity();
    for ( int denominator : denominators ) {
        const int numerator =
            static_cast<int>(std::max(0.0, std::round(fraction * denominator)));
        const double approx =
            static_cast<double>(numerator) / static_cast<double>(denominator);
        const double error = std::abs(approx - fraction);
        if ( error < bestError ) {
            bestError       = error;
            bestNumerator   = numerator;
            bestDenominator = denominator;
        }
    }

    if ( bestNumerator <= 0 ) {
        return fraction > 0.0 ? fmt::format("<1/{}", denominators.back()) : "0";
    }

    const int divisor = std::gcd(bestNumerator, bestDenominator);
    return fmt::format(
        "{}/{}", bestNumerator / divisor, bestDenominator / divisor);
}

/// @brief 按主画布规则查询指定分母的分拍线样式。
/// @param denominator 分拍分母。
/// @return 分拍线颜色和线宽。
BeatLineStyle getBeatLineStyle(int denominator)
{
    if ( denominator <= 0 ) {
        denominator = 1;
    }

    auto&         skin  = Config::SkinManager::instance();
    std::string   key   = "beat_lines.beat_" + std::to_string(denominator);
    Config::Color color = skin.getColor(key);
    if ( color.r == 1.0f && color.g == 0.0f && color.b == 1.0f &&
         color.a == 1.0f ) {
        color = skin.getColor("beat_lines.default");
        key   = "beat_lines_width.default";
    } else {
        key = "beat_lines_width.beat_" + std::to_string(denominator);
    }

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return { toImColor(color, visual.beatLineAlpha),
             skin.getValue(key,
                           skin.getValue("beat_lines_width.default", 2.0f)) };
}

/// @brief 按音频控制器的方式读取逻辑层同步后的播放时间。
/// @return 已应用视觉偏移和亚帧补偿的播放时间。
/// @warning UI
/// 热路径/共享指针：每帧读取同步缓冲区快照；getSyncBuffer 返回 shared_ptr
/// 用于保证画布关闭时快照生命周期，与 AudioWaveformView/AudioSpectrumView
/// 保持一致。
PlaybackTimelineState readPlaybackTimelineState()
{
    auto&                 audioManager = Audio::AudioManager::instance();
    PlaybackTimelineState state;
    state.visualOffset = Config::AppConfig::instance()
                             .getVisualConfig()
                             .getEffectiveVisualOffset();
    state.audioTime    = audioManager.getCurrentTime();
    state.visualTime   = state.audioTime + state.visualOffset;
    state.totalTime    = audioManager.getTotalTime();
    state.isPlaying =
        audioManager.getStatus() == Audio::PlaybackStatus::Playing;

    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    auto syncBuffer = Logic::EditorEngine::instance().getSyncBuffer(
        activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
    if ( !syncBuffer ) {
        return state;
    }

    auto snapshot = syncBuffer->getReadingSnapshot();
    if ( !snapshot ) {
        return state;
    }

    state.visualTime = snapshot->currentTime;
    state.audioTime  = state.visualTime - state.visualOffset;
    state.isPlaying  = snapshot->isPlaying;

    if ( !snapshot->isPreviewDragging && snapshot->isPlaying &&
         snapshot->snapshotSysTime > 0.0 ) {
        const double now =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const double dt = now - snapshot->snapshotSysTime;
        if ( dt > 0.0 && dt < 0.1 ) {
            state.visualTime += dt * snapshot->playbackSpeed;
            state.audioTime += dt * snapshot->playbackSpeed;
        }
    }

    return state;
}

}  // namespace

/// @brief 构造 BPM 测量工具窗口。
BpmMeasurementToolView::BpmMeasurementToolView(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    m_beatDivisor = std::clamp(
        Config::AppConfig::instance().getEditorSettings().beatDivisor, 1, 64);
    m_statusText = TR("ui.tools.bpm_measure.select_track").data();
}

/// @brief 销毁窗口并等待后台分析任务和 GPU 资源释放。
BpmMeasurementToolView::~BpmMeasurementToolView()
{
    stopAnalysisWorker();

    auto context = Graphic::VKContext::get();
    if ( context ) {
        (void)context->get().getLogicalDevice().waitIdle();
    }
    m_spectrumTextures.clear();
}

/// @brief 打开窗口并选中指定项目音频轨道。
/// @param audioTrackId 项目内音频资源 ID；为空时仅打开窗口。
void BpmMeasurementToolView::openWithAudioTrack(const std::string& audioTrackId)
{
    m_isOpen = true;
    (void)ensureMetronomeSoundEffects();

    if ( audioTrackId.empty() ) {
        return;
    }

    const bool selectionChanged = m_selectedAudioTrackId != audioTrackId;
    const bool needsAnalysis =
        selectionChanged ||
        (m_waveTimes.empty() &&
         !m_analysisRunning.load(std::memory_order_relaxed));
    if ( selectionChanged ) {
        m_selectedAudioTrackId = audioTrackId;
        if ( auto resource = selectedAudioResource() ) {
            m_playbackSpeed =
                std::clamp<double>(resource->m_config.playbackSpeed, 0.25, 2.0);
        }
    }
    if ( needsAnalysis ) {
        requestAnalyzeSelectedTrack();
    }
}

/// @brief 打开窗口并对指定或默认项目音频轨道执行自动 BPM 测量。
/// @param audioTrackId 项目内音频资源 ID；为空时选择默认主音轨。
void BpmMeasurementToolView::openWithAutoMeasurement(
    const std::string& audioTrackId)
{
    m_isOpen = true;
    (void)ensureMetronomeSoundEffects();

    const std::string targetAudioTrackId =
        audioTrackId.empty() ? defaultAudioTrackId() : audioTrackId;
    if ( targetAudioTrackId.empty() ) {
        m_statusText = TR("ui.tools.bpm_measure.no_audio").data();
        return;
    }

    m_selectedAudioTrackId = targetAudioTrackId;
    if ( auto resource = selectedAudioResource() ) {
        m_playbackSpeed =
            std::clamp<double>(resource->m_config.playbackSpeed, 0.25, 2.0);
    }
    requestAnalyzeSelectedTrack(true);
}

/// @brief 更新并绘制 BPM 测量工具 UI。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI 热路径：每帧执行；不得在此处扫描文件系统、重新解码整段音频或创建
/// FFT 计划。
void BpmMeasurementToolView::update(UIManager* sourceManager)
{
    (void)sourceManager;
    consumePendingAnalysis();
    updateMetronomePlayback();

    ImGui::SetNextWindowSize(ImVec2(1120.0f, 720.0f), ImGuiCond_FirstUseEver);
    std::string windowTitle =
        std::string(TR("ui.tools.bpm_measure.title").data()) +
        "###BpmMeasurementTool";
    LayoutContext layoutContext(
        m_layoutCtx,
        windowTitle,
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
        &m_isOpen);

    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float controlsWidth =
        std::clamp(contentWidth * 0.30f, 320.0f, 420.0f);
    if ( ImGui::BeginTable("##BpmMeasureLayout",
                           2,
                           ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_BordersInnerV) ) {
        ImGui::TableSetupColumn(
            "##Analysis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(
            "##Controls", ImGuiTableColumnFlags_WidthFixed, controlsWidth);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if ( ImGui::BeginChild("##BpmMeasureAnalysisChild",
                               ImVec2(0.0f, 0.0f),
                               ImGuiChildFlags_None,
                               ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse) ) {
            renderAnalysisPanel();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if ( ImGui::BeginChild("##BpmMeasureControlsChild",
                               ImVec2(0.0f, 0.0f),
                               ImGuiChildFlags_None,
                               ImGuiWindowFlags_AlwaysVerticalScrollbar) ) {
            renderControlPanel();
        }
        ImGui::EndChild();

        ImGui::EndTable();
    }

    renderAutoApplyOffsetPopup();
    renderApplyTimingPopup();
}

/// @brief 判断频谱纹理是否需要上传。
/// @warning 渲染准备热路径：每帧查询；只读取低频变更脏位。
bool BpmMeasurementToolView::needReload()
{
    return m_texturesNeedReload;
}

/// @brief 上传后台分析生成的频谱纹理。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传命令池。
/// @param queue 上传队列。
/// @warning 低频资源准备路径：可能等待 GPU
/// 空闲并上传纹理，只能由音轨切换或重新分析触发。
void BpmMeasurementToolView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                            vk::Device&         logicalDevice,
                                            vk::CommandPool&    cmdPool,
                                            vk::Queue&          queue)
{
    if ( !m_texturesNeedReload ) {
        return;
    }

    if ( !m_spectrumTextureReloadStarted ) {
        (void)logicalDevice.waitIdle();
        m_spectrumTextures.clear();
        m_spectrumTextures.reserve(m_pendingSpectrumChunks.size());
        m_nextSpectrumChunkUploadIndex = 0;
        m_spectrumTextureReloadStarted = true;
    }

    constexpr size_t MAX_UPLOAD_CHUNKS_PER_FRAME = 1;
    size_t           uploadedThisFrame           = 0;
    while ( m_nextSpectrumChunkUploadIndex < m_pendingSpectrumChunks.size() &&
            uploadedThisFrame < MAX_UPLOAD_CHUNKS_PER_FRAME ) {
        const auto& chunk =
            m_pendingSpectrumChunks[m_nextSpectrumChunkUploadIndex];
        ++m_nextSpectrumChunkUploadIndex;
        ++uploadedThisFrame;

        if ( chunk.pixels.empty() || chunk.width == 0 || chunk.height == 0 ) {
            continue;
        }

        m_spectrumTextures.push_back(
            std::make_unique<Graphic::VKTexture>(chunk.pixels.data(),
                                                 chunk.width,
                                                 chunk.height,
                                                 physicalDevice,
                                                 logicalDevice,
                                                 cmdPool,
                                                 queue));
    }

    if ( m_nextSpectrumChunkUploadIndex >= m_pendingSpectrumChunks.size() ) {
        m_pendingSpectrumChunks.clear();
        m_nextSpectrumChunkUploadIndex = 0;
        m_spectrumTextureReloadStarted = false;
        m_texturesNeedReload           = false;
    }
}

/// @brief 尝试消费后台分析结果。
void BpmMeasurementToolView::consumePendingAnalysis()
{
    if ( !m_analysisFinished.load(std::memory_order_acquire) ) {
        return;
    }

    std::optional<AnalysisResult> result;
    {
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        if ( m_pendingResult ) {
            result = std::move(m_pendingResult);
            m_pendingResult.reset();
        }
    }

    if ( !result ) {
        m_analysisFinished.store(false, std::memory_order_release);
        return;
    }

    m_waveTimes = std::move(result->waveTimes);
    m_waveCanvasTimes.clear();
    m_waveCanvasTimesOffset        = std::numeric_limits<double>::quiet_NaN();
    m_waveMin                      = std::move(result->waveMin);
    m_waveMax                      = std::move(result->waveMax);
    m_pendingSpectrumChunks        = std::move(result->spectrumChunks);
    m_duration                     = result->duration;
    m_spectrumSegmentsPerSecond    = result->spectrumSegmentsPerSecond;
    m_spectrumSegmentCount         = result->spectrumSegmentCount;
    m_spectrumBinCount             = result->spectrumBinCount;
    m_nextSpectrumChunkUploadIndex = 0;
    m_spectrumTextureReloadStarted = false;
    m_texturesNeedReload           = true;

    if ( result->failed ) {
        m_statusText = result->autoTimingRequested
                           ? TR("ui.tools.bpm_measure.auto_failed").data()
                           : TR("ui.tools.bpm_measure.load_failed").data();
        m_analysisFinished.store(false, std::memory_order_release);
        return;
    }

    if ( result->autoTimingRequested ) {
        if ( result->autoTimingResult ) {
            const auto& autoTiming = *result->autoTimingResult;
            m_bpm                  = std::clamp(autoTiming.bpm, 1.0, 999.0);
            m_beatLengthSeconds    = 60.0 / m_bpm;
            m_firstBeatTime = clampFirstBeatTime(autoTiming.offsetMs / 1000.0,
                                                 m_beatLengthSeconds,
                                                 playbackCanvasDuration());
            if ( m_timingSegments.empty() ) {
                m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
            } else {
                m_timingSegments.front().timestampSeconds = m_firstBeatTime;
                m_timingSegments.front().bpm              = m_bpm;
            }
            normalizeTimingSegments();
            m_viewCenter = std::clamp<double>(
                m_firstBeatTime, 0.0, std::max(0.0, playbackCanvasDuration()));
            resetMetronomeScheduler(m_viewCenter);
            const std::string inaccuracyFraction = formatBeatFractionInaccuracy(
                autoTiming.alignmentInaccuracyMs, m_beatLengthSeconds);
            m_statusText = TR_FMT("ui.tools.bpm_measure.auto_ready",
                                  m_bpm,
                                  autoTiming.offsetMs,
                                  inaccuracyFraction,
                                  autoTiming.rawBpm,
                                  autoTiming.signature,
                                  autoTiming.division);
            m_shouldOpenAutoApplyPopup = true;
        } else {
            m_statusText = TR("ui.tools.bpm_measure.auto_failed").data();
        }
    } else {
        m_statusText = TR("ui.tools.bpm_measure.ready").data();
    }
    m_analysisFinished.store(false, std::memory_order_release);
}

/// @brief 确保至少存在一个 BPM 段落，并同步旧单段字段。
void BpmMeasurementToolView::ensureTimingSegments()
{
    if ( m_timingSegments.empty() ) {
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
        normalizeTimingSegments();
    }
}

/// @brief 归一化 BPM 段落列表，保持按时间排序且数值有效。
void BpmMeasurementToolView::normalizeTimingSegments()
{
    const double canvasDuration = playbackCanvasDuration();
    std::erase_if(m_timingSegments, [](const auto& segment) {
        return !std::isfinite(segment.timestampSeconds) ||
               !std::isfinite(segment.bpm) || segment.bpm <= 0.0;
    });

    if ( m_timingSegments.empty() ) {
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
    }

    for ( auto& segment : m_timingSegments ) {
        segment.bpm = std::clamp(segment.bpm, 1.0, 999.0);
        segment.timestampSeconds =
            std::clamp(segment.timestampSeconds,
                       firstBeatMinSeconds(60.0 / segment.bpm),
                       std::max(0.0, canvasDuration));
    }

    std::stable_sort(m_timingSegments.begin(),
                     m_timingSegments.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.timestampSeconds < rhs.timestampSeconds;
                     });

    std::vector<BpmTimingSegment> normalized;
    normalized.reserve(m_timingSegments.size());
    for ( const auto& segment : m_timingSegments ) {
        if ( !normalized.empty() &&
             std::abs(normalized.back().timestampSeconds -
                      segment.timestampSeconds) < 1e-6 ) {
            normalized.back() = segment;
            continue;
        }
        normalized.push_back(segment);
    }
    m_timingSegments = std::move(normalized);
    syncPrimaryTimingFieldsFromSegments();
}

/// @brief 从第一段同步兼容旧绘制/输入路径的字段。
void BpmMeasurementToolView::syncPrimaryTimingFieldsFromSegments()
{
    if ( m_timingSegments.empty() ) {
        return;
    }
    m_firstBeatTime     = m_timingSegments.front().timestampSeconds;
    m_bpm               = std::clamp(m_timingSegments.front().bpm, 1.0, 999.0);
    m_beatLengthSeconds = 60.0 / m_bpm;
}

/// @brief 将兼容旧输入路径的字段写回第一段。
void BpmMeasurementToolView::syncPrimaryTimingFieldsToSegments()
{
    if ( m_timingSegments.empty() ) {
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
    } else {
        m_timingSegments.front().timestampSeconds = m_firstBeatTime;
        m_timingSegments.front().bpm              = m_bpm;
    }
    normalizeTimingSegments();
}

/// @brief 查找指定时间所在的 BPM 段落索引。
/// @param timeSeconds 查询时间，单位为秒。
/// @return 段落索引。
std::size_t BpmMeasurementToolView::findSegmentIndexForTime(
    double timeSeconds) const
{
    if ( m_timingSegments.empty() ) {
        return 0;
    }

    std::size_t result = 0;
    for ( std::size_t i = 0; i < m_timingSegments.size(); ++i ) {
        if ( timeSeconds + 1e-9 < m_timingSegments[i].timestampSeconds ) {
            break;
        }
        result = i;
    }
    return result;
}

/// @brief 获取段落拍长。
/// @param segmentIndex 段落索引。
/// @return 单拍时长，单位为秒。
double BpmMeasurementToolView::segmentBeatLengthSeconds(
    std::size_t segmentIndex) const
{
    if ( m_timingSegments.empty() ) {
        return m_beatLengthSeconds;
    }
    const auto& segment =
        m_timingSegments[std::min(segmentIndex, m_timingSegments.size() - 1)];
    return 60.0 / std::clamp(segment.bpm, 1.0, 999.0);
}

/// @brief 将当前段落列表转换为可写入谱面的 BPM Timing 列表。
std::vector<::MMM::Timing> BpmMeasurementToolView::makeMeasuredTimings() const
{
    std::vector<::MMM::Timing> timings;
    timings.reserve(m_timingSegments.size());
    for ( const auto& segment : m_timingSegments ) {
        const double  bpm = std::clamp(segment.bpm, 1.0, 999.0);
        ::MMM::Timing timing;
        timing.m_timestamp = std::max(0.0, segment.timestampSeconds) * 1000.0;
        timing.m_timingEffect          = ::MMM::TimingEffect::BPM;
        timing.m_timingEffectParameter = bpm;
        timing.m_bpm                   = bpm;
        timing.m_beat_length           = 60000.0 / bpm;
        timings.push_back(timing);
    }
    return timings;
}

/// @brief 收集当前已打开且可写入的谱面列表。
std::vector<BpmMeasurementToolView::OpenBeatmapApplyOption>
BpmMeasurementToolView::collectApplyBeatmapOptions() const
{
    std::vector<OpenBeatmapApplyOption> options;
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    for ( std::size_t i = 0; i < entries.size(); ++i ) {
        const auto& entry = entries[i];
        if ( entry.isLogoPlaceholder || !entry.session ) {
            continue;
        }

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) {
            continue;
        }

        std::string label = entry.displayName;
        if ( label.empty() ) {
            label = ctx.currentBeatmap->m_baseMapMetadata.name;
        }
        if ( !ctx.currentBeatmap->m_baseMapMetadata.version.empty() ) {
            label += " [" + ctx.currentBeatmap->m_baseMapMetadata.version + "]";
        }

        options.push_back({ static_cast<int32_t>(i), entry.cameraId, label });
    }
    return options;
}

/// @brief 请求打开应用到谱面的弹窗。
void BpmMeasurementToolView::requestOpenApplyTimingPopup()
{
    ensureTimingSegments();
    m_shouldOpenApplyTimingPopup = true;
}

/// @brief 将测量结果应用到当前弹窗选中的谱面。
void BpmMeasurementToolView::applyMeasuredTimingsToSelectedBeatmap()
{
    if ( m_applyTargetSessionIndex < 0 ) {
        return;
    }

    auto& engine = Logic::EditorEngine::instance();
    engine.setActiveSessionIndex(m_applyTargetSessionIndex);
    engine.requestSessionFocus(m_applyTargetSessionIndex);
    engine.pushCommand(Logic::CmdReplaceBeatmapTimings{
        makeMeasuredTimings(), m_keepNonBpmTimingsOnApply });
    m_statusText = TR("ui.tools.bpm_measure.apply_done").data();
}

/// @brief 绘制右侧测量参数面板。
void BpmMeasurementToolView::renderControlPanel()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           TR("ui.tools.no_active_session").data());
        return;
    }
    ensureTimingSegments();

    if ( m_selectedAudioTrackId.empty() ) {
        m_selectedAudioLabel = TR("ui.tools.bpm_measure.select_track").data();
    } else {
        bool foundSelected = false;
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id == m_selectedAudioTrackId ) {
                m_selectedAudioLabel = makeAudioResourceLabel(resource);
                foundSelected        = true;
                break;
            }
        }
        if ( !foundSelected ) {
            m_selectedAudioTrackId.clear();
            m_selectedAudioLabel =
                TR("ui.tools.bpm_measure.select_track").data();
        }
    }

    ImGui::SeparatorText(TR("ui.tools.bpm_measure.audio").data());
    if ( ImGui::BeginCombo("##BpmMeasureAudioTrack",
                           m_selectedAudioLabel.c_str()) ) {
        for ( const auto& resource : project->m_audioResources ) {
            const bool  isSelected = resource.m_id == m_selectedAudioTrackId;
            std::string label      = makeAudioResourceLabel(resource);
            if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                m_selectedAudioTrackId = resource.m_id;
                m_selectedAudioLabel   = label;
                m_playbackSpeed        = std::clamp<double>(
                    resource.m_config.playbackSpeed, 0.25, 2.0);
                requestAnalyzeSelectedTrack();
            }
            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if ( project->m_audioResources.empty() ) {
        ImGui::TextDisabled("%s", TR("ui.tools.bpm_measure.no_audio").data());
    }

    const bool hasSelection = !m_selectedAudioTrackId.empty();
    if ( !hasSelection ) {
        ImGui::BeginDisabled();
    }
    if ( ImGui::Button(TR("ui.tools.bpm_measure.reload").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        requestAnalyzeSelectedTrack();
    }
    if ( ImGui::Button(TR("ui.tools.bpm_measure.auto_button").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        requestAutoMeasureSelectedTrack();
    }
    if ( !hasSelection ) {
        ImGui::EndDisabled();
    }

    renderPlaybackControls();

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.params").data());

    float bpm = static_cast<float>(m_bpm);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.bpm").data(),
                          &bpm,
                          0.01f,
                          1.0f,
                          999.0f,
                          "%.3f") ) {
        m_bpm               = std::clamp<double>(bpm, 1.0, 999.0);
        m_beatLengthSeconds = 60.0 / m_bpm;
        m_firstBeatTime     = clampFirstBeatTime(
            m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    constexpr double minBeatLength = 60.0 / 999.0;
    float            beatLength    = static_cast<float>(m_beatLengthSeconds);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.beat_length").data(),
                          &beatLength,
                          0.0001f,
                          static_cast<float>(minBeatLength),
                          60.0f,
                          "%.6f") ) {
        m_beatLengthSeconds =
            std::clamp<double>(beatLength, minBeatLength, 60.0);
        m_bpm           = 60.0 / m_beatLengthSeconds;
        m_firstBeatTime = clampFirstBeatTime(
            m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    const double minFirstBeat = firstBeatMinSeconds(m_beatLengthSeconds);
    float        firstBeat    = static_cast<float>(m_firstBeatTime);
    if ( ImGui::DragFloat(
             TR("ui.tools.bpm_measure.first_beat").data(),
             &firstBeat,
             0.001f,
             static_cast<float>(minFirstBeat),
             static_cast<float>(std::max(0.001, playbackCanvasDuration())),
             "%.6f") ) {
        m_firstBeatTime = clampFirstBeatTime(
            firstBeat, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    float markerWidth = static_cast<float>(m_markerWidthMs);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.marker_width").data(),
                          &markerWidth,
                          0.1f,
                          4.0f,
                          1000.0f,
                          "%.1f") ) {
        m_markerWidthMs = std::clamp<double>(markerWidth, 4.0, 1000.0);
    }

    int beatDivisor = m_beatDivisor;
    if ( ImGui::SliderInt(TR("ui.tools.bpm_measure.beat_divisor").data(),
                          &beatDivisor,
                          1,
                          64) ) {
        m_beatDivisor = std::clamp(beatDivisor, 1, 64);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.view").data());

    float center = static_cast<float>(m_viewCenter);
    if ( ImGui::DragFloat(
             TR("ui.tools.bpm_measure.center").data(),
             &center,
             0.01f,
             0.0f,
             static_cast<float>(std::max(0.001, playbackCanvasDuration())),
             "%.3f") ) {
        m_viewCenter = std::clamp<double>(
            center, 0.0, std::max(0.0, playbackCanvasDuration()));
    }

    float zoom = static_cast<float>(m_zoomSeconds);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.zoom").data(),
                          &zoom,
                          0.05f,
                          0.1f,
                          120.0f,
                          "%.2f") ) {
        m_zoomSeconds = std::clamp<double>(zoom, 0.1, 120.0);
    }

    if ( ImGui::Button(TR("ui.tools.bpm_measure.center_first").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        m_viewCenter = std::clamp<double>(
            m_firstBeatTime, 0.0, std::max(0.0, playbackCanvasDuration()));
    }

    renderTimingSegmentsPanel();

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.status").data());
    if ( m_analysisRunning.load(std::memory_order_relaxed) ) {
        const float progress =
            m_analysisProgress.load(std::memory_order_relaxed);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
        ImGui::Text("%s %.0f%%",
                    TR("ui.tools.bpm_measure.analyzing").data(),
                    progress * 100.0f);
    } else {
        ImGui::TextWrapped("%s", m_statusText.c_str());
    }

    if ( m_duration > 0.0 ) {
        ImGui::TextDisabled(
            "%s %.3fs", TR("ui.tools.bpm_measure.duration").data(), m_duration);
    }
}

/// @brief 绘制 BPM 段落列表和应用入口。
void BpmMeasurementToolView::renderTimingSegmentsPanel()
{
    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.segments").data());

    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float childHeight =
        std::min(170.0f, std::max(rowHeight * 3.0f, rowHeight * 4.5f));
    bool changed = false;
    if ( ImGui::BeginChild("##BpmMeasureSegments",
                           ImVec2(0.0f, childHeight),
                           ImGuiChildFlags_Borders) ) {
        for ( std::size_t i = 0; i < m_timingSegments.size(); ++i ) {
            auto& segment = m_timingSegments[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("#%zu", i + 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(78.0f);
            float time = static_cast<float>(segment.timestampSeconds);
            if ( ImGui::DragFloat("##SegmentTime",
                                  &time,
                                  0.001f,
                                  static_cast<float>(firstBeatMinSeconds(
                                      60.0 / std::max(1.0, segment.bpm))),
                                  static_cast<float>(std::max(
                                      0.001, playbackCanvasDuration())),
                                  "%.3fs") ) {
                segment.timestampSeconds = time;
                changed                  = true;
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s", TR("ui.tools.bpm_measure.segment_time").data());
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(76.0f);
            float bpm = static_cast<float>(segment.bpm);
            if ( ImGui::DragFloat(
                     "##SegmentBpm", &bpm, 0.01f, 1.0f, 999.0f, "%.3f") ) {
                segment.bpm = bpm;
                changed     = true;
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s", TR("ui.tools.bpm_measure.segment_bpm").data());
            }
            ImGui::SameLine();
            if ( i == 0 ) {
                ImGui::BeginDisabled();
            }
            if ( ImGui::SmallButton(TR("ui.common.delete").data()) ) {
                m_timingSegments.erase(m_timingSegments.begin() +
                                       static_cast<std::ptrdiff_t>(i));
                changed = true;
                if ( i == 0 ) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
                break;
            }
            if ( i == 0 ) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if ( changed ) {
        normalizeTimingSegments();
        resetMetronomeScheduler(m_viewCenter);
    }

    if ( ImGui::Button(TR("ui.tools.bpm_measure.add_segment").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        const std::size_t sourceIndex = findSegmentIndexForTime(m_viewCenter);
        const double      bpm         = m_timingSegments.empty()
                                            ? m_bpm
                                            : m_timingSegments[sourceIndex].bpm;
        m_timingSegments.push_back(
            { std::clamp<double>(
                  m_viewCenter, 0.0, std::max(0.0, playbackCanvasDuration())),
              bpm });
        normalizeTimingSegments();
        resetMetronomeScheduler(m_viewCenter);
    }

    ImGui::Checkbox(TR("ui.tools.bpm_measure.keep_scroll").data(),
                    &m_keepNonBpmTimingsOnApply);
    if ( ImGui::Button(TR("ui.tools.bpm_measure.apply_to_beatmap").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        requestOpenApplyTimingPopup();
    }
}

/// @brief 绘制自动测偏移后的应用确认弹窗。
void BpmMeasurementToolView::renderAutoApplyOffsetPopup()
{
    const char* popupTitle = TR("ui.tools.bpm_measure.auto_apply_title").data();
    if ( m_shouldOpenAutoApplyPopup ) {
        ImGui::OpenPopup(popupTitle);
        m_shouldOpenAutoApplyPopup = false;
    }

    if ( ImGui::BeginPopupModal(
             popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::TextWrapped(
            "%s", TR("ui.tools.bpm_measure.auto_apply_message").data());
        ImGui::Spacing();
        const float buttonWidth = 120.0f;
        if ( ImGui::Button(TR("ui.common.apply").data(),
                           ImVec2(buttonWidth, 0.0f)) ) {
            requestOpenApplyTimingPopup();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.common.cancel").data(),
                           ImVec2(buttonWidth, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制将测量结果应用到已打开谱面的弹窗。
void BpmMeasurementToolView::renderApplyTimingPopup()
{
    const char* popupTitle =
        TR("ui.tools.bpm_measure.apply_popup_title").data();
    if ( m_shouldOpenApplyTimingPopup ) {
        ImGui::OpenPopup(popupTitle);
        m_shouldOpenApplyTimingPopup = false;
    }

    if ( ImGui::BeginPopupModal(
             popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        const auto options = collectApplyBeatmapOptions();
        if ( options.empty() ) {
            ImGui::TextColored(
                Utils::UIThemeUtils::getWarningColor(),
                "%s",
                TR("ui.tools.bpm_measure.no_apply_target").data());
        } else {
            const auto activeIt = std::find_if(
                options.begin(), options.end(), [&](const auto& option) {
                    return option.sessionIndex == m_applyTargetSessionIndex;
                });
            if ( activeIt == options.end() ) {
                m_applyTargetSessionIndex = options.front().sessionIndex;
            }

            std::string preview = options.front().displayName;
            for ( const auto& option : options ) {
                if ( option.sessionIndex == m_applyTargetSessionIndex ) {
                    preview = option.displayName;
                    break;
                }
            }

            ImGui::Text("%s", TR("ui.tools.bpm_measure.apply_target").data());
            ImGui::SetNextItemWidth(360.0f);
            if ( ImGui::BeginCombo("##BpmApplyTarget", preview.c_str()) ) {
                for ( const auto& option : options ) {
                    const bool selected =
                        option.sessionIndex == m_applyTargetSessionIndex;
                    if ( ImGui::Selectable(option.displayName.c_str(),
                                           selected) ) {
                        m_applyTargetSessionIndex = option.sessionIndex;
                    }
                    if ( selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox(TR("ui.tools.bpm_measure.keep_scroll").data(),
                            &m_keepNonBpmTimingsOnApply);
        }

        ImGui::Spacing();
        const float buttonWidth = 120.0f;
        if ( options.empty() ) {
            ImGui::BeginDisabled();
        }
        if ( ImGui::Button(TR("ui.common.apply").data(),
                           ImVec2(buttonWidth, 0.0f)) ) {
            applyMeasuredTimingsToSelectedBeatmap();
            ImGui::CloseCurrentPopup();
        }
        if ( options.empty() ) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.common.cancel").data(),
                           ImVec2(buttonWidth, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制试听播放、暂停、进度和倍速控制。
/// @warning UI
/// 热路径：每帧执行；只读取播放状态和处理用户输入，文件检查仅在按钮触发后发生。
void BpmMeasurementToolView::renderPlaybackControls()
{
    auto&      audio        = Audio::AudioManager::instance();
    bool       trackLoaded  = isSelectedTrackLoadedForPlayback();
    const bool hasSelection = !m_selectedAudioTrackId.empty();
    const auto status =
        trackLoaded ? audio.getStatus() : Audio::PlaybackStatus::Stopped;
    const bool isPlaying = status == Audio::PlaybackStatus::Playing;

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.playback").data());

    if ( !hasSelection ) {
        ImGui::BeginDisabled();
    }

    const float iconButtonSize = ImGui::GetFrameHeight();
    Utils::pushFixedButtonStyleVars();
    if ( ImGui::Button(isPlaying ? ICON_MMM_PAUSE : ICON_MMM_PLAY,
                       ImVec2(iconButtonSize, iconButtonSize)) ) {
        if ( isPlaying ) {
            setPlaybackState(false);
        } else if ( loadSelectedTrackForPlayback() ) {
            trackLoaded            = true;
            const double totalTime = audio.getTotalTime();
            if ( totalTime > 0.0 &&
                 audio.getCurrentTime() >= totalTime - 0.001 ) {
                seekPlaybackToCanvasTime(0.0);
            }
            setPlaybackState(true);
        }
    }
    Utils::popFixedButtonStyleVars();
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s",
                          isPlaying ? TR("ui.tools.bpm_measure.pause").data()
                                    : TR("ui.tools.bpm_measure.play").data());
    }

    ImGui::SameLine();
    if ( !trackLoaded ) {
        ImGui::BeginDisabled();
    }
    Utils::pushFixedButtonStyleVars();
    if ( ImGui::Button(ICON_MMM_STOP,
                       ImVec2(iconButtonSize, iconButtonSize)) ) {
        setPlaybackState(false);
        seekPlaybackToCanvasTime(0.0);
        audio.stop();
    }
    Utils::popFixedButtonStyleVars();
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", TR("ui.tools.bpm_measure.stop").data());
    }
    if ( !trackLoaded ) {
        ImGui::EndDisabled();
    }

    if ( !hasSelection ) {
        ImGui::EndDisabled();
    }

    const double totalTime =
        trackLoaded ? std::max(audio.getTotalTime(), m_duration) : m_duration;
    const PlaybackTimelineState playbackState =
        trackLoaded ? readPlaybackTimelineState() : PlaybackTimelineState{};
    const double currentTime =
        trackLoaded ? playbackState.visualTime
                    : std::clamp(m_viewCenter, 0.0, std::max(0.0, totalTime));
    float position = static_cast<float>(
        std::clamp(currentTime, 0.0, std::max(0.0, totalTime)));
    if ( !trackLoaded || totalTime <= 0.0 ) {
        ImGui::BeginDisabled();
    }
    ImGui::Text("%s", TR("ui.tools.bpm_measure.position").data());
    ImGui::SetNextItemWidth(-1.0f);
    if ( ImGui::SliderFloat("##BpmMeasurePlaybackPosition",
                            &position,
                            0.0f,
                            static_cast<float>(std::max(0.001, totalTime)),
                            "%.3fs") ) {
        const double seekTime =
            std::clamp<double>(position, 0.0, std::max(0.0, totalTime));
        seekPlaybackToCanvasTime(seekTime);
    }
    if ( !trackLoaded || totalTime <= 0.0 ) {
        ImGui::EndDisabled();
    }

    const std::string positionText = Canvas::formatCanvasTime(position) +
                                     " / " +
                                     Canvas::formatCanvasTime(totalTime);
    ImGui::TextDisabled("%s", positionText.c_str());

    char speedInfo[160] = { 0 };
    std::snprintf(
        speedInfo,
        sizeof(speedInfo),
        TR("ui.audio_manager.speed_info").data(),
        m_playbackSpeed,
        trackLoaded ? audio.getActualPlaybackSpeed() : m_playbackSpeed);
    ImGui::TextDisabled("%s", speedInfo);

    const char* presetLabels[] = {
        TR("ui.audio_manager.speed_025x").data(),
        TR("ui.audio_manager.speed_050x").data(),
        TR("ui.audio_manager.speed_075x").data(),
        TR("ui.audio_manager.speed_100x").data(),
    };
    constexpr double presetSpeeds[] = { 0.25, 0.5, 0.75, 1.0 };
    const float      spacing        = ImGui::GetStyle().ItemSpacing.x;
    const float      buttonWidth    = std::max(
        0.0f, (ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f);
    constexpr size_t presetCount =
        sizeof(presetSpeeds) / sizeof(presetSpeeds[0]);
    for ( size_t i = 0; i < presetCount; ++i ) {
        if ( i > 0 ) {
            ImGui::SameLine();
        }
        if ( ImGui::Button(presetLabels[i], ImVec2(buttonWidth, 0.0f)) ) {
            applyPlaybackSpeed(presetSpeeds[i]);
        }
    }

    float speed = static_cast<float>(m_playbackSpeed);
    ImGui::Text("%s", TR("ui.audio_manager.speed_value").data());
    ImGui::SetNextItemWidth(-1.0f);
    if ( ImGui::SliderFloat(
             "##BpmMeasurePlaybackSpeed", &speed, 0.25f, 2.0f, "%.4fx") ) {
        applyPlaybackSpeed(speed);
    }
}

/// @brief 绘制左侧波形和频谱面板。
void BpmMeasurementToolView::renderAnalysisPanel()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if ( avail.x <= 1.0f || avail.y <= 1.0f ) {
        return;
    }

    followPlaybackIfNeeded();

    ImGui::Text("%s", TR("ui.tools.bpm_measure.waveform").data());
    const float textH      = ImGui::GetTextLineHeightWithSpacing();
    const float spacingY   = ImGui::GetStyle().ItemSpacing.y;
    const float waveHeight = std::max(120.0f, (avail.y - textH * 2.0f) * 0.42f);
    const float specHeight =
        std::max(160.0f, avail.y - waveHeight - textH * 2.0f - spacingY * 2.0f);

    renderWaveformPlot(ImVec2(avail.x, waveHeight));

    ImGui::Text("%s", TR("ui.tools.bpm_measure.spectrum").data());
    renderSpectrumImage(ImVec2(avail.x, specHeight));
}

/// @brief 播放时让分析视图自动跟随播放指针。
/// @warning UI
/// 热路径：每帧执行；只读取播放同步快照并更新视图中心，不能访问文件系统。
void BpmMeasurementToolView::followPlaybackIfNeeded()
{
    if ( m_isTimelinePanning || m_isPlaybackCursorDragging ||
         m_isBeatMarkerDragging || !isSelectedTrackLoadedForPlayback() ) {
        return;
    }

    const PlaybackTimelineState playbackState = readPlaybackTimelineState();
    if ( !playbackState.isPlaying ) {
        return;
    }

    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 ) {
        return;
    }

    m_viewCenter = std::clamp<double>(
        playbackState.visualTime, 0.0, std::max(0.0, canvasDuration));
}

/// @brief 更新 BPM 工具节拍器音效触发。
/// @warning UI
/// 热路径：每帧执行；只读取播放同步快照并播放已预加载音效，不访问文件系统。
void BpmMeasurementToolView::updateMetronomePlayback()
{
    ensureTimingSegments();
    if ( !isSelectedTrackLoadedForPlayback() || m_timingSegments.empty() ) {
        m_metronomeScheduleInitialized = false;
        return;
    }

    auto& audio = Audio::AudioManager::instance();
    if ( !m_metronomeSfxReady ) {
        m_metronomeSfxReady =
            audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 &&
            audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0;
        if ( !m_metronomeSfxReady ) {
            return;
        }
    }

    const PlaybackTimelineState playbackState = readPlaybackTimelineState();
    if ( !playbackState.isPlaying ) {
        m_metronomeScheduleInitialized = false;
        return;
    }

    const double canvasDuration = playbackCanvasDuration();
    const double totalTime =
        std::max(canvasDuration, std::max(m_duration, playbackState.totalTime));
    if ( canvasDuration <= 0.0 || totalTime <= 0.0 ) {
        m_metronomeScheduleInitialized = false;
        return;
    }

    const double audioTime =
        std::clamp(playbackState.audioTime, 0.0, totalTime);
    const std::size_t activeSegmentIndex = findSegmentIndexForTime(audioTime);
    const double      activeFirstBeatTime =
        m_timingSegments[activeSegmentIndex].timestampSeconds;
    const double activeBeatLength =
        segmentBeatLengthSeconds(activeSegmentIndex);
    const bool gridChanged =
        m_metronomeScheduledSegmentIndex != activeSegmentIndex ||
        std::abs(m_metronomeScheduledFirstBeatTime - activeFirstBeatTime) >
            1e-9 ||
        std::abs(m_metronomeScheduledBeatLength - activeBeatLength) > 1e-9;
    const double jumpThreshold = std::max(0.25, activeBeatLength * 2.0);
    const bool   jumped = audioTime + 1e-4 < m_lastMetronomeAudioTime ||
                          audioTime - m_lastMetronomeAudioTime > jumpThreshold;
    if ( !m_metronomeScheduleInitialized || gridChanged || jumped ) {
        resetMetronomeScheduler(audioTime);
    }

    const double scheduleEndAudioTime = std::min(
        totalTime, audioTime + BPM_METRONOME_SCHEDULE_LOOKAHEAD_SECONDS);

    int          scheduledCount{ 0 };
    const double scheduledSegmentEnd =
        m_metronomeScheduledSegmentIndex + 1 < m_timingSegments.size()
            ? m_timingSegments[m_metronomeScheduledSegmentIndex + 1]
                  .timestampSeconds
            : totalTime;
    while ( scheduledCount < BPM_METRONOME_MAX_TRIGGERED_PER_FRAME ) {
        const double beatAudioTime =
            m_metronomeScheduledFirstBeatTime +
            static_cast<double>(m_nextMetronomeBeatIndex) *
                m_metronomeScheduledBeatLength;
        if ( beatAudioTime < audioTime - BPM_METRONOME_PAST_TRIGGER_WINDOW ) {
            ++m_nextMetronomeBeatIndex;
            continue;
        }
        if ( beatAudioTime >= scheduledSegmentEnd - 1e-9 ) {
            break;
        }
        if ( beatAudioTime > scheduleEndAudioTime ) {
            break;
        }

        // 拍线和频谱都位于音频/谱面时间轴，SFX 也按同一时间轴调度；
        // visualOffset 只补偿播放指针显示，不能在这里二次叠加。
        if ( beatAudioTime >= 0.0 && beatAudioTime <= totalTime ) {
            int64_t beatMod = m_nextMetronomeBeatIndex % 4;
            if ( beatMod < 0 ) {
                beatMod += 4;
            }
            const char* key =
                beatMod == 0 ? BPM_METRONOME_HIGH_KEY : BPM_METRONOME_LOW_KEY;
            audio.playSoundEffectScheduled(
                key, beatAudioTime, BPM_METRONOME_VOLUME_FACTOR);
        }

        ++m_nextMetronomeBeatIndex;
        ++scheduledCount;
    }

    m_lastMetronomeAudioTime = audioTime;
}

/// @brief 确保 BPM 工具节拍器音效已预加载。
/// @return 两个节拍器音效均可播放时返回 true。
bool BpmMeasurementToolView::ensureMetronomeSoundEffects()
{
    auto& audio = Audio::AudioManager::instance();
    if ( audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 &&
         audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0 ) {
        m_metronomeSfxReady = true;
        return true;
    }

    const auto& skinData         = Config::SkinManager::instance().getData();
    auto        resolveAudioPath = [&](const char*                  key,
                                       const std::filesystem::path& fallback) {
        if ( const auto it = skinData.audioPaths.find(key);
             it != skinData.audioPaths.end() ) {
            return it->second;
        }
        return skinData.skinPath / Config::utf8ToPath("resources") / fallback;
    };

    const std::filesystem::path lowPath =
        resolveAudioPath(BPM_METRONOME_LOW_KEY,
                         Config::utf8ToPath("audio/metronome/beat_low.wav"));
    const std::filesystem::path highPath = resolveAudioPath(
        BPM_METRONOME_HIGH_KEY,
        Config::utf8ToPath("audio/metronome/downbeat_high.wav"));
    auto resolveLeadIn = [&](const char* key) {
        if ( const auto it = skinData.audioLeadInSeconds.find(key);
             it != skinData.audioLeadInSeconds.end() ) {
            return it->second;
        }
        return 0.0;
    };

    const bool lowLoaded =
        audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 ||
        audio.preloadSoundEffect(BPM_METRONOME_LOW_KEY,
                                 Config::pathToUtf8(lowPath),
                                 1.0f,
                                 resolveLeadIn(BPM_METRONOME_LOW_KEY));
    const bool highLoaded =
        audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0 ||
        audio.preloadSoundEffect(BPM_METRONOME_HIGH_KEY,
                                 Config::pathToUtf8(highPath),
                                 1.0f,
                                 resolveLeadIn(BPM_METRONOME_HIGH_KEY));
    m_metronomeSfxReady = lowLoaded && highLoaded;
    return m_metronomeSfxReady;
}

/// @brief 从当前音频调度时间重置节拍器调度游标。
/// @param audioTime 当前音频调度时间，单位为秒。
void BpmMeasurementToolView::resetMetronomeScheduler(double audioTime)
{
    ensureTimingSegments();
    if ( m_timingSegments.empty() ) {
        m_metronomeScheduleInitialized = false;
        return;
    }

    const std::size_t segmentIndex = findSegmentIndexForTime(audioTime);
    const double      beatLength   = segmentBeatLengthSeconds(segmentIndex);
    if ( beatLength <= 1e-6 ) {
        m_metronomeScheduleInitialized = false;
        return;
    }

    const double firstBeatTime =
        m_timingSegments[segmentIndex].timestampSeconds;
    m_nextMetronomeBeatIndex = static_cast<int64_t>(
        std::ceil((audioTime - firstBeatTime) / beatLength - 1e-6));
    m_lastMetronomeAudioTime          = audioTime;
    m_metronomeScheduledFirstBeatTime = firstBeatTime;
    m_metronomeScheduledBeatLength    = beatLength;
    m_metronomeScheduledSegmentIndex  = segmentIndex;
    m_metronomeScheduleInitialized    = true;
}

/// @brief 更新波形绘制用的画布时间缓存。
/// @param canvasOffset 画布时间相对音频采样时间的偏移，单位为秒。
/// @warning UI
/// 热路径：波形图每帧查询；仅在画布偏移或波形缓存变化时重建时间数组。
void BpmMeasurementToolView::updateWaveCanvasTimes(double canvasOffset)
{
    if ( m_waveCanvasTimes.size() == m_waveTimes.size() &&
         std::abs(m_waveCanvasTimesOffset - canvasOffset) < 1e-9 ) {
        return;
    }

    m_waveCanvasTimes.resize(m_waveTimes.size());
    for ( size_t i = 0; i < m_waveTimes.size(); ++i ) {
        m_waveCanvasTimes[i] = m_waveTimes[i] + canvasOffset;
    }
    m_waveCanvasTimesOffset = canvasOffset;
}

/// @brief 绘制波形图。
/// @param size 绘制区域尺寸。
void BpmMeasurementToolView::renderWaveformPlot(const ImVec2& size)
{
    const double canvasDuration = std::max(0.0, playbackCanvasDuration());
    const double clampedCenter =
        std::clamp<double>(m_viewCenter, 0.0, canvasDuration);
    const double viewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
    const double viewEnd = std::min(std::max(canvasDuration, viewStart + 0.001),
                                    clampedCenter + m_zoomSeconds);

    if ( ImPlot::BeginPlot("##BpmMeasureWaveform",
                           size,
                           ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                               ImPlotFlags_NoMouseText) ) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1,
                          nullptr,
                          ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(
            ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.05, 1.05, ImGuiCond_Always);

        if ( !m_waveTimes.empty() ) {
            updateWaveCanvasTimes(0.0);
            const size_t availableCount = std::min(
                { m_waveCanvasTimes.size(),
                  m_waveMin.size(),
                  m_waveMax.size(),
                  static_cast<size_t>(std::numeric_limits<int>::max()) });
            auto beginIt = std::lower_bound(
                m_waveCanvasTimes.begin(),
                m_waveCanvasTimes.begin() +
                    static_cast<std::ptrdiff_t>(availableCount),
                viewStart);
            auto endIt = std::upper_bound(
                m_waveCanvasTimes.begin(),
                m_waveCanvasTimes.begin() +
                    static_cast<std::ptrdiff_t>(availableCount),
                viewEnd);
            if ( beginIt != m_waveCanvasTimes.begin() ) {
                --beginIt;
            }
            if ( endIt != m_waveCanvasTimes.begin() +
                              static_cast<std::ptrdiff_t>(availableCount) ) {
                ++endIt;
            }

            const size_t firstVisibleIndex =
                static_cast<size_t>(beginIt - m_waveCanvasTimes.begin());
            const int visibleCount = static_cast<int>(endIt - beginIt);
            if ( visibleCount >= 2 ) {
                ImPlot::PlotShaded("##WaveEnvelope",
                                   m_waveCanvasTimes.data() + firstVisibleIndex,
                                   m_waveMin.data() + firstVisibleIndex,
                                   m_waveMax.data() + firstVisibleIndex,
                                   visibleCount,
                                   ImPlotSpec(ImPlotProp_FillAlpha, 0.55f));
            }
        }

        ImPlot::PushPlotClipRect();
        ImVec2 plotMin = ImPlot::GetPlotPos();
        ImVec2 plotMax = ImVec2(plotMin.x + ImPlot::GetPlotSize().x,
                                plotMin.y + ImPlot::GetPlotSize().y);
        drawBeatSubdivisionLines(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        drawBeatMarkers(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        drawPlaybackCursor(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        handlePlaybackCursorDrag(plotMin, plotMax, viewStart, viewEnd, 1);
        handleBeatMarkerDrag(plotMin, plotMax, viewStart, viewEnd, 1);
        handleTimelineNavigation(plotMin, plotMax, viewStart, viewEnd);
        ImPlot::PopPlotClipRect();

        if ( ImPlot::IsPlotHovered() ) {
            ImPlotPoint  mousePos  = ImPlot::GetPlotMousePos();
            const double hoverTime = std::clamp<double>(
                mousePos.x, 0.0, std::max(0.0, canvasDuration));
            ImGui::SetTooltip("%s",
                              Canvas::formatCanvasTime(hoverTime).c_str());
            if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                m_firstBeatTime = hoverTime;
                syncPrimaryTimingFieldsToSegments();
            }
        }

        ImPlot::EndPlot();
    }
}

/// @brief 绘制频谱图。
/// @param size 绘制区域尺寸。
void BpmMeasurementToolView::renderSpectrumImage(const ImVec2& size)
{
    const double canvasDuration = std::max(0.0, playbackCanvasDuration());
    const double clampedCenter =
        std::clamp<double>(m_viewCenter, 0.0, canvasDuration);
    const double viewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
    const double viewEnd = std::min(std::max(canvasDuration, viewStart + 0.001),
                                    clampedCenter + m_zoomSeconds);
    const double viewRange      = std::max(0.001, viewEnd - viewStart);
    const double audioViewStart = viewStart;
    const double audioViewEnd   = viewEnd;
    const double pixelStart     = audioViewStart * m_spectrumSegmentsPerSecond;
    const double pixelEnd       = audioViewEnd * m_spectrumSegmentsPerSecond;
    const double pixelWidth     = std::max(1.0, pixelEnd - pixelStart);

    ImVec2      imageMin = ImGui::GetCursorScreenPos();
    ImVec2      imageMax = ImVec2(imageMin.x + size.x, imageMin.y + size.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(12, 14, 18, 255));

    if ( !m_texturesNeedReload && !m_spectrumTextures.empty() ) {
        for ( size_t i = 0; i < m_spectrumTextures.size(); ++i ) {
            const auto&  texture = m_spectrumTextures[i];
            const double texStart =
                static_cast<double>(i * static_cast<size_t>(MAX_TEXTURE_W));
            const double texEnd = texStart + texture->width();

            if ( texEnd < pixelStart || texStart > pixelEnd ) {
                continue;
            }

            const double intersectStart = std::max(texStart, pixelStart);
            const double intersectEnd   = std::min(texEnd, pixelEnd);
            const float  uv0x = static_cast<float>((intersectStart - texStart) /
                                                   texture->width());
            const float  uv1x = static_cast<float>((intersectEnd - texStart) /
                                                   texture->width());
            const float  screenX0 =
                imageMin.x + static_cast<float>((intersectStart - pixelStart) /
                                                pixelWidth * size.x);
            const float screenX1 =
                imageMin.x + static_cast<float>((intersectEnd - pixelStart) /
                                                pixelWidth * size.x);
            if ( screenX1 <= screenX0 ) {
                continue;
            }

            drawList->AddImage(texture->getImTextureID(),
                               ImVec2(screenX0, imageMin.y),
                               ImVec2(screenX1, imageMax.y),
                               ImVec2(uv0x, 0.0f),
                               ImVec2(uv1x, 1.0f));
        }
    }

    drawBeatSubdivisionLines(*drawList, imageMin, imageMax, viewStart, viewEnd);
    drawBeatMarkers(*drawList, imageMin, imageMax, viewStart, viewEnd);
    drawPlaybackCursor(*drawList, imageMin, imageMax, viewStart, viewEnd);
    handlePlaybackCursorDrag(imageMin, imageMax, viewStart, viewEnd, 2);
    handleBeatMarkerDrag(imageMin, imageMax, viewStart, viewEnd, 2);

    ImGui::SetCursorScreenPos(imageMin);
    ImGui::InvisibleButton(
        "##BpmMeasureSpectrumHover",
        ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y));
    handleTimelineNavigation(imageMin, imageMax, viewStart, viewEnd);
    const bool isSpectrumHovered = ImGui::IsItemHovered();
    if ( isSpectrumHovered ) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        const double relX     = std::clamp<double>(
            (mousePos.x - imageMin.x) / std::max(1.0f, imageMax.x - imageMin.x),
            0.0,
            1.0);
        const double hoverTime = std::clamp<double>(
            viewStart + relX * viewRange, 0.0, std::max(0.0, canvasDuration));
        ImGui::SetTooltip("%s", Canvas::formatCanvasTime(hoverTime).c_str());
        if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
            m_firstBeatTime = hoverTime;
            syncPrimaryTimingFieldsToSegments();
        }
    }
}

/// @brief 在指定矩形区域叠加拍线、黄色拍框和首拍红色覆盖框。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
void BpmMeasurementToolView::drawBeatMarkers(ImDrawList&   drawList,
                                             const ImVec2& rectMin,
                                             const ImVec2& rectMax,
                                             double        viewStart,
                                             double        viewEnd) const
{
    if ( m_timingSegments.empty() || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x ) {
        return;
    }

    const auto&  segments = m_timingSegments;
    const double markerSeconds =
        std::clamp<double>(m_markerWidthMs / 1000.0, 0.004, 1.0);
    const double halfMarkerSeconds = markerSeconds * 0.5;
    const double width             = rectMax.x - rectMin.x;

    auto timeToX = [&](double time) {
        return rectMin.x + static_cast<float>((time - viewStart) /
                                              (viewEnd - viewStart) * width);
    };

    const ImU32 white           = IM_COL32(255, 255, 255, 255);
    const ImU32 fill            = IM_COL32(255, 220, 60, 70);
    const ImU32 border          = IM_COL32(255, 220, 60, 235);
    const ImU32 firstBeatFill   = IM_COL32(255, 40, 40, 82);
    const ImU32 firstBeatBorder = IM_COL32(255, 35, 35, 255);
    const ImU32 firstBeatHandle = IM_COL32(255, 90, 90, 255);

    drawList.PushClipRect(rectMin, rectMax, true);
    for ( std::size_t segmentIndex = 0; segmentIndex < segments.size();
          ++segmentIndex ) {
        const auto&  segment    = segments[segmentIndex];
        const double beatLength = 60.0 / std::clamp(segment.bpm, 1.0, 999.0);
        if ( beatLength <= 1e-6 ) {
            continue;
        }

        const double segmentStart = segment.timestampSeconds;
        const double segmentEnd =
            segmentIndex + 1 < segments.size()
                ? segments[segmentIndex + 1].timestampSeconds
                : viewEnd + beatLength;
        if ( segmentIndex + 1 < segments.size() &&
             segmentEnd <= segmentStart + 1e-9 ) {
            continue;
        }

        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart - halfMarkerSeconds ) {
            continue;
        }

        const int64_t firstIndex = static_cast<int64_t>(std::ceil(
            (visibleStart - segmentStart - halfMarkerSeconds) / beatLength));
        const int64_t lastIndex  = static_cast<int64_t>(std::floor(
            (visibleEnd - segmentStart + halfMarkerSeconds) / beatLength));
        for ( int64_t index = firstIndex; index <= lastIndex; ++index ) {
            const double beatTime =
                segmentStart + static_cast<double>(index) * beatLength;
            if ( segmentIndex > 0 && beatTime < segmentStart - 1e-6 ) {
                continue;
            }
            if ( segmentIndex + 1 < segments.size() &&
                 beatTime >= segmentEnd + 1e-6 ) {
                continue;
            }

            const float lineX             = timeToX(beatTime);
            const bool  isFirstBeatMarker = index == 0;
            const float halfBoxWidth =
                std::max(3.0f,
                         static_cast<float>(halfMarkerSeconds /
                                            (viewEnd - viewStart) * width));
            if ( lineX < rectMin.x - halfBoxWidth ||
                 lineX > rectMax.x + halfBoxWidth ) {
                continue;
            }

            drawList.AddLine(ImVec2(lineX, rectMin.y),
                             ImVec2(lineX, rectMax.y),
                             white,
                             1.0f);

            const float boxX1 = lineX - halfBoxWidth;
            const float boxX2 = lineX + halfBoxWidth;
            if ( boxX2 <= boxX1 ) {
                continue;
            }

            drawList.AddRectFilled(
                ImVec2(boxX1, rectMin.y), ImVec2(boxX2, rectMax.y), fill);
            drawList.AddRect(ImVec2(boxX1, rectMin.y),
                             ImVec2(boxX2, rectMax.y),
                             border,
                             0.0f,
                             0,
                             1.5f);
            if ( isFirstBeatMarker ) {
                drawList.AddRectFilled(ImVec2(boxX1, rectMin.y),
                                       ImVec2(boxX2, rectMax.y),
                                       firstBeatFill);
                drawList.AddRect(ImVec2(boxX1, rectMin.y),
                                 ImVec2(boxX2, rectMax.y),
                                 firstBeatBorder,
                                 0.0f,
                                 0,
                                 2.4f);
                drawList.AddLine(ImVec2(lineX, rectMin.y),
                                 ImVec2(lineX, rectMax.y),
                                 firstBeatBorder,
                                 2.0f);
            }

            const ImVec2 handleLeft(lineX - BEAT_MARKER_HANDLE_HALF_WIDTH,
                                    rectMin.y);
            const ImVec2 handleRight(lineX + BEAT_MARKER_HANDLE_HALF_WIDTH,
                                     rectMin.y);
            const ImVec2 handleTip(lineX,
                                   rectMin.y + BEAT_MARKER_HANDLE_HEIGHT);
            drawList.AddTriangleFilled(
                handleLeft, handleRight, handleTip, IM_COL32(80, 65, 0, 230));
            drawList.AddTriangleFilled(
                ImVec2(handleLeft.x + 1.0f, handleLeft.y + 1.0f),
                ImVec2(handleRight.x - 1.0f, handleRight.y + 1.0f),
                ImVec2(handleTip.x, handleTip.y - 1.0f),
                isFirstBeatMarker ? firstBeatHandle : border);
        }
    }
    drawList.PopClipRect();
}

/// @brief 在指定矩形区域叠加分拍线。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI
/// 热路径：波形图和频谱图每帧执行；按当前视野增量绘制分拍线，不得加入音频解码或文件访问。
void BpmMeasurementToolView::drawBeatSubdivisionLines(ImDrawList&   drawList,
                                                      const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double viewEnd) const
{
    const int beatDivisor = std::clamp(m_beatDivisor, 1, 64);
    if ( m_timingSegments.empty() || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x || rectMax.y <= rectMin.y ) {
        return;
    }

    const auto&  segments    = m_timingSegments;
    const double width       = rectMax.x - rectMin.x;
    const int    columnCount = std::max(1, static_cast<int>(std::ceil(width)));
    drawList.PushClipRect(rectMin, rectMax, true);
    for ( std::size_t segmentIndex = 0; segmentIndex < segments.size();
          ++segmentIndex ) {
        const auto&  segment      = segments[segmentIndex];
        const double beatLength   = 60.0 / std::clamp(segment.bpm, 1.0, 999.0);
        const double stepDuration = beatLength / beatDivisor;
        if ( stepDuration <= 1e-6 ) {
            continue;
        }

        const double segmentStart = segment.timestampSeconds;
        const double segmentEnd =
            segmentIndex + 1 < segments.size()
                ? segments[segmentIndex + 1].timestampSeconds
                : viewEnd + stepDuration;
        if ( segmentIndex + 1 < segments.size() &&
             segmentEnd <= segmentStart + 1e-9 ) {
            continue;
        }

        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart ) {
            continue;
        }

        int64_t stepOffset = static_cast<int64_t>(
            std::ceil((visibleStart - segmentStart) / stepDuration - 1e-4));
        if ( segmentIndex == 0 && visibleStart < segmentStart ) {
            stepOffset = static_cast<int64_t>(std::floor(
                (visibleStart - segmentStart) / stepDuration + 1e-4));
        }

        double t =
            segmentStart + static_cast<double>(stepOffset) * stepDuration;
        while ( t < visibleStart - 1e-4 ) {
            ++stepOffset;
            t = segmentStart + static_cast<double>(stepOffset) * stepDuration;
        }

        int lastColumn = -1;
        while ( t <= visibleEnd + 1e-4 ) {
            const float lineX =
                rectMin.x + static_cast<float>((t - viewStart) /
                                               (viewEnd - viewStart) * width);
            const int column = static_cast<int>(std::floor(lineX - rectMin.x));
            if ( column >= 0 && column < columnCount && column != lastColumn ) {
                int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                if ( beatIndex < 0 ) {
                    beatIndex += beatDivisor;
                }

                int denominator = 1;
                if ( beatIndex != 0 ) {
                    const int divisorGcd = std::gcd(beatIndex, beatDivisor);
                    denominator          = beatDivisor / divisorGcd;
                }

                const BeatLineStyle style = getBeatLineStyle(denominator);
                drawList.AddLine(ImVec2(lineX, rectMin.y),
                                 ImVec2(lineX, rectMax.y),
                                 style.color,
                                 style.width);
                lastColumn = column;
            }

            ++stepOffset;
            t = segmentStart + static_cast<double>(stepOffset) * stepDuration;
        }
    }
    drawList.PopClipRect();
}

/// @brief 在指定矩形区域叠加当前音频播放指针。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI
/// 热路径：波形图和频谱图每帧执行；只读取当前播放路径、播放时间并绘制播放指针。
void BpmMeasurementToolView::drawPlaybackCursor(ImDrawList&   drawList,
                                                const ImVec2& rectMin,
                                                const ImVec2& rectMax,
                                                double        viewStart,
                                                double        viewEnd) const
{
    if ( viewEnd <= viewStart || rectMax.x <= rectMin.x ||
         rectMax.y <= rectMin.y || !isSelectedTrackLoadedForPlayback() ) {
        return;
    }

    const PlaybackTimelineState playbackState = readPlaybackTimelineState();
    const double totalTime = std::max(m_duration, playbackState.totalTime);
    if ( totalTime <= 0.0 ) {
        return;
    }

    const double canvasTime = playbackState.visualTime;
    if ( canvasTime < viewStart || canvasTime > viewEnd ) {
        return;
    }

    const float lineX =
        rectMin.x +
        static_cast<float>((canvasTime - viewStart) / (viewEnd - viewStart) *
                           (rectMax.x - rectMin.x));

    drawList.PushClipRect(rectMin, rectMax, true);
    drawList.AddLine(ImVec2(lineX, rectMin.y),
                     ImVec2(lineX, rectMax.y),
                     IM_COL32(80, 0, 0, 220),
                     4.0f);
    drawList.AddLine(ImVec2(lineX, rectMin.y),
                     ImVec2(lineX, rectMax.y),
                     IM_COL32(255, 40, 40, 255),
                     2.0f);
    const ImVec2 handleLeft(lineX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH,
                            rectMin.y);
    const ImVec2 handleRight(lineX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH,
                             rectMin.y);
    const ImVec2 handleTip(lineX, rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT);
    drawList.AddTriangleFilled(
        handleLeft, handleRight, handleTip, IM_COL32(80, 0, 0, 230));
    drawList.AddTriangleFilled(
        ImVec2(handleLeft.x + 1.0f, handleLeft.y + 1.0f),
        ImVec2(handleRight.x - 1.0f, handleRight.y + 1.0f),
        ImVec2(handleTip.x, handleTip.y - 1.0f),
        IM_COL32(255, 40, 40, 255));
    drawList.AddTriangle(
        handleLeft, handleRight, handleTip, IM_COL32(255, 170, 170, 220), 1.0f);
    drawList.PopClipRect();
}

/// @brief 处理整拍线顶部三角手柄的拖拽，反向调整第一拍位置。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
/// @warning UI
/// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算，不访问文件系统。
void BpmMeasurementToolView::handleBeatMarkerDrag(const ImVec2& rectMin,
                                                  const ImVec2& rectMax,
                                                  double        viewStart,
                                                  double viewEnd, int ownerId)
{
    ensureTimingSegments();
    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x || rectMax.y <= rectMin.y ) {
        if ( m_beatMarkerDragOwner == ownerId ) {
            m_isBeatMarkerDragging = false;
            m_beatMarkerDragOwner  = 0;
        }
        return;
    }

    if ( m_isPlaybackCursorDragging ||
         (m_isBeatMarkerDragging && m_beatMarkerDragOwner != ownerId) ) {
        return;
    }

    const double width   = rectMax.x - rectMin.x;
    auto         timeToX = [&](double time) {
        return rectMin.x + static_cast<float>((time - viewStart) /
                                              (viewEnd - viewStart) * width);
    };

    const ImVec2 mousePos = ImGui::GetMousePos();
    int64_t      hoveredIndex{ 0 };
    std::size_t  hoveredSegmentIndex{ 0 };
    float        hoveredDistance  = std::numeric_limits<float>::max();
    bool         hasHoveredHandle = false;

    constexpr int64_t MAX_SCANNED_BEAT_HANDLES = 4096;
    int64_t           scannedHandleCount       = 0;

    for ( std::size_t segmentIndex = 0;
          segmentIndex < m_timingSegments.size() &&
          scannedHandleCount < MAX_SCANNED_BEAT_HANDLES;
          ++segmentIndex ) {
        const auto&  segment      = m_timingSegments[segmentIndex];
        const double beatLength   = segmentBeatLengthSeconds(segmentIndex);
        const double segmentStart = segment.timestampSeconds;
        const double segmentEnd =
            segmentIndex + 1 < m_timingSegments.size()
                ? m_timingSegments[segmentIndex + 1].timestampSeconds
                : viewEnd + beatLength;
        if ( beatLength <= 1e-6 ||
             (segmentIndex + 1 < m_timingSegments.size() &&
              segmentEnd <= segmentStart + 1e-9) ) {
            continue;
        }

        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart ) {
            continue;
        }

        const int64_t firstIndex =
            std::max<int64_t>(1,
                              static_cast<int64_t>(std::ceil(
                                  (visibleStart - segmentStart) / beatLength)));
        const int64_t lastIndex = static_cast<int64_t>(
            std::floor((visibleEnd - segmentStart) / beatLength));
        for ( int64_t index = firstIndex;
              index <= lastIndex &&
              scannedHandleCount < MAX_SCANNED_BEAT_HANDLES;
              ++index, ++scannedHandleCount ) {
            const double beatTime =
                segmentStart + static_cast<double>(index) * beatLength;
            if ( segmentIndex + 1 < m_timingSegments.size() &&
                 beatTime >= segmentEnd + 1e-6 ) {
                continue;
            }

            const float  lineX = timeToX(beatTime);
            const ImVec2 handleMin(lineX - BEAT_MARKER_HANDLE_HALF_WIDTH - 2.0f,
                                   rectMin.y);
            const ImVec2 handleMax(
                lineX + BEAT_MARKER_HANDLE_HALF_WIDTH + 2.0f,
                rectMin.y + BEAT_MARKER_HANDLE_HEIGHT + 3.0f);
            if ( !ImGui::IsMouseHoveringRect(handleMin, handleMax, true) ) {
                continue;
            }

            const float distance = std::abs(mousePos.x - lineX);
            if ( distance < hoveredDistance ) {
                hoveredDistance     = distance;
                hoveredIndex        = index;
                hoveredSegmentIndex = segmentIndex;
                hasHoveredHandle    = true;
            }
        }
    }

    if ( hasHoveredHandle || m_isBeatMarkerDragging ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if ( hasHoveredHandle && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        m_isBeatMarkerDragging    = true;
        m_isTimelinePanning       = false;
        m_beatMarkerDragOwner     = ownerId;
        m_draggedBeatIndex        = hoveredIndex;
        m_draggedBeatSegmentIndex = hoveredSegmentIndex;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        if ( m_beatMarkerDragOwner == ownerId ) {
            m_isBeatMarkerDragging = false;
            m_beatMarkerDragOwner  = 0;
        }
        return;
    }

    if ( !m_isBeatMarkerDragging ) {
        return;
    }

    if ( m_draggedBeatSegmentIndex >= m_timingSegments.size() ||
         m_draggedBeatIndex <= 0 ) {
        return;
    }

    const double rectWidth = std::max(1.0f, rectMax.x - rectMin.x);
    const double relX =
        std::clamp<double>((mousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
    const double targetBeatTime = std::clamp<double>(
        viewStart + relX * (viewEnd - viewStart), 0.0, canvasDuration);
    auto&        segment = m_timingSegments[m_draggedBeatSegmentIndex];
    const double minTargetTime =
        segment.timestampSeconds +
        static_cast<double>(m_draggedBeatIndex) * (60.0 / 999.0);
    const double clampedTargetTime = std::max(minTargetTime, targetBeatTime);
    const double beatLength = (clampedTargetTime - segment.timestampSeconds) /
                              static_cast<double>(m_draggedBeatIndex);
    if ( beatLength > 1e-6 && std::isfinite(beatLength) ) {
        segment.bpm = std::clamp(60.0 / beatLength, 1.0, 999.0);
        syncPrimaryTimingFieldsFromSegments();
        resetMetronomeScheduler(clampedTargetTime);
    }

    const std::string tooltip =
        fmt::format("{} / {:.3f} BPM",
                    Canvas::formatCanvasTime(clampedTargetTime),
                    segment.bpm);
    ImGui::SetTooltip("%s", tooltip.c_str());
}

/// @brief 处理播放指针顶部三角手柄的拖拽跳转。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
/// @warning UI
/// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算，不访问文件系统。
void BpmMeasurementToolView::handlePlaybackCursorDrag(const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double        viewEnd,
                                                      int           ownerId)
{
    if ( viewEnd <= viewStart || rectMax.x <= rectMin.x ||
         rectMax.y <= rectMin.y || !isSelectedTrackLoadedForPlayback() ) {
        if ( m_playbackCursorDragOwner == ownerId ) {
            m_isPlaybackCursorDragging = false;
            m_playbackCursorDragOwner  = 0;
            m_hasPendingPlaybackSeek   = false;
        }
        return;
    }

    const PlaybackTimelineState playbackState = readPlaybackTimelineState();
    const double totalTime = std::max(m_duration, playbackState.totalTime);
    if ( totalTime <= 0.0 ) {
        if ( m_playbackCursorDragOwner == ownerId ) {
            m_isPlaybackCursorDragging = false;
            m_playbackCursorDragOwner  = 0;
            m_hasPendingPlaybackSeek   = false;
        }
        return;
    }

    if ( m_isPlaybackCursorDragging && m_playbackCursorDragOwner != ownerId ) {
        return;
    }

    const double canvasTime  = playbackState.visualTime;
    const bool cursorVisible = canvasTime >= viewStart && canvasTime <= viewEnd;
    const float lineX =
        rectMin.x +
        static_cast<float>((canvasTime - viewStart) / (viewEnd - viewStart) *
                           (rectMax.x - rectMin.x));
    const ImVec2 handleMin(lineX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH - 2.0f,
                           rectMin.y);
    const ImVec2 handleMax(lineX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH + 2.0f,
                           rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT + 3.0f);
    const bool   hoverHandle =
        cursorVisible && ImGui::IsMouseHoveringRect(handleMin, handleMax, true);

    if ( hoverHandle || m_isPlaybackCursorDragging ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if ( hoverHandle && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        m_isPlaybackCursorDragging = true;
        m_isTimelinePanning        = false;
        m_playbackCursorDragOwner  = ownerId;
        m_pendingPlaybackSeekCanvasTime =
            std::clamp<double>(canvasTime, 0.0, playbackCanvasDuration());
        m_hasPendingPlaybackSeek = true;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        if ( m_playbackCursorDragOwner == ownerId ) {
            if ( m_hasPendingPlaybackSeek ) {
                seekPlaybackToCanvasTime(m_pendingPlaybackSeekCanvasTime);
            }
            m_isPlaybackCursorDragging = false;
            m_playbackCursorDragOwner  = 0;
            m_hasPendingPlaybackSeek   = false;
        }
        return;
    }

    if ( !m_isPlaybackCursorDragging ) {
        return;
    }

    ImGuiIO&     io        = ImGui::GetIO();
    const double rectWidth = std::max(1.0f, rectMax.x - rectMin.x);
    const double relX =
        std::clamp<double>((io.MousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
    const double targetCanvasTime   = viewStart + relX * (viewEnd - viewStart);
    m_pendingPlaybackSeekCanvasTime = targetCanvasTime;
    m_hasPendingPlaybackSeek        = true;

    const float previewX =
        rectMin.x +
        static_cast<float>((targetCanvasTime - viewStart) /
                           (viewEnd - viewStart) * (rectMax.x - rectMin.x));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(rectMin, rectMax, true);
    drawList->AddLine(ImVec2(previewX, rectMin.y),
                      ImVec2(previewX, rectMax.y),
                      IM_COL32(255, 80, 80, 145),
                      2.0f);
    drawList->AddTriangleFilled(
        ImVec2(previewX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH, rectMin.y),
        ImVec2(previewX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH, rectMin.y),
        ImVec2(previewX, rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT),
        IM_COL32(255, 80, 80, 180));
    drawList->PopClipRect();

    ImGui::SetTooltip("%s", Canvas::formatCanvasTime(targetCanvasTime).c_str());
}

/// @brief 处理分析视图的滚轮缩放和鼠标拖动平移。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算。
void BpmMeasurementToolView::handleTimelineNavigation(const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double        viewEnd)
{
    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 || rectMax.x <= rectMin.x ||
         viewEnd <= viewStart ) {
        m_isTimelinePanning = false;
        return;
    }

    if ( m_isPlaybackCursorDragging || m_isBeatMarkerDragging ) {
        m_isTimelinePanning = false;
        return;
    }

    ImGuiIO&   io    = ImGui::GetIO();
    const bool hover = ImGui::IsMouseHoveringRect(rectMin, rectMax);
    if ( hover || m_isTimelinePanning ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    const double rectWidth   = std::max(1.0f, rectMax.x - rectMin.x);
    const double viewRange   = viewEnd - viewStart;
    auto         clampCenter = [&](double center) {
        return std::clamp(center, 0.0, std::max(0.0, canvasDuration));
    };

    if ( hover && io.MouseWheel != 0.0f ) {
        const double mouseRatio = std::clamp<double>(
            (io.MousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
        const double anchorTime = viewStart + mouseRatio * viewRange;
        const double zoomFactor = std::pow(0.86, io.MouseWheel);
        const double maxZoom = std::max(0.1, std::max(canvasDuration, 120.0));
        const double nextZoom =
            std::clamp(m_zoomSeconds * zoomFactor, 0.01, maxZoom);
        const double nextViewStart = anchorTime - mouseRatio * nextZoom * 2.0;
        m_zoomSeconds              = nextZoom;
        m_viewCenter               = clampCenter(nextViewStart + nextZoom);
    }

    if ( hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
        m_isTimelinePanning = true;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_isTimelinePanning = false;
    }

    if ( m_isTimelinePanning &&
         ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ) {
        const double timeDelta =
            -static_cast<double>(io.MouseDelta.x) / rectWidth * viewRange;
        m_viewCenter = clampCenter(m_viewCenter + timeDelta);
    }
}

/// @brief 查找当前选中的音频资源。
/// @return 成功时返回音频资源副本，否则返回空。
std::optional<AudioResource>
BpmMeasurementToolView::selectedAudioResource() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        return std::nullopt;
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            return resource;
        }
    }

    return std::nullopt;
}

/// @brief 确保当前选中音轨已加载到播放图。
/// @return 加载成功或已经加载时返回 true。
bool BpmMeasurementToolView::loadSelectedTrackForPlayback()
{
    (void)ensureMetronomeSoundEffects();

    auto resource = selectedAudioResource();
    auto path     = selectedAudioAbsolutePath();
    if ( !resource || !path ) {
        m_statusText = TR("ui.tools.bpm_measure.select_track").data();
        return false;
    }

    std::error_code ec;
    if ( !std::filesystem::exists(*path, ec) || ec ) {
        m_statusText = TR("ui.tools.bpm_measure.file_missing").data();
        return false;
    }

    const std::string pathString = Config::pathToUtf8(*path);
    auto&             audio      = Audio::AudioManager::instance();
    if ( audio.getLoadedBGMPath() != pathString ) {
        if ( !audio.loadBGM(pathString, resource->m_config) ) {
            m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
            return false;
        }
    }

    if ( Logic::EditorEngine::instance().getActiveSessionIndex() >= 0 ) {
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSetPlaybackSpeed{ m_playbackSpeed });
    } else {
        audio.setPlaybackSpeed(m_playbackSpeed);
    }
    return true;
}

/// @brief 判断播放图当前加载的是否为选中音轨。
/// @return 当前加载音轨与选中音轨路径一致时返回 true。
/// @warning UI 热路径：每帧读取播放路径；不得在此加入文件存在性检查或音频加载。
bool BpmMeasurementToolView::isSelectedTrackLoadedForPlayback() const
{
    auto path = selectedAudioAbsolutePath();
    if ( !path ) {
        return false;
    }

    return Audio::AudioManager::instance().getLoadedBGMPath() ==
           Config::pathToUtf8(*path);
}

/// @brief 应用 BPM 工具的本地试听倍速。
/// @param speed 目标倍速。
void BpmMeasurementToolView::applyPlaybackSpeed(double speed)
{
    m_playbackSpeed = std::clamp(speed, 0.25, 2.0);
    if ( isSelectedTrackLoadedForPlayback() ) {
        if ( Logic::EditorEngine::instance().getActiveSessionIndex() >= 0 ) {
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdSetPlaybackSpeed{ m_playbackSpeed });
        } else {
            Audio::AudioManager::instance().setPlaybackSpeed(m_playbackSpeed);
        }
    }
}

/// @brief 获取当前配置下的视觉偏移，单位为秒。
/// @return 音频时间转换为视觉时间时需要叠加的偏移。
double BpmMeasurementToolView::playbackVisualOffset() const
{
    return Config::AppConfig::instance()
        .getVisualConfig()
        .getEffectiveVisualOffset();
}

/// @brief 获取当前音频对应的 BPM 工具画布时间轴总长度。
/// @return 画布时间轴上可显示的最大时间。
double BpmMeasurementToolView::playbackCanvasDuration() const
{
    return std::max(0.0, m_duration);
}

/// @brief 跳转到指定音频时间并同步活动主画布。
/// @param audioTime 目标音频时间，单位为秒。
void BpmMeasurementToolView::seekPlaybackToAudioTime(double audioTime)
{
    auto&        audio        = Audio::AudioManager::instance();
    const double totalTime    = std::max(m_duration, audio.getTotalTime());
    const double visualOffset = playbackVisualOffset();
    double       minTime      = -visualOffset;
    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    const double commandAudioTime =
        std::clamp(audioTime, minTime, std::max(minTime, totalTime));
    const double hardwareAudioTime =
        std::clamp(commandAudioTime, 0.0, std::max(0.0, totalTime));
    audio.seek(hardwareAudioTime);

    const double canvasDuration = playbackCanvasDuration();
    m_viewCenter                = std::clamp<double>(
        commandAudioTime + visualOffset, 0.0, std::max(0.0, canvasDuration));
    resetMetronomeScheduler(commandAudioTime);

    if ( Logic::EditorEngine::instance().getActiveSessionIndex() >= 0 ) {
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSeek{ commandAudioTime });
    }
}

/// @brief 跳转到指定 BPM 工具画布时间并同步活动主画布。
/// @param canvasTime 目标画布时间，单位为秒。
void BpmMeasurementToolView::seekPlaybackToCanvasTime(double canvasTime)
{
    seekPlaybackToAudioTime(canvasTime - playbackVisualOffset());
}

/// @brief 切换试听和活动主画布的播放状态。
/// @param shouldPlay true 表示播放，false 表示暂停。
void BpmMeasurementToolView::setPlaybackState(bool shouldPlay)
{
    auto& audio = Audio::AudioManager::instance();
    if ( !shouldPlay ) {
        m_metronomeScheduleInitialized = false;
    }

    if ( Logic::EditorEngine::instance().getActiveSessionIndex() >= 0 ) {
        if ( shouldPlay ) {
            (void)ensureMetronomeSoundEffects();
            const PlaybackTimelineState playbackState =
                readPlaybackTimelineState();
            const double canvasTime =
                std::clamp<double>(playbackState.visualTime,
                                   0.0,
                                   std::max(0.0, playbackCanvasDuration()));
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdSeek{ canvasTime - playbackVisualOffset() });
        }
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSetPlayState{ shouldPlay });
        return;
    }

    if ( shouldPlay ) {
        (void)ensureMetronomeSoundEffects();
        const PlaybackTimelineState playbackState = readPlaybackTimelineState();
        const double                canvasTime =
            std::clamp<double>(playbackState.visualTime,
                               0.0,
                               std::max(0.0, playbackCanvasDuration()));
        seekPlaybackToCanvasTime(canvasTime);
        audio.play();
    } else {
        audio.pause();
    }
}

/// @brief 请求重新分析当前选择的音频轨道。
/// @param autoMeasure 是否在分析完成后自动估算 BPM 和 offset。
void BpmMeasurementToolView::requestAnalyzeSelectedTrack(bool autoMeasure)
{
    stopAnalysisWorker();
    clearAnalysisData();

    auto path = selectedAudioAbsolutePath();
    if ( !path ) {
        m_statusText = TR("ui.tools.bpm_measure.select_track").data();
        return;
    }

    std::error_code ec;
    if ( !std::filesystem::exists(*path, ec) || ec ) {
        m_statusText = TR("ui.tools.bpm_measure.file_missing").data();
        return;
    }

    const std::string pathString = Config::pathToUtf8(*path);
    auto              track =
        Audio::AudioManager::instance().loadTrackForAnalysis(pathString);
    if ( !track ) {
        m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
        return;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    m_duration = sampleRate > 0.0
                     ? static_cast<double>(track->num_frames()) / sampleRate
                     : 0.0;
    m_firstBeatTime = clampFirstBeatTime(
        m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
    m_viewCenter = std::clamp<double>(
        m_firstBeatTime, 0.0, std::max(0.0, playbackCanvasDuration()));
    m_statusText = autoMeasure
                       ? TR("ui.tools.bpm_measure.auto_analyzing").data()
                       : TR("ui.tools.bpm_measure.analyzing").data();
    m_analysisProgress.store(0.0f, std::memory_order_relaxed);
    m_analysisFinished.store(false, std::memory_order_release);
    m_analysisRunning.store(true, std::memory_order_relaxed);

    const auto spectrumProfile = Config::spectrumDetailProfile(
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel);

    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        m_analysisRunning.store(false, std::memory_order_relaxed);
        m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
        XERROR("AppThreadPool is not initialized before BPM analysis.");
        return;
    }

    m_analysisStopSource            = std::stop_source{};
    const std::stop_token stopToken = m_analysisStopSource.get_token();
    m_analysisFuture = appThreadPool->enqueue([this,
                                               stopToken,
                                               track    = std::move(track),
                                               duration = m_duration,
                                               autoMeasure,
                                               spectrumProfile]() {
        analyzeTrack(stopToken, track, duration, autoMeasure, spectrumProfile);
    });
}

/// @brief 从后台线程发布一次分析失败结果。
/// @param autoMeasure 本次任务是否属于自动 BPM/offset 测量。
/// @warning 后台线程路径：只写入受互斥锁保护的待消费结果和原子状态。
void BpmMeasurementToolView::publishAnalysisFailure(bool autoMeasure)
{
    AnalysisResult result;
    result.autoTimingRequested = autoMeasure;
    result.failed              = true;

    {
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        m_pendingResult = std::move(result);
    }
    m_analysisProgress.store(1.0f, std::memory_order_relaxed);
    m_analysisRunning.store(false, std::memory_order_relaxed);
    m_analysisFinished.store(true, std::memory_order_release);
}

/// @brief 请求自动测量当前选择的音频轨道。
void BpmMeasurementToolView::requestAutoMeasureSelectedTrack()
{
    if ( m_selectedAudioTrackId.empty() ) {
        const std::string targetAudioTrackId = defaultAudioTrackId();
        if ( targetAudioTrackId.empty() ) {
            m_statusText = TR("ui.tools.bpm_measure.no_audio").data();
            return;
        }
        m_selectedAudioTrackId = targetAudioTrackId;
    }

    requestAnalyzeSelectedTrack(true);
}

/// @brief 查找当前项目默认用于 BPM 自动测量的音频资源 ID。
/// @return 优先返回主音轨 ID，否则返回首个音频资源 ID；不存在时为空。
std::string BpmMeasurementToolView::defaultAudioTrackId() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_audioResources.empty() ) {
        return {};
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_type == AudioTrackType::Main ) {
            return resource.m_id;
        }
    }

    return project->m_audioResources.front().m_id;
}

/// @brief 停止并等待当前后台分析任务。
/// @warning
/// 不可中断低频路径：会等待后台线程退出，只能在关闭窗口或重新选择音轨时执行。
void BpmMeasurementToolView::stopAnalysisWorker()
{
    if ( m_analysisFuture.valid() ) {
        m_analysisStopSource.request_stop();
        m_analysisFuture.wait();
        m_analysisFuture = std::future<void>{};
    }
    m_analysisRunning.store(false, std::memory_order_relaxed);
}

/// @brief 清理当前分析缓存并延迟释放频谱 GPU 资源。
/// @warning 低频资源路径：由用户切换音轨或重新分析触发；GPU 纹理只打脏位，
/// 实际释放必须延迟到下一次资源准备阶段，避免当前 ImGui draw list 仍引用旧
/// descriptor set。
void BpmMeasurementToolView::clearAnalysisData()
{
    m_waveTimes.clear();
    m_waveCanvasTimes.clear();
    m_waveCanvasTimesOffset = std::numeric_limits<double>::quiet_NaN();
    m_waveMin.clear();
    m_waveMax.clear();
    m_pendingSpectrumChunks.clear();
    m_nextSpectrumChunkUploadIndex = 0;
    m_spectrumTextureReloadStarted = false;
    m_texturesNeedReload           = !m_spectrumTextures.empty();
    m_spectrumSegmentCount         = 0;
    m_spectrumBinCount             = 0;
    m_analysisProgress             = 0.0f;
    m_analysisFinished             = false;
}

/// @brief 后台分析线程执行体。
/// @param stopToken 线程停止令牌。
/// @param track 待分析音频轨道，后台线程持有共享所有权。
/// @param duration 音频时长，单位为秒。
/// @param autoMeasure 是否在频谱分析后继续执行自动 BPM/offset 测量。
/// @warning 后台耗时路径：执行完整音频解码和 FFT；不在 UI/渲染热路径中运行。
void BpmMeasurementToolView::analyzeTrack(
    std::stop_token stopToken, std::shared_ptr<ice::AudioTrack> track,
    double duration, bool autoMeasure,
    Config::SpectrumDetailProfile spectrumProfile)
{
    if ( !track ) {
        publishAnalysisFailure(autoMeasure);
        return;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const size_t totalFrames = track->num_frames();
    if ( duration <= 0.0 || sampleRate <= 0.0 || totalFrames == 0 ) {
        publishAnalysisFailure(autoMeasure);
        return;
    }

    /// @brief 后台分析失败兜底，避免异常或早退后 UI 长期停在分析状态。
    struct AnalysisFailureGuard {
        /// @brief 当前 BPM 工具实例。
        BpmMeasurementToolView& view;

        /// @brief 是否属于自动 BPM/offset 测量任务。
        bool autoMeasure{ false };

        /// @brief 仍需要在析构时发布失败。
        bool active{ true };

        /// @brief 任务成功完成，不再发布失败。
        void dismiss() { active = false; }

        /// @brief 任务被取消，只清理运行状态。
        void cancel()
        {
            active = false;
            view.m_analysisRunning.store(false, std::memory_order_relaxed);
        }

        /// @brief 兜底发布失败结果。
        ~AnalysisFailureGuard()
        {
            if ( active ) {
                view.publishAnalysisFailure(autoMeasure);
            }
        }
    } failureGuard{ *this, autoMeasure };

    const int wavePointCount =
        std::max(2, static_cast<int>(duration * m_wavePointsPerSecond) + 1);
    const double spectrumSegmentsPerSecond = spectrumProfile.segmentsPerSecond;
    const int    spectrumSegmentCount =
        std::max(1, static_cast<int>(duration * spectrumSegmentsPerSecond) + 1);
    const int    spectrumBinCount = spectrumProfile.frequencyBins;
    const int    fftSize          = 2048;
    const size_t hopSize          = std::max<size_t>(
        1, static_cast<size_t>(sampleRate / spectrumSegmentsPerSecond));
    const uint16_t channelCount = ice::ICEConfig::internal_format.channels;
    const int      totalWork =
        wavePointCount + spectrumSegmentCount + (autoMeasure ? 1 : 0);
    int finishedWork = 0;

    AnalysisResult result;
    result.duration                  = duration;
    result.spectrumSegmentsPerSecond = spectrumSegmentsPerSecond;
    result.spectrumSegmentCount      = spectrumSegmentCount;
    result.spectrumBinCount          = spectrumBinCount;
    result.autoTimingRequested       = autoMeasure;
    result.waveTimes.resize(wavePointCount);
    result.waveMin.assign(wavePointCount, 0.0);
    result.waveMax.assign(wavePointCount, 0.0);

    auto updateProgress = [&]() {
        if ( totalWork <= 0 ) {
            return;
        }
        m_analysisProgress.store(std::clamp(static_cast<float>(finishedWork) /
                                                static_cast<float>(totalWork),
                                            0.0f,
                                            1.0f),
                                 std::memory_order_relaxed);
    };

    ice::AudioBuffer waveBuffer;
    for ( int point = 0; point < wavePointCount; ++point ) {
        if ( stopToken.stop_requested() ) {
            failureGuard.cancel();
            return;
        }

        const double timeStart =
            static_cast<double>(point) / m_wavePointsPerSecond;
        const double timeEnd =
            static_cast<double>(point + 1) / m_wavePointsPerSecond;
        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(timeStart * sampleRate));
        const size_t endFrame =
            std::min(totalFrames, static_cast<size_t>(timeEnd * sampleRate));
        const size_t frameCount =
            endFrame > startFrame ? endFrame - startFrame : 0;

        result.waveTimes[point] = timeStart;
        if ( frameCount > 0 ) {
            waveBuffer.resize(ice::ICEConfig::internal_format, frameCount);
            waveBuffer.clear();
            const size_t decoded =
                track->read(waveBuffer, startFrame, frameCount);
            const size_t usableFrames = std::min(decoded, frameCount);
            double       minValue     = 0.0;
            double       maxValue     = 0.0;
            float**      data         = waveBuffer.raw_ptrs();

            for ( size_t frame = 0; frame < usableFrames; ++frame ) {
                double mixed = 0.0;
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][frame];
                }
                mixed /= std::max<uint16_t>(1, channelCount);
                minValue = std::min(minValue, mixed);
                maxValue = std::max(maxValue, mixed);
            }
            result.waveMin[point] = minValue;
            result.waveMax[point] = maxValue;
        }

        ++finishedWork;
        updateProgress();
    }

    std::vector<float> heatmap(
        static_cast<size_t>(spectrumBinCount) * spectrumSegmentCount, -100.0f);
    std::vector<double> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
    }

    double* fftInput =
        static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
    fftw_complex* fftOutput = static_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1)));
    if ( !fftInput || !fftOutput ) {
        if ( fftInput ) {
            fftw_free(fftInput);
        }
        if ( fftOutput ) {
            fftw_free(fftOutput);
        }
        return;
    }

    fftw_plan fftPlan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftPlan =
            fftw_plan_dft_r2c_1d(fftSize, fftInput, fftOutput, FFTW_ESTIMATE);
    }

    if ( !fftPlan ) {
        fftw_free(fftInput);
        fftw_free(fftOutput);
        return;
    }

    ice::AudioBuffer spectrumBuffer;
    spectrumBuffer.resize(ice::ICEConfig::internal_format, fftSize);

    const double fmin         = 20.0;
    const double fmax         = m_maxFrequency;
    const double freqRange    = std::max(1.0, fmax - fmin);
    const double logBias      = m_logFrequencyBias;
    const double expKMinus1   = std::exp(logBias) - 1.0;
    auto         binFrequency = [&](double progress) {
        if ( std::abs(logBias) < 1e-4 ) {
            return fmin + freqRange * progress;
        }
        return fmin +
               freqRange * (std::exp(logBias * progress) - 1.0) / expKMinus1;
    };

    for ( int segment = 0; segment < spectrumSegmentCount; ++segment ) {
        if ( stopToken.stop_requested() ) {
            {
                std::lock_guard<std::mutex> lock(fftwPlanMutex());
                fftw_destroy_plan(fftPlan);
            }
            fftw_free(fftInput);
            fftw_free(fftOutput);
            failureGuard.cancel();
            return;
        }

        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(segment) * hopSize);
        const size_t frameCount =
            startFrame < totalFrames
                ? std::min<size_t>(fftSize, totalFrames - startFrame)
                : 0;

        spectrumBuffer.clear();
        const size_t decoded =
            frameCount > 0 ? track->read(spectrumBuffer, startFrame, frameCount)
                           : 0;
        float** data = spectrumBuffer.raw_ptrs();
        for ( int i = 0; i < fftSize; ++i ) {
            double mixed = 0.0;
            if ( static_cast<size_t>(i) < decoded ) {
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][i];
                }
                mixed /= std::max<uint16_t>(1, channelCount);
            }
            fftInput[i] = mixed * window[i];
        }

        fftw_execute(fftPlan);

        for ( int bin = 0; bin < spectrumBinCount; ++bin ) {
            const double freqStart =
                binFrequency(static_cast<double>(bin) / spectrumBinCount);
            const double freqEnd =
                binFrequency(static_cast<double>(bin + 1) / spectrumBinCount);
            const int binStart =
                std::clamp(static_cast<int>(freqStart * fftSize / sampleRate),
                           0,
                           fftSize / 2);
            const int binEnd =
                std::clamp(static_cast<int>(freqEnd * fftSize / sampleRate),
                           binStart,
                           fftSize / 2);

            double maxMagnitude = 0.0;
            for ( int i = binStart; i <= binEnd; ++i ) {
                const double magSq = fftOutput[i][0] * fftOutput[i][0] +
                                     fftOutput[i][1] * fftOutput[i][1];
                maxMagnitude       = std::max(maxMagnitude, magSq);
            }
            const double db =
                maxMagnitude > 1e-9
                    ? 20.0 * std::log10(std::sqrt(maxMagnitude) / fftSize)
                    : -100.0;
            heatmap[static_cast<size_t>(bin) * spectrumSegmentCount + segment] =
                static_cast<float>(std::clamp(db, -100.0, 0.0));
        }

        ++finishedWork;
        updateProgress();
    }

    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftw_destroy_plan(fftPlan);
    }
    fftw_free(fftInput);
    fftw_free(fftOutput);

    if ( autoMeasure ) {
        if ( duration >= 10.0 ) {
            if ( auto monoSamples =
                     readMonoSamplesForAutoTiming(stopToken, track) ) {
                if ( !stopToken.stop_requested() ) {
                    result.autoTimingResult = BpmAutoDetector::detect(
                        *monoSamples,
                        ice::ICEConfig::internal_format.samplerate);
                }
            }
        }

        ++finishedWork;
        updateProgress();
    }

    const int chunkCount =
        (spectrumSegmentCount + static_cast<int>(MAX_TEXTURE_W) - 1) /
        static_cast<int>(MAX_TEXTURE_W);
    result.spectrumChunks.reserve(static_cast<size_t>(chunkCount));

    for ( int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex ) {
        const uint32_t chunkStart =
            static_cast<uint32_t>(chunkIndex) * MAX_TEXTURE_W;
        const uint32_t chunkWidth = std::min<uint32_t>(
            MAX_TEXTURE_W,
            static_cast<uint32_t>(spectrumSegmentCount) - chunkStart);

        TextureChunkData chunk;
        chunk.width  = chunkWidth;
        chunk.height = static_cast<uint32_t>(spectrumBinCount);
        chunk.pixels.resize(static_cast<size_t>(chunk.width) * chunk.height *
                            4);

        for ( uint32_t y = 0; y < chunk.height; ++y ) {
            const int bin = spectrumBinCount - 1 - static_cast<int>(y);
            for ( uint32_t x = 0; x < chunk.width; ++x ) {
                const uint32_t globalX = chunkStart + x;
                const float    value =
                    heatmap[static_cast<size_t>(bin) * spectrumSegmentCount +
                            globalX];
                const auto   color = spectrumColorFromDb(value);
                const size_t offset =
                    (static_cast<size_t>(y) * chunk.width + x) * 4;
                std::memcpy(&chunk.pixels[offset], color.data(), 4);
            }
        }

        result.spectrumChunks.push_back(std::move(chunk));
    }

    m_analysisProgress.store(1.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        m_pendingResult = std::move(result);
    }
    failureGuard.dismiss();
    m_analysisRunning.store(false, std::memory_order_relaxed);
    m_analysisFinished.store(true, std::memory_order_release);
}

/// @brief 读取完整音轨并混合为单声道采样，供自动 BPM 检测使用。
/// @param stopToken 后台线程停止令牌。
/// @param track 待读取的音频轨道。
/// @return 成功时返回单声道采样，否则返回空。
/// @warning 后台耗时路径：会读取完整音频，只能由手动触发的分析任务调用。
std::optional<std::vector<float>>
BpmMeasurementToolView::readMonoSamplesForAutoTiming(
    std::stop_token                         stopToken,
    const std::shared_ptr<ice::AudioTrack>& track) const
{
    if ( !track ) {
        return std::nullopt;
    }

    const size_t totalFrames = track->num_frames();
    if ( totalFrames == 0 ) {
        return std::nullopt;
    }

    const uint16_t channelCount = ice::ICEConfig::internal_format.channels;
    if ( channelCount == 0 ) {
        return std::nullopt;
    }

    constexpr size_t   AUTO_TIMING_READ_CHUNK_FRAMES = 32768;
    std::vector<float> monoSamples(totalFrames, 0.0f);
    ice::AudioBuffer   buffer;

    size_t readOffset = 0;
    while ( readOffset < totalFrames ) {
        if ( stopToken.stop_requested() ) {
            return std::nullopt;
        }

        const size_t frameCount =
            std::min(AUTO_TIMING_READ_CHUNK_FRAMES, totalFrames - readOffset);
        buffer.resize(ice::ICEConfig::internal_format, frameCount);
        buffer.clear();

        const size_t decoded = track->read(buffer, readOffset, frameCount);
        if ( decoded == 0 ) {
            break;
        }

        float** data = buffer.raw_ptrs();
        for ( size_t frame = 0; frame < decoded; ++frame ) {
            double mixed = 0.0;
            for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                mixed += data[ch][frame];
            }
            monoSamples[readOffset + frame] =
                static_cast<float>(mixed / channelCount);
        }

        readOffset += decoded;
    }

    if ( readOffset == 0 ) {
        return std::nullopt;
    }
    if ( readOffset < monoSamples.size() ) {
        monoSamples.resize(readOffset);
    }
    return monoSamples;
}

/// @brief 查找当前选中音频轨道的绝对路径。
/// @return 成功时返回绝对路径，否则返回空。
std::optional<std::filesystem::path>
BpmMeasurementToolView::selectedAudioAbsolutePath() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        return std::nullopt;
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            return project->m_projectRoot / Config::utf8ToPath(resource.m_path);
        }
    }

    return std::nullopt;
}

/// @brief 将 dB 值映射为热力图 RGBA 颜色。
/// @param db 频谱能量，单位为 dB。
/// @return RGBA 颜色。
std::array<unsigned char, 4> BpmMeasurementToolView::spectrumColorFromDb(
    double db) const
{
    const double scaleMin = -80.0;
    const double scaleMax = -8.0;
    const double t =
        std::clamp((db - scaleMin) / (scaleMax - scaleMin), 0.0, 1.0);
    const double r = std::clamp(3.0 * t, 0.0, 1.0);
    const double g = std::clamp(3.0 * t - 1.0, 0.0, 1.0);
    const double b = std::clamp(3.0 * t - 2.0, 0.0, 1.0);

    return { static_cast<unsigned char>(r * 255.0),
             static_cast<unsigned char>(g * 255.0),
             static_cast<unsigned char>(b * 255.0),
             255 };
}

}  // namespace MMM::UI
