#include "BackgroundSpectrumAnalyzer.h"

#include "audio/AudioManager.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <numbers>
#include <utility>

namespace MMM::Audio
{
// 背景频谱由实时采集与 UI 线程分析两部分组成：
//
// - CaptureNode 先拉取原音频节点，再把最近样本写入固定大小原子环形缓冲；
// - 音频线程只做有界样本复制，不执行 FFT、分配、锁或绘制；
// - Analyzer 在 UI 更新路径复制最近窗口，允许读到相邻 block 的近似组合；
// - BGM 与 HitEffect 在时域相加后共同进入窗函数和 FFT；
// - FFT 表、位反转索引和旋转因子在构造时一次性预计算；
// - 频段按对数频率划分，更符合音乐低频需要更高分辨率的显示需求；
// - 每个频段取峰值并进行快攻慢放平滑，减少柱体闪烁；
// - 自适应参考峰值只改变视觉填充比例，不反馈到实际音频信号。
// - 左右声道始终共享频率边界与缩放参考，便于直接比较立体声能量；
// - 固定数组中 bandCount 之后的元素每次清零，减少频段时不会显示旧柱体。
// - 分析结果只服务视觉反馈，不承担计量级频谱或响度测量职责。

/// @brief 构造包装上游节点的实时频谱采集节点。
/// @param input 要透传并旁路采样的音频节点。
///
/// 环形缓冲显式初始化为静音，使首个完整 FFT 窗口到来前的左侧补零确定。
/// shared_ptr 只在构造时移动；process 不复制共享所有权。
/// 采集节点不改变上游输出，只额外保存左右前两个声道用于可视化。
BackgroundSpectrumCaptureNode::BackgroundSpectrumCaptureNode(
    std::shared_ptr<ice::IAudioNode> input)
    : m_input(std::move(input))
{
    // 原子 float 默认值不作为跨平台初始化契约，逐项显式写零。
    for ( auto& sample : m_left ) {
        sample.store(0.0f, std::memory_order_relaxed);
    }
    for ( auto& sample : m_right ) {
        sample.store(0.0f, std::memory_order_relaxed);
    }
}

/// @brief 透传上游音频并记录最近一个 FFT 窗口的立体声样本。
/// @param buffer 后端提供的预分配音频缓冲。
///
/// 超过 FFT_SIZE 的 block 只保留尾部，但 writtenFrames 仍按完整 block 前进，
/// 从而让环形索引与全局帧序列保持一致。单声道输入复制到右声道。
/// @warning 音频回调热路径；逐帧原子写入但不分配、不加锁、不等待。
void BackgroundSpectrumCaptureNode::process(ice::AudioBuffer& buffer)
{
    // 先清零确保空上游或上游短写时仍输出并采集确定静音。
    buffer.clear();
    if ( m_input ) {
        m_input->process(buffer);
    }

    const float* const* samples      = buffer.raw_ptrs();
    const std::size_t   frameCount   = buffer.num_frames();
    const std::size_t   channelCount = buffer.num_channels();
    const std::uint64_t writeStart =
        m_writtenFrames.load(std::memory_order_relaxed);
    if ( !samples || channelCount == 0U || frameCount == 0U ) {
        return;
    }

    // 大 block 只写其最新 FFT_SIZE 帧，旧部分不可能出现在任何后续分析窗口。
    const std::size_t firstFrame =
        frameCount > FFT_SIZE ? frameCount - FFT_SIZE : 0U;
    for ( std::size_t frame = firstFrame; frame < frameCount; ++frame ) {
        // 全局帧序号取模后写入固定槽，避免维护可竞争的单独写游标。
        const std::size_t ringIndex =
            static_cast<std::size_t>((writeStart + frame) % FFT_SIZE);
        m_left[ringIndex].store(samples[0][frame], std::memory_order_relaxed);
        const float rightSample =
            channelCount > 1U ? samples[1][frame] : samples[0][frame];
        m_right[ringIndex].store(rightSample, std::memory_order_relaxed);
    }
    // release 在样本写完后提交新窗口边界，读者先 acquire 该计数再复制。
    m_writtenFrames.store(writeStart + frameCount, std::memory_order_release);
}

/// @brief 把最近 FFT_SIZE 帧复制到调用方固定数组。
/// @param left 左声道目标窗口。
/// @param right 右声道目标窗口。
///
/// 尚未积累满窗口时在左侧补零，把最新样本右对齐。采集继续写入期间允许个别
/// 槽来自相邻 block；频谱仅用于视觉展示，这种无锁近似优于阻塞音频线程。
/// acquire 计数保证复制不会看到尚未提交的新窗口边界，但不锁住正在更新的槽。
/// @warning UI 更新路径；目标尺寸不符时不写入。
void BackgroundSpectrumCaptureNode::copyLatest(std::span<float> left,
                                               std::span<float> right) const
{
    if ( left.size() != FFT_SIZE || right.size() != FFT_SIZE ) return;

    // 先铺满静音，下面只需覆盖实际可用的右侧前缀。
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    const std::uint64_t written =
        m_writtenFrames.load(std::memory_order_acquire);
    // written 可能远大于环形容量，但任何时刻最多恢复最近一个窗口。
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(written, FFT_SIZE));
    const std::size_t   outputOffset = FFT_SIZE - available;
    const std::uint64_t firstFrame   = written - available;
    // firstFrame 保持时间顺序，即使物理环形存储在窗口中间发生回绕。
    for ( std::size_t index = 0U; index < available; ++index ) {
        const std::size_t ringIndex =
            static_cast<std::size_t>((firstFrame + index) % FFT_SIZE);
        left[outputOffset + index] =
            m_left[ringIndex].load(std::memory_order_relaxed);
        right[outputOffset + index] =
            m_right[ringIndex].load(std::memory_order_relaxed);
    }
}

