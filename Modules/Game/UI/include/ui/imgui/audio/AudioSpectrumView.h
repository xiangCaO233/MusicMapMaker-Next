#pragma once

#include "config/VisualConfig.h"
#include "ui/ITextureLoader.h"
#include <atomic>
#include <fftw3.h>
#include <future>
#include <memory>
#include <stop_token>
#include <vector>

namespace ice
{
class GraphicEqualizer;
class AudioBuffer;
}  // namespace ice

namespace MMM::Graphic
{
class VKTexture;
}  // namespace MMM::Graphic

namespace MMM::UI
{

/// @brief 高精度频谱热力图视图
/// 通过 CPU 侧光栅化到 RGBA 像素缓冲区，
/// 再上传为 VKTexture 并以单张纹理绘制，
/// 达到专业音频编辑器 (如 Adobe Audition) 级别的精度与性能。
class AudioSpectrumView : public ITextureLoader
{
public:
    AudioSpectrumView(const std::string& name);
    ~AudioSpectrumView() override;

    /// @brief ImGui 更新
    void update(UIManager* sourceManager) override;

    /// @brief 是否需要重载纹理
    bool needReload() override;

    /// @brief 重载纹理 (由 UIManager 在准备资源阶段回调)
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

private:
    /// @brief 更新当前视图的频谱数据 (从缓存切片并光栅化到像素缓冲)
    void updateSpectrum(double currentTime, double totalTime);

    /// @brief 同步 EQ 参数到预览 EQ
    void syncEQ();

    /// @brief 准备 FFTW 计划 (线程安全: 仅在主线程调用 plan 创建)
    void prepareFFT(int fftSize);

    /// @brief 启动异步全局预计算 (非阻塞)
    void startAsyncRecalculate();

    /// @brief 后台线程执行体：多核并行 FFT 计算
    struct EQSettings {
        bool                enabled{ false };
        std::vector<double> freqs;
        std::vector<double> gains;
        std::vector<double> qs;
    };
    /// @brief 后台重新计算完整频谱缓存。
    /// @param eq 计算时捕获的 EQ 设置。
    /// @param maxFreq 最高显示频率。
    /// @param logBias 对数频率映射偏置。
    /// @param detailLevel 计算时捕获的频谱精细度枚举。
    /// @param detailProfile 计算时捕获的频谱精细度参数。
    /// @warning 后台耗时路径：执行完整音频 FFT 计算，不在 UI/渲染热路径运行。
    void backgroundRecalculate(std::stop_token stopToken, const EQSettings& eq,
                               float maxFreq, float logBias,
                               Config::SpectrumDetailLevel   detailLevel,
                               Config::SpectrumDetailProfile detailProfile);

    /// @brief 构建 "Hot" 色图查找表 (256 级)
    void buildColormapLUT();

    std::shared_ptr<ice::GraphicEqualizer> m_previewEQ;
    std::unique_ptr<ice::AudioBuffer>      m_processBuffer;
    std::unique_ptr<ice::AudioBuffer>      m_rawBuffer;

    // FFTW 资源 (仅主线程或已完成的后台线程使用)
    fftw_plan           m_fftPlan{ nullptr };
    double*             m_fftIn{ nullptr };
    fftw_complex*       m_fftOut{ nullptr };
    int                 m_currentFFTSize{ 0 };
    std::vector<double> m_window;

    // --- 全局缓存数据 ---
    /// @brief 高分辨率缓存 (频率 bins × 时间段)，行优先 [bin * totalSegments +
    /// t]
    std::vector<double> m_cachedHeatmapL;
    std::vector<double> m_cachedHeatmapR;

    /// @brief 当前缓存时间分辨率，单位为段/秒。
    double m_cacheSegmentsPerSecond{ 100.0 };

    /// @brief 缓存的总时间段数
    int m_cachedNumTotalSegments{ 0 };

    /// @brief 当前频率 bin 数，映射到 20Hz~20kHz 对数空间。
    int m_numFrequencyBins{ 128 };

    /// @brief 当前已生成频谱纹理对应的全局精细度。
    Config::SpectrumDetailLevel m_spectrumDetailLevel{
        Config::SpectrumDetailLevel::Balanced
    };

    /// @brief 后台计算完成后等待主线程提交的精细度。
    Config::SpectrumDetailLevel m_pendingSpectrumDetailLevel{
        Config::SpectrumDetailLevel::Balanced
    };

    /// @brief 后台计算完成后等待主线程提交的频谱时间分辨率。
    double m_pendingCacheSegmentsPerSecond{ 100.0 };

    /// @brief 后台计算完成后等待主线程提交的频率 bin 数。
    int m_pendingNumFrequencyBins{ 128 };

    // --- 异步计算状态 ---

    /// @brief 后台计算在线程池中的任务句柄。
    /// @warning 生命周期路径：仅由用户触发重算或窗口销毁时写入；任务体不在 UI
    /// 热路径执行。
    std::future<void> m_calcFuture;

    /// @brief 后台计算停止请求源。
    /// @warning 跨线程停止信号：UI 线程低频写入，后台 FFT
    /// 任务分段读取，只承载取消标志。
    std::stop_source m_calcStopSource;

    /// @brief 是否正在后台计算
    std::atomic<bool> m_isCalculating{ false };

    /// @brief 计算进度 [0.0, 1.0]
    std::atomic<float> m_calcProgress{ 0.0f };

    /// @brief 后台计算是否已完成 (主线程读取后重置)
    std::atomic<bool> m_calcFinished{ false };

    // --- 像素缓冲与纹理 (全量静态存储) ---

    /// @brief 纹理分块存储 (L/R 通道)
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_texturesL;
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_texturesR;

    /// @brief 纹理分块的数据缓冲区 (待上传)
    struct TextureChunkData {
        std::vector<unsigned char> pixels;
        uint32_t                   width;
        uint32_t                   height;
    };
    std::vector<TextureChunkData> m_pendingChunksL;
    std::vector<TextureChunkData> m_pendingChunksR;

    /// @brief 纹理是否需要重新加载 (全量)
    bool m_texturesNeedReload{ false };

    /// @brief 分块宽度 (通常取 16384 或更小以适配硬件限制)
    static constexpr uint32_t MAX_TEXTURE_W{ 16384 };

    /// @brief 构建全量像素缓冲并准备上传
    void prepareFullGlobalTextures();

    // --- 色图查找表 ---
    /// @brief 256 级预计算颜色表 (RGBA u8 × 4)
    std::array<std::array<unsigned char, 4>, 256> m_colormapLUT;

    // --- 视图状态 ---
    float m_zoom{ 1.0f };
    float m_maxFreq{ 20000.0f };
    float m_logBias{ 6.91f };
};

}  // namespace MMM::UI
