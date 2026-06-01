#pragma once

#include "config/VisualConfig.h"
#include "graphic/imguivk/VKTexture.h"
#include "mmm/project/AudioResource.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/tools/BpmAutoDetector.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ice
{
class AudioTrack;
}  // namespace ice

namespace MMM::UI
{

/// @brief BPM 手动测量工具窗口，独立于播放控制器的音频分析视图。
class BpmMeasurementToolView : public ITextureLoader
{
public:
    /// @brief 构造 BPM 测量工具窗口。
    explicit BpmMeasurementToolView(const std::string& name);

    /// @brief 销毁窗口并等待后台分析任务和 GPU 资源释放。
    ~BpmMeasurementToolView() override;

    /// @brief 打开窗口并选中指定项目音频轨道。
    /// @param audioTrackId 项目内音频资源 ID；为空时仅打开窗口。
    void openWithAudioTrack(const std::string& audioTrackId);

    /// @brief 打开窗口并对指定或默认项目音频轨道执行自动 BPM 测量。
    /// @param audioTrackId 项目内音频资源 ID；为空时选择默认主音轨。
    void openWithAutoMeasurement(const std::string& audioTrackId);

    /// @brief 获取当前选中的项目音频资源 ID。
    /// @return 当前选中的音频资源 ID，未选择时为空。
    const std::string& getSelectedAudioTrackId() const
    {
        return m_selectedAudioTrackId;
    }

    /// @brief 更新并绘制 BPM 测量工具 UI。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI
    /// 热路径：每帧执行；不得在此处扫描文件系统、重新解码整段音频或创建 FFT
    /// 计划。
    void update(UIManager* sourceManager) override;

    /// @brief 判断频谱纹理是否需要上传。
    /// @warning 渲染准备热路径：每帧查询；只读取低频变更脏位。
    bool needReload() override;

    /// @brief 上传后台分析生成的频谱纹理。
    /// @param physicalDevice Vulkan 物理设备。
    /// @param logicalDevice Vulkan 逻辑设备。
    /// @param cmdPool 上传命令池。
    /// @param queue 上传队列。
    /// @warning 低频资源准备路径：可能等待 GPU
    /// 空闲并上传纹理，只能由音轨切换或重新分析触发。
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

    /// @brief 返回实际派生类实例指针，供禁用 RTTI 的 UIManager 下行转换使用。
    void* getActualInstance() override { return this; }

private:
    /// @brief 频谱贴图分块数据。
    struct TextureChunkData {
        /// @brief RGBA 像素数据。
        std::vector<unsigned char> pixels;

        /// @brief 分块宽度，单位为像素。
        uint32_t width{ 0 };

        /// @brief 分块高度，单位为像素。
        uint32_t height{ 0 };
    };

    /// @brief 后台音频分析结果。
    struct AnalysisResult {
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
    };

    /// @brief 尝试消费后台分析结果。
    void consumePendingAnalysis();

    /// @brief 绘制右侧测量参数面板。
    void renderControlPanel();

    /// @brief 绘制试听播放、暂停、进度和倍速控制。
    /// @warning UI
    /// 热路径：每帧执行；只读取播放状态和处理用户输入，文件检查仅在按钮触发后发生。
    void renderPlaybackControls();

    /// @brief 绘制左侧波形和频谱面板。
    void renderAnalysisPanel();

    /// @brief 播放时让分析视图自动跟随播放指针。
    /// @warning UI
    /// 热路径：每帧执行；只读取播放同步快照并更新视图中心，不能访问文件系统。
    void followPlaybackIfNeeded();

    /// @brief 更新 BPM 工具节拍器音效触发。
    /// @warning UI
    /// 热路径：每帧执行；只读取播放同步快照并播放已预加载音效，不访问文件系统。
    void updateMetronomePlayback();

    /// @brief 确保 BPM 工具节拍器音效已预加载。
    /// @return 两个节拍器音效均可播放时返回 true。
    bool ensureMetronomeSoundEffects();

    /// @brief 从当前音频调度时间重置节拍器触发游标。
    /// @param audioTime 当前音频调度时间，单位为秒。
    void resetMetronomeScheduler(double audioTime);