/// @brief 预计算 Hann 窗、位反转排列和 radix-2 FFT 旋转因子。
///
/// FFT_SIZE 必须是二次幂，才能使用迭代 Cooley-Tukey 蝶形。输入会在 analyze
/// 中直接写入位反转位置，因此 executeFft 不再需要额外重排或临时分配。
BackgroundSpectrumAnalyzer::BackgroundSpectrumAnalyzer()
{
    constexpr std::size_t fftSize = BackgroundSpectrumCaptureNode::FFT_SIZE;
    static_assert((fftSize & (fftSize - 1U)) == 0U);

    // 二次幂长度的索引位数固定，反转这些低位即可得到 FFT 输入排列。
    constexpr std::size_t bitCount = std::bit_width(fftSize) - 1U;
    for ( std::size_t index = 0U; index < fftSize; ++index ) {
        // Hann 窗压低窗口两端，减少非整周期信号的频谱泄漏。
        m_window[index] =
            0.5 -
            0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                           static_cast<double>(fftSize - 1U));

        // 在构造期计算位反转，避免每帧频谱分析重复位操作。
        std::size_t reversed = 0U;
        std::size_t value    = index;
        for ( std::size_t bit = 0U; bit < bitCount; ++bit ) {
            reversed = (reversed << 1U) | (value & 1U);
            value >>= 1U;
        }
        m_bitReversedIndices[index] = reversed;
    }

    // 实值输入仍使用完整复数 FFT；半圈旋转因子足以覆盖所有蝶形阶段。
    for ( std::size_t index = 0U; index < fftSize / 2U; ++index ) {
        const double angle = -2.0 * std::numbers::pi *
                             static_cast<double>(index) /
                             static_cast<double>(fftSize);
        m_twiddleFactors[index] = { std::cos(angle), std::sin(angle) };
    }
}

/// @brief 就地执行预排好位反转顺序的 radix-2 复数 FFT。
/// @param buffer 固定 FFT_SIZE 的复数样本数组。
///
/// stageSize 从 2 倍增到完整窗口，每组把偶分量与乘旋转因子的奇分量组合。
/// twiddleStride 把当前阶段的局部 offset 映射到全局预计算旋转表。
/// @warning UI 分析路径；O(N log N)，不在音频回调执行。
void BackgroundSpectrumAnalyzer::executeFft(
    std::span<std::complex<double>, BackgroundSpectrumCaptureNode::FFT_SIZE>
        buffer) const
{
    constexpr std::size_t fftSize = BackgroundSpectrumCaptureNode::FFT_SIZE;
    // 每轮合并长度相同的相邻子变换，最终得到完整窗口频谱。
    for ( std::size_t stageSize = 2U; stageSize <= fftSize; stageSize <<= 1U ) {
        const std::size_t halfStage     = stageSize / 2U;
        const std::size_t twiddleStride = fftSize / stageSize;
        for ( std::size_t base = 0U; base < fftSize; base += stageSize ) {
            for ( std::size_t offset = 0U; offset < halfStage; ++offset ) {
                // 先保存两个输入，避免原地写前半结果覆盖后半计算所需值。
                const auto even = buffer[base + offset];
                const auto odd  = buffer[base + offset + halfStage] *
                                 m_twiddleFactors[offset * twiddleStride];
                buffer[base + offset]             = even + odd;
                buffer[base + offset + halfStage] = even - odd;
            }
        }
    }
}

