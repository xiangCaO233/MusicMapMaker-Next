#pragma once

#include "audio/BackgroundSpectrum.h"

#include <fftw3.h>
#include <ice/core/IAudioNode.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace MMM::Audio
{

/// @brief 在音频图中透传数据并保存最近一段双声道 PCM 的无锁采样节点。
class BackgroundSpectrumCaptureNode final : public ice::IAudioNode
{
public:
    /// @brief 背景频谱 FFT 的固定采样帧数。
    static constexpr std::size_t FFT_SIZE = 2048U;

    /// @brief 构造采样节点。
    /// @param input 实际被透传的音频输入节点。
    explicit BackgroundSpectrumCaptureNode(
        std::shared_ptr<ice::IAudioNode> input);

    /// @brief 透传输入并记录最近的左右声道样本。
    /// @param buffer 音频线程提供的输出缓冲。
    /// @warning 音频回调热路径：每个音频缓冲周期执行，只允许固定容量原子写入；
    /// 写入者为音频线程，读取者为逻辑线程，禁止加入分配、锁或阻塞操作。
    void process(ice::AudioBuffer& buffer) override;

    /// @brief 复制最近的固定长度双声道样本。
    /// @param left 左声道目标缓冲，长度必须为 FFT_SIZE。
    /// @param right 右声道目标缓冲，长度必须为 FFT_SIZE。
    /// @warning 逻辑更新热路径：每次频谱刷新调用；原子读取用于避免与音频线程
    /// 发生数据竞争，禁止加入共享所有权复制。
    void copyLatest(std::span<float> left, std::span<float> right) const;

private:
    /// @brief 被透传的稳定输入节点。
    std::shared_ptr<ice::IAudioNode> m_input;
    /// @brief 左声道无锁环形采样缓存。
    /// @warning 音频线程写、逻辑线程读；逐样本 relaxed 原子是跨线程安全所必需。
    std::array<std::atomic<float>, FFT_SIZE> m_left{};
    /// @brief 右声道无锁环形采样缓存。
    /// @warning 音频线程写、逻辑线程读；逐样本 relaxed 原子是跨线程安全所必需。
    std::array<std::atomic<float>, FFT_SIZE> m_right{};
    /// @brief 音频线程累计写入帧数。
    /// @warning 音频线程 release 写、逻辑线程 acquire
    /// 读，用于发布完整采样窗口。
    std::atomic<std::uint64_t> m_writtenFrames{ 0U };
};

/// @brief 将实时 PCM 转换为可直接绘制的对数频段与平滑电平。
class BackgroundSpectrumAnalyzer final
{
public:
    /// @brief 分配固定 FFT 缓冲并创建双声道计划。
    BackgroundSpectrumAnalyzer();
    /// @brief 销毁 FFT 计划和缓冲。
    ~BackgroundSpectrumAnalyzer();

    BackgroundSpectrumAnalyzer(const BackgroundSpectrumAnalyzer&) = delete;
    BackgroundSpectrumAnalyzer& operator=(const BackgroundSpectrumAnalyzer&) =
        delete;

    /// @brief 分析 BGM，并按配置选择是否混入 HitEffect 采样。
    /// @param bgmCapture 当前 BGM 采样节点；为空时按静音处理。
    /// @param hitEffectCapture 当前 HitEffect 采样节点；为空时不混入。
    /// @param requestedBandCount 请求的单声道频段数。
    /// @return 内部稳定保存的平滑立体声频段。
    /// @warning 逻辑更新热路径：每次主画布快照最多调用一次；执行固定 2048 点
    /// FFT，不得加入文件访问、动态规划创建或共享所有权复制。
    [[nodiscard]] const BackgroundSpectrumLevels& analyze(
        const BackgroundSpectrumCaptureNode* bgmCapture,
        const BackgroundSpectrumCaptureNode* hitEffectCapture,
        std::size_t                          requestedBandCount);

private:
    /// @brief 根据 FFT 输出刷新一个声道的对数频段。
    /// @param spectrum 当前声道的 FFT 复数输出。
    /// @param smoothed 当前声道跨帧平滑缓存。
    /// @param output 当前声道的归一化绘制结果。
    /// @param bandCount 当前有效频段数。
    /// @return 本帧平滑前的最高频段电平，用于判断是否仍有有效信号。
    [[nodiscard]] float updateChannel(
        const fftw_complex*                                       spectrum,
        std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& smoothed,
        std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& output,
        std::size_t                                               bandCount);

    /// @brief 按当前立体声最高电平动态调整绘制范围。
    /// @param signalPeak 本帧平滑前的最高频段电平。
    /// @param bandCount 当前有效频段数。
    /// @warning 逻辑更新热路径：每次电平图刷新调用，只允许固定频段遍历。
    void normalizeLevels(float signalPeak, std::size_t bandCount);

    /// @brief 当前 BGM 与可选 HitEffect 合并后的左声道采样。
    std::array<float, BackgroundSpectrumCaptureNode::FFT_SIZE> m_captureLeft{};
    /// @brief 当前 BGM 与可选 HitEffect 合并后的右声道采样。
    std::array<float, BackgroundSpectrumCaptureNode::FFT_SIZE> m_captureRight{};
    /// @brief 本次分析使用的 HitEffect 左声道采样。
    std::array<float, BackgroundSpectrumCaptureNode::FFT_SIZE>
        m_hitCaptureLeft{};
    /// @brief 本次分析使用的 HitEffect 右声道采样。
    std::array<float, BackgroundSpectrumCaptureNode::FFT_SIZE>
        m_hitCaptureRight{};
    /// @brief 固定 FFT 窗口的 Hann 系数。
    std::array<double, BackgroundSpectrumCaptureNode::FFT_SIZE> m_window{};
    /// @brief 左声道跨帧平滑电平。
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS> m_smoothedLeft{};
    /// @brief 右声道跨帧平滑电平。
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS> m_smoothedRight{};
    /// @brief FFTW 管理的左声道时域输入。
    double* m_fftInputLeft{ nullptr };
    /// @brief FFTW 管理的右声道时域输入。
    double* m_fftInputRight{ nullptr };
    /// @brief FFTW 管理的左声道频域输出。
    fftw_complex* m_fftOutputLeft{ nullptr };
    /// @brief FFTW 管理的右声道频域输出。
    fftw_complex* m_fftOutputRight{ nullptr };
    /// @brief 左声道固定 FFT 执行计划。
    fftw_plan m_fftPlanLeft{ nullptr };
    /// @brief 右声道固定 FFT 执行计划。
    fftw_plan m_fftPlanRight{ nullptr };
    /// @brief 上次分析使用的频段数，用于检测平滑缓存重置时机。
    std::size_t m_previousBandCount{ 0U };
    /// @brief 动态高度归一化使用的跨帧峰值参考。
    float m_adaptivePeakReference{ 0.0f };
    /// @brief 返回给渲染层的稳定频段存储。
    BackgroundSpectrumLevels m_levels;
};

}  // namespace MMM::Audio