    /// @brief 更新波形绘制用的画布时间缓存。
    /// @param canvasOffset 画布时间相对音频采样时间的偏移，单位为秒。
    /// @warning UI
    /// 热路径：波形图每帧查询；仅在画布偏移或波形缓存变化时重建时间数组。
    void updateWaveCanvasTimes(double canvasOffset);

    /// @brief 绘制波形图。
    /// @param size 绘制区域尺寸。
    void renderWaveformPlot(const ImVec2& size);

    /// @brief 绘制频谱图。
    /// @param size 绘制区域尺寸。
    void renderSpectrumImage(const ImVec2& size);

    /// @brief 在指定矩形区域叠加拍线、黄色拍框和首拍红色覆盖框。
    /// @param drawList 目标 ImGui 绘制列表。
    /// @param rectMin 绘制区域左上角。
    /// @param rectMax 绘制区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    void drawBeatMarkers(ImDrawList& drawList, const ImVec2& rectMin,
                         const ImVec2& rectMax, double viewStart,
                         double viewEnd) const;

    /// @brief 在指定矩形区域叠加分拍线。
    /// @param drawList 目标 ImGui 绘制列表。
    /// @param rectMin 绘制区域左上角。
    /// @param rectMax 绘制区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    /// @warning UI
    /// 热路径：波形图和频谱图每帧执行；按当前视野增量绘制分拍线，不得加入音频解码或文件访问。
    void drawBeatSubdivisionLines(ImDrawList& drawList, const ImVec2& rectMin,
                                  const ImVec2& rectMax, double viewStart,
                                  double viewEnd) const;

    /// @brief 在指定矩形区域叠加当前音频播放指针。
    /// @param drawList 目标 ImGui 绘制列表。
    /// @param rectMin 绘制区域左上角。
    /// @param rectMax 绘制区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    /// @warning UI
    /// 热路径：波形图和频谱图每帧执行；只读取当前播放路径、播放时间并绘制播放指针。
    void drawPlaybackCursor(ImDrawList& drawList, const ImVec2& rectMin,
                            const ImVec2& rectMax, double viewStart,
                            double viewEnd) const;

    /// @brief 处理整拍线顶部三角手柄的拖拽，反向调整第一拍位置。
    /// @param rectMin 交互区域左上角。
    /// @param rectMax 交互区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    /// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
    /// @warning UI
    /// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算，不访问文件系统。
    void handleBeatMarkerDrag(const ImVec2& rectMin, const ImVec2& rectMax,
                              double viewStart, double viewEnd, int ownerId);

    /// @brief 处理播放指针顶部三角手柄的拖拽跳转。
    /// @param rectMin 交互区域左上角。
    /// @param rectMax 交互区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    /// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
    /// @warning UI
    /// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算，不访问文件系统。
    void handlePlaybackCursorDrag(const ImVec2& rectMin, const ImVec2& rectMax,
                                  double viewStart, double viewEnd,
                                  int ownerId);

    /// @brief 处理分析视图的滚轮缩放和鼠标拖动平移。
    /// @param rectMin 交互区域左上角。
    /// @param rectMax 交互区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    /// @warning UI
    /// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算。
    void handleTimelineNavigation(const ImVec2& rectMin, const ImVec2& rectMax,
                                  double viewStart, double viewEnd);

    /// @brief 请求重新分析当前选择的音频轨道。
    /// @param autoMeasure 是否在分析完成后自动估算 BPM 和 offset。
    void requestAnalyzeSelectedTrack(bool autoMeasure = false);

    /// @brief 请求自动测量当前选择的音频轨道。
    void requestAutoMeasureSelectedTrack();

    /// @brief 查找当前项目默认用于 BPM 自动测量的音频资源 ID。
    /// @return 优先返回主音轨 ID，否则返回首个音频资源 ID；不存在时为空。
    std::string defaultAudioTrackId() const;

    /// @brief 查找当前选中的音频资源。
    /// @return 成功时返回音频资源副本，否则返回空。
    std::optional<AudioResource> selectedAudioResource() const;

    /// @brief 确保当前选中音轨已加载到播放图。
    /// @return 加载成功或已经加载时返回 true。
    bool loadSelectedTrackForPlayback();

    /// @brief 判断播放图当前加载的是否为选中音轨。
    /// @return 当前加载音轨与选中音轨路径一致时返回 true。
    /// @warning UI
    /// 热路径：每帧读取播放路径；不得在此加入文件存在性检查或音频加载。
    bool isSelectedTrackLoadedForPlayback() const;