/// @brief 合成立体声采集窗口并更新可直接绘制的频段电平。
/// @param bgmCapture 可选的背景音乐采集节点。
/// @param hitEffectCapture 可选的打击音效采集节点。
/// @param requestedBandCount 调用方期望显示的单声道频段数。
/// @return 内部复用的最新立体声归一化电平。
///
/// 频段数变化时清除平滑历史与自适应峰值，防止旧频段映射污染新布局。
/// 两路节点在时域合成，随后统一应用 Hann 窗和 FFT，近似最终听到的总信号。
/// 没有任一采集节点时仍运行静音窗口，使旧电平按相同 release 规则自然衰减。
/// 输出对象复用固定数组，调用方必须在当前 UI 帧内消费返回引用。
/// @warning 返回引用只在本分析器下次 analyze 前保持本帧内容。
const BackgroundSpectrumLevels& BackgroundSpectrumAnalyzer::analyze(
    const BackgroundSpectrumCaptureNode* bgmCapture,
    const BackgroundSpectrumCaptureNode* hitEffectCapture,
    std::size_t                          requestedBandCount)
{
    // 固定数组只允许配置范围内的有效前缀，外部异常值在入口处裁剪。
    const std::size_t bandCount = std::clamp(
        requestedBandCount,
        static_cast<std::size_t>(Config::BACKGROUND_SPECTRUM_MIN_BANDS),
        static_cast<std::size_t>(Config::BACKGROUND_SPECTRUM_MAX_BANDS));
    m_levels.bandCount = bandCount;
    if ( bandCount != m_previousBandCount ) {
        // 频段边界改变后旧平滑值不再代表相同频率范围，必须全部重置。
        m_smoothedLeft.fill(0.0f);
        m_smoothedRight.fill(0.0f);
        m_adaptivePeakReference = 0.0f;
        m_previousBandCount     = bandCount;
    }

    // 缺失 BGM 时仍以静音为基底，HitEffect 可以单独产生背景响应。
    m_captureLeft.fill(0.0f);
    m_captureRight.fill(0.0f);
    if ( bgmCapture ) {
        bgmCapture->copyLatest(m_captureLeft, m_captureRight);
    }
    if ( hitEffectCapture ) {
        // 时域相加保留 BGM 与音效的相位关系，比频谱幅值直接相加更接近总线输出。
        hitEffectCapture->copyLatest(m_hitCaptureLeft, m_hitCaptureRight);
        for ( std::size_t index = 0U;
              index < BackgroundSpectrumCaptureNode::FFT_SIZE;
              ++index ) {
            m_captureLeft[index] += m_hitCaptureLeft[index];
            m_captureRight[index] += m_hitCaptureRight[index];
        }
    }

    // 窗函数与位反转在同一次线性遍历完成，直接准备 executeFft 的输入布局。
    for ( std::size_t index = 0U;
          index < BackgroundSpectrumCaptureNode::FFT_SIZE;
          ++index ) {
        const std::size_t target = m_bitReversedIndices[index];
        m_fftLeft[target]        = {
            static_cast<double>(m_captureLeft[index]) * m_window[index], 0.0
        };
        m_fftRight[target] = {
            static_cast<double>(m_captureRight[index]) * m_window[index], 0.0
        };
    }
    executeFft(m_fftLeft);
    executeFft(m_fftRight);
    // 两声道独立平滑，但共享自适应归一化参考，保持立体声相对强弱。
    const float leftSignalPeak =
        updateChannel(m_fftLeft, m_smoothedLeft, m_levels.left, bandCount);
    const float rightSignalPeak =
        updateChannel(m_fftRight, m_smoothedRight, m_levels.right, bandCount);
    normalizeLevels(std::max(leftSignalPeak, rightSignalPeak), bandCount);
    return m_levels;
}

