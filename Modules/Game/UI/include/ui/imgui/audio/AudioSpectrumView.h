#pragma once

#include "ui/ITextureLoader.h"
#include <fftw3.h>
#include <memory>
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

    /// @brief 准备 FFTW 计划
    void prepareFFT(int fftSize);

    /// @brief 全局预计算所有频谱数据 (冷加载)
    void fullRecalculate();

    /// @brief 构建 "Hot" 色图查找表 (256 级)
    void buildColormapLUT();

    std::shared_ptr<ice::GraphicEqualizer> m_previewEQ;
    std::unique_ptr<ice::AudioBuffer>      m_processBuffer;
    std::unique_ptr<ice::AudioBuffer>      m_rawBuffer;

    // FFTW 资源
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

    /// @brief 缓存时间分辨率 (段/秒)，200 接近专业编辑器水平
    double m_cacheSegmentsPerSecond{ 200.0 };

    /// @brief 缓存的总时间段数
    int m_cachedNumTotalSegments{ 0 };

    /// @brief 频率 bin 数 (行) — 高精度，映射到 20Hz~20kHz 对数空间
    int m_numFrequencyBins{ 256 };

    bool m_isCalculating{ false };

    // --- 像素缓冲与纹理 ---

    /// @brief CPU 侧 RGBA 像素缓冲 (L 通道)，尺寸 = texW × texH × 4
    std::vector<unsigned char> m_pixelBufferL;

    /// @brief CPU 侧 RGBA 像素缓冲 (R 通道)
    std::vector<unsigned char> m_pixelBufferR;

    /// @brief 当前纹理宽度 (像素)，对应可视窗口的时间跨度
    int m_texW{ 0 };

    /// @brief 当前纹理高度 (像素)，对应频率 bin 数
    int m_texH{ 0 };

    /// @brief L 通道的 GPU 纹理
    std::unique_ptr<Graphic::VKTexture> m_textureL;

    /// @brief R 通道的 GPU 纹理
    std::unique_ptr<Graphic::VKTexture> m_textureR;

    /// @brief 纹理是否需要重新上传 GPU
    bool m_textureDirty{ false };

    // --- 色图查找表 ---
    /// @brief 256 级预计算颜色表 (RGBA u8 × 4)
    std::array<std::array<unsigned char, 4>, 256> m_colormapLUT;

    // --- 视图状态 ---
    float  m_zoom{ 1.0f };
    double m_lastViewStart{ -1e9 };
    double m_lastViewEnd{ -1e9 };

    /// @brief 视图窗口变化阈值 (秒)
    static constexpr double VIEW_CHANGE_THRESHOLD{ 0.002 };
};

}  // namespace MMM::UI