    /// @brief 应用 BPM 工具的本地试听倍速。
    /// @param speed 目标倍速。
    void applyPlaybackSpeed(double speed);

    /// @brief 获取当前配置下的视觉偏移，单位为秒。
    /// @return 音频时间转换为视觉时间时需要叠加的偏移。
    double playbackVisualOffset() const;

    /// @brief 获取当前音频对应的 BPM 工具画布时间轴总长度。
    /// @return 画布时间轴上可显示的最大时间。
    double playbackCanvasDuration() const;

    /// @brief 跳转到指定音频时间并同步活动主画布。
    /// @param audioTime 目标音频时间，单位为秒。
    void seekPlaybackToAudioTime(double audioTime);

    /// @brief 跳转到指定 BPM 工具画布时间并同步活动主画布。
    /// @param canvasTime 目标画布时间，单位为秒。
    void seekPlaybackToCanvasTime(double canvasTime);

    /// @brief 切换试听和活动主画布的播放状态。
    /// @param shouldPlay true 表示播放，false 表示暂停。
    void setPlaybackState(bool shouldPlay);

    /// @brief 停止并等待当前后台分析任务。
    /// @warning
    /// 不可中断低频路径：会等待后台线程退出，只能在关闭窗口或重新选择音轨时执行。
    void stopAnalysisWorker();

    /// @brief 清理当前分析缓存和频谱 GPU 资源。
    /// @warning 低频资源路径：可能等待 GPU 空闲，严禁放入每帧绘制路径。
    void clearAnalysisData();

    /// @brief 后台分析线程执行体。
    /// @param stopToken 线程停止令牌。
    /// @param track 待分析音频轨道，后台线程持有共享所有权。
    /// @param duration 音频时长，单位为秒。
    /// @param autoMeasure 是否在频谱分析后继续执行自动 BPM/offset 测量。
    /// @param spectrumProfile 计算时捕获的全局频谱精细度参数。
    /// @warning 后台耗时路径：执行完整音频解码和 FFT；不在
    /// UI/渲染热路径中运行。
    void analyzeTrack(std::stop_token                  stopToken,
                      std::shared_ptr<ice::AudioTrack> track, double duration,
                      bool                          autoMeasure,
                      Config::SpectrumDetailProfile spectrumProfile);

    /// @brief 读取完整音轨并混合为单声道采样，供自动 BPM 检测使用。
    /// @param stopToken 后台线程停止令牌。
    /// @param track 待读取的音频轨道。
    /// @return 成功时返回单声道采样，否则返回空。
    /// @warning 后台耗时路径：会读取完整音频，只能由手动触发的分析任务调用。
    std::optional<std::vector<float>> readMonoSamplesForAutoTiming(
        std::stop_token                         stopToken,
        const std::shared_ptr<ice::AudioTrack>& track) const;

    /// @brief 查找当前选中音频轨道的绝对路径。
    /// @return 成功时返回绝对路径，否则返回空。
    std::optional<std::filesystem::path> selectedAudioAbsolutePath() const;

    /// @brief 将 dB 值映射为热力图 RGBA 颜色。
    /// @param db 频谱能量，单位为 dB。
    /// @return RGBA 颜色。
    std::array<unsigned char, 4> spectrumColorFromDb(double db) const;

    /// @brief 频谱纹理最大分块宽度，避免超过硬件纹理限制。
    static constexpr uint32_t MAX_TEXTURE_W{ 16384 };

    /// @brief 当前选中的项目音频资源 ID。
    std::string m_selectedAudioTrackId;

    /// @brief 当前选中音频轨道显示名。
    std::string m_selectedAudioLabel;

    /// @brief 分析状态说明文本。
    std::string m_statusText;

    /// @brief 波形采样点时间缓存，单位为秒。
    std::vector<double> m_waveTimes;

    /// @brief 波形采样点的画布时间缓存，单位为秒。
    std::vector<double> m_waveCanvasTimes;

    /// @brief 波形下包络缓存。
    std::vector<double> m_waveMin;

    /// @brief 波形上包络缓存。
    std::vector<double> m_waveMax;

    /// @brief 已上传的频谱纹理分块。
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_spectrumTextures;