/// @brief 把单声道复数频谱映射为对数频段并更新平滑状态。
/// @param spectrum 已完成 FFT 的完整复数频谱。
/// @param smoothed 跨帧保留的频段平滑值。
/// @param output 本帧写给渲染器的固定容量数组。
/// @param bandCount 当前有效频段前缀长度。
/// @return 压缩但尚未自适应缩放的本帧目标峰值。
///
/// 本步骤完成频率分桶、幅值压缩与快攻慢放；最终左右统一缩放由
/// normalizeLevels 负责，以免两个声道各自拉满后失去立体声强弱关系。
/// 频率上边界限制到 16 kHz 是显示取舍，不影响音频链中的高频内容。
/// 每个频段至少包含一个正频率 bin，且固定数组的无效尾部在本次调用清零。
float BackgroundSpectrumAnalyzer::updateChannel(
    std::span<const std::complex<double>,
              BackgroundSpectrumCaptureNode::FFT_SIZE>
                                                              spectrum,
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& smoothed,
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& output,
    std::size_t                                               bandCount)
{
    // 频率映射依赖有效采样率；异常配置直接清零，避免除零或产生非有限值。
    constexpr std::size_t fftSize = BackgroundSpectrumCaptureNode::FFT_SIZE;
    const double          sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) {
        output.fill(0.0f);
        return 0.0f;
    }

    // 显示聚焦常用音乐范围，直流与极低频漂移不参与背景柱体。
    constexpr double minFrequency = 40.0;
    const double     maxFrequency = std::min(16000.0, sampleRate * 0.5);
    if ( maxFrequency <= minFrequency ) {
        output.fill(0.0f);
        return 0.0f;
    }
    // 对数坐标让低频获得更细的分段，高频仍覆盖到 Nyquist 或 16 kHz。
    const double     logMin        = std::log(minFrequency);
    const double     logRange      = std::log(maxFrequency) - logMin;
    const double     binFrequency  = sampleRate / static_cast<double>(fftSize);
    constexpr double responseScale = 80.0;
    const double     responseDenominator = std::log1p(responseScale);
    float            signalPeak          = 0.0f;

    // 每个频段独立求峰值，避免窄带强音被同带大量弱 bin 平均稀释。
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        const double lowerRatio =
            static_cast<double>(band) / static_cast<double>(bandCount);
        const double upperRatio =
            static_cast<double>(band + 1U) / static_cast<double>(bandCount);
        const double lowerFrequency = std::exp(logMin + logRange * lowerRatio);
        const double upperFrequency = std::exp(logMin + logRange * upperRatio);
        std::size_t  firstBin =
            static_cast<std::size_t>(std::floor(lowerFrequency / binFrequency));
        std::size_t finalBin =
            static_cast<std::size_t>(std::ceil(upperFrequency / binFrequency));
        // 排除直流 bin，并保证每段至少覆盖一个、且不越过正频率半谱。
        firstBin = std::clamp(firstBin, std::size_t{ 1U }, fftSize / 2U);
        finalBin = std::clamp(std::max(finalBin, firstBin + 1U),
                              std::size_t{ 1U },
                              fftSize / 2U + 1U);

        double peakMagnitude = 0.0;
        for ( std::size_t bin = firstBin; bin < finalBin; ++bin ) {
            peakMagnitude = std::max(peakMagnitude, std::abs(spectrum[bin]));
        }
        // 单边谱乘二补偿丢弃的负频率，随后按 FFT 长度归一化。
        const double normalizedMagnitude =
            peakMagnitude * 2.0 / static_cast<double>(fftSize);
        // log1p 压缩动态范围，平方根进一步提高弱信号的视觉可见度。
        const float target =
            std::clamp(static_cast<float>(std::sqrt(
                           std::log1p(normalizedMagnitude * responseScale) /
                           responseDenominator)),
                       0.0f,
                       1.0f);
        signalPeak = std::max(signalPeak, target);
        // 上升使用快攻，下降使用慢放，兼顾节拍响应与可读性。
        const float smoothing = target > smoothed[band] ? 0.58f : 0.14f;
        smoothed[band] += (target - smoothed[band]) * smoothing;
        output[band] = smoothed[band];
    }
    // 无效尾部必须清零，防止减少频段数后渲染侧误读旧数据。
    std::fill(output.begin() + bandCount, output.end(), 0.0f);
    return signalPeak;
}

