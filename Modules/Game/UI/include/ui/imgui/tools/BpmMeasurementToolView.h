#pragma once

#include "graphic/imguivk/VKTexture.h"
#include "ui/ITextureLoader.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
    };

    /// @brief 尝试消费后台分析结果。
    void consumePendingAnalysis();

    /// @brief 绘制右侧测量参数面板。
    void renderControlPanel();

    /// @brief 绘制左侧波形和频谱面板。
    void renderAnalysisPanel();

    /// @brief 绘制波形图。
    /// @param size 绘制区域尺寸。
    void renderWaveformPlot(const ImVec2& size);

    /// @brief 绘制频谱图。
    /// @param size 绘制区域尺寸。
    void renderSpectrumImage(const ImVec2& size);

    /// @brief 在指定矩形区域叠加拍线和黄色拍框。
    /// @param drawList 目标 ImGui 绘制列表。
    /// @param rectMin 绘制区域左上角。
    /// @param rectMax 绘制区域右下角。
    /// @param viewStart 当前视图起始时间，单位为秒。
    /// @param viewEnd 当前视图结束时间，单位为秒。
    void drawBeatMarkers(ImDrawList& drawList, const ImVec2& rectMin,
                         const ImVec2& rectMax, double viewStart,
                         double viewEnd) const;

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
    void requestAnalyzeSelectedTrack();

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
    /// @warning 后台耗时路径：执行完整音频解码和 FFT；不在
    /// UI/渲染热路径中运行。
    void analyzeTrack(std::stop_token                  stopToken,
                      std::shared_ptr<ice::AudioTrack> track, double duration);

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

    /// @brief 波形下包络缓存。
    std::vector<double> m_waveMin;

    /// @brief 波形上包络缓存。
    std::vector<double> m_waveMax;

    /// @brief 已上传的频谱纹理分块。
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_spectrumTextures;

    /// @brief 待上传的频谱纹理分块数据。
    std::vector<TextureChunkData> m_pendingSpectrumChunks;

    /// @brief 当前音频总时长，单位为秒。
    double m_duration{ 0.0 };

    /// @brief 当前视图中心时间，单位为秒。
    double m_viewCenter{ 0.0 };

    /// @brief 当前视图半宽时间，单位为秒。
    double m_zoomSeconds{ 8.0 };

    /// @brief 用户输入的 BPM。
    double m_bpm{ 120.0 };

    /// @brief 单拍时长，单位为秒。
    double m_beatLengthSeconds{ 0.5 };

    /// @brief 首拍位置，单位为秒。
    double m_firstBeatTime{ 0.0 };

    /// @brief 黄色拍框宽度，单位为毫秒。
    double m_markerWidthMs{ 80.0 };

    /// @brief 当前是否正在拖动分析视图时间轴。
    bool m_isTimelinePanning{ false };

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

    /// @brief 后台分析线程。
    std::unique_ptr<std::jthread> m_analysisThread;

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