    /// @brief 待上传的频谱纹理分块数据。
    std::vector<TextureChunkData> m_pendingSpectrumChunks;

    /// @brief 频谱纹理分块上传是否已经进入进行中状态。
    bool m_spectrumTextureReloadStarted{ false };

    /// @brief 下一块待上传的频谱纹理分块索引。
    size_t m_nextSpectrumChunkUploadIndex{ 0 };

    /// @brief 当前音频总时长，单位为秒。
    double m_duration{ 0.0 };

    /// @brief 当前波形画布时间缓存使用的偏移。
    double m_waveCanvasTimesOffset{ std::numeric_limits<double>::quiet_NaN() };

    /// @brief 当前视图中心画布时间，单位为秒。
    double m_viewCenter{ 0.0 };

    /// @brief 当前视图半宽时间，单位为秒。
    double m_zoomSeconds{ 8.0 };

    /// @brief 用户输入的 BPM。
    double m_bpm{ 120.0 };

    /// @brief 单拍时长，单位为秒。
    double m_beatLengthSeconds{ 0.5 };

    /// @brief 首拍位置，使用 BPM 工具画布时间，单位为秒。
    double m_firstBeatTime{ 0.0 };

    /// @brief 黄色拍框宽度，单位为毫秒。
    double m_markerWidthMs{ 80.0 };

    /// @brief 分拍线切分数量。
    int m_beatDivisor{ 4 };

    /// @brief BPM 工具本地试听播放倍速，不写回项目音轨配置。
    double m_playbackSpeed{ 1.0 };

    /// @brief 节拍器音效是否已准备好。
    bool m_metronomeSfxReady{ false };

    /// @brief 节拍器触发器是否已有有效游标。
    bool m_metronomeScheduleInitialized{ false };

    /// @brief 节拍器下一次要触发的拍序，相对首拍位置。
    int64_t m_nextMetronomeBeatIndex{ 0 };

    /// @brief 上一帧节拍器看到的音频调度时间，单位为秒。
    double m_lastMetronomeAudioTime{ 0.0 };

    /// @brief 当前节拍器触发游标对应的首拍位置，单位为秒。
    double m_metronomeScheduledFirstBeatTime{
        std::numeric_limits<double>::quiet_NaN()
    };

    /// @brief 当前节拍器触发游标对应的单拍时长，单位为秒。
    double m_metronomeScheduledBeatLength{
        std::numeric_limits<double>::quiet_NaN()
    };

    /// @brief 当前是否正在拖动分析视图时间轴。
    bool m_isTimelinePanning{ false };

    /// @brief 当前是否正在拖动播放指针手柄。
    bool m_isPlaybackCursorDragging{ false };

    /// @brief 当前播放指针拖拽所属的视图标识，0 表示无拖拽。
    int m_playbackCursorDragOwner{ 0 };

    /// @brief 是否存在待鼠标松开时应用的播放跳转。
    bool m_hasPendingPlaybackSeek{ false };

    /// @brief 播放指针拖拽预览的目标画布时间，单位为秒。
    double m_pendingPlaybackSeekCanvasTime{ 0.0 };

    /// @brief 当前是否正在拖动整拍线手柄。
    bool m_isBeatMarkerDragging{ false };

    /// @brief 当前整拍线拖拽所属的视图标识，0 表示无拖拽。
    int m_beatMarkerDragOwner{ 0 };

    /// @brief 当前正在拖动的整拍索引。
    int64_t m_draggedBeatIndex{ 0 };

    /// @brief 波形缓存时间分辨率，单位为点/秒。
    double m_wavePointsPerSecond{ 200.0 };

    /// @brief 频谱缓存时间分辨率，单位为段/秒。
    double m_spectrumSegmentsPerSecond{ 100.0 };

    /// @brief 当前频谱总段数。
    int m_spectrumSegmentCount{ 0 };

    /// @brief 当前频谱频率 bin 数。
    int m_spectrumBinCount{ 0 };

    /// @brief 频谱最高显示频率，单位为 Hz。
    double m_maxFrequency{ 20000.0 };

    /// @brief 频谱对数频率映射偏置。
    double m_logFrequencyBias{ 6.91 };

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

    /// @brief 频谱纹理上传脏位。
    bool m_texturesNeedReload{ false };
};

}  // namespace MMM::UI