/// @brief 用左右共享的自适应参考缩放当前有效频段。
/// @param signalPeak 两声道原始目标峰值的最大值。
/// @param bandCount 当前有效频段数量。
///
/// 响度改变后柱体仍应利用大部分高度，但静音不能把残余平滑值重新放大。参考值
/// 因此使用独立快攻慢放，并在有效信号降低时限制最多保留的余量。
/// targetFill 小于一会保留顶部空间，避免轻微峰值变化导致持续硬截顶。
/// 左右使用同一 scale，因此该步骤不改变同频段左右声道的比例。
void BackgroundSpectrumAnalyzer::normalizeLevels(float       signalPeak,
                                                 std::size_t bandCount)
{
    // signalFloor 抑制静音底噪，targetFill 为最强柱体预留少量顶部空间。
    constexpr float signalFloor            = 0.0125f;
    constexpr float targetFill             = 0.94f;
    constexpr float referenceAttack        = 0.65f;
    constexpr float referenceRelease       = 0.18f;
    constexpr float silentReferenceRelease = 0.94f;
    constexpr float maximumHeadroom        = 1.25f;

    // 左右声道共享最大显示峰值，归一化不会破坏它们的相对响度。
    float displayPeak = 0.0f;
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        displayPeak = std::max(
            displayPeak, std::max(m_levels.left[band], m_levels.right[band]));
    }

    if ( std::isfinite(signalPeak) && signalPeak > signalFloor &&
         displayPeak > 0.0f ) {
        // 有效信号出现时快速追随更高峰值，较慢跟随音量降低。
        if ( m_adaptivePeakReference <= signalFloor ) {
            // 首个有效窗口直接建立参考，避免从零缓慢爬升导致瞬时满屏。
            m_adaptivePeakReference = displayPeak;
        } else {
            // attack/release 只作用于视觉参考，不修改频段自身的平滑状态。
            const float response = displayPeak > m_adaptivePeakReference
                                       ? referenceAttack
                                       : referenceRelease;
            m_adaptivePeakReference +=
                (displayPeak - m_adaptivePeakReference) * response;
            // 音量降低时限制保留余量，避免电平柱长时间只占据底部。
            m_adaptivePeakReference = std::min(m_adaptivePeakReference,
                                               displayPeak * maximumHeadroom);
        }
        m_adaptivePeakReference =
            std::max(m_adaptivePeakReference, signalFloor);
    } else {
        // 静音时参考峰值比频段平滑值回落得更慢，使残余柱自然收缩而非放大底噪。
        m_adaptivePeakReference = std::max(
            signalFloor, m_adaptivePeakReference * silentReferenceRelease);
    }

    // 参考永不低于 floor，因此静音残留不会被无限放大。
    const float scale =
        targetFill / std::max(m_adaptivePeakReference, signalFloor);
    // 最终钳位保护绘制契约，即使未来响应函数稍有过冲也保持 [0, 1]。
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        m_levels.left[band] =
            std::clamp(m_levels.left[band] * scale, 0.0f, 1.0f);
        m_levels.right[band] =
            std::clamp(m_levels.right[band] * scale, 0.0f, 1.0f);
    }
}

/// @brief 由 AudioManager 汇总当前背景频谱绘制数据。
/// @param bandCount 调用方希望展示的频段数量。
/// @param includeHitEffects 是否把打击音效采集叠加到背景音乐。
/// @return 当前分析结果；音频图未初始化时返回空电平。
///
/// 该入口只传递已存在节点的稳定观察指针，不改变音频图或捕获节点生命周期。
/// includeHitEffects=false 时不读取音效环形缓冲，背景只反映 BGM 总线。
const BackgroundSpectrumLevels& AudioManager::updateBackgroundSpectrum(
    std::size_t bandCount, bool includeHitEffects)
{
    // 分析器尚未随音频图初始化时返回稳定静态空对象，调用者无需判空。
    static const BackgroundSpectrumLevels empty;
    if ( !m_backgroundSpectrumAnalyzer ) return empty;
    // HitEffect 是否叠加由视图需求决定，BGM 捕获始终作为主要背景输入。
    return m_backgroundSpectrumAnalyzer->analyze(
        m_bgmSpectrumCapture.get(),
        includeHitEffects ? m_hitEffectSpectrumCapture.get() : nullptr,
        bandCount);
}

}  // namespace MMM::Audio
