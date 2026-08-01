#include "audio/SoundEffectPool.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/KeySoundControl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::Audio
{

SoundEffectSchedulePlan planSoundEffectSchedule(
    std::size_t targetTimelineFrame, std::size_t currentTimelineFrame,
    double previewSpeed, bool syncSpeed) noexcept
{
    if ( syncSpeed ) {
        return {
            .mode  = SoundEffectScheduleMode::AbsoluteTimelineFrame,
            .frame = targetTimelineFrame,
        };
    }

    const std::size_t timelineDelay =
        targetTimelineFrame > currentTimelineFrame
            ? targetTimelineFrame - currentTimelineFrame
            : 0U;
    const long double safeSpeed =
        std::isfinite(previewSpeed) && previewSpeed > 0.0
            ? static_cast<long double>(previewSpeed)
            : 1.0L;
    const long double outputDelay =
        static_cast<long double>(timelineDelay) / safeSpeed;
    constexpr long double MAX_DELAY =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    return {
        .mode  = SoundEffectScheduleMode::RelativeOutputDelay,
        .frame = outputDelay >= MAX_DELAY
                     ? std::numeric_limits<std::size_t>::max()
                     : static_cast<std::size_t>(std::round(outputDelay)),
    };
}

/// @brief 从 PreparedTimelineAudio 按独立播放位置输出 PCM 的实时音源。
///
/// 该节点只读取已在非实时线程完成 DSP 的不可变 PCM，使同一个 Effect
/// 作为自动采样和 Note HitEffect 时共享完全一致的资源级音频语义。
class PreparedSampleSourceNode final : public ice::IAudioNode
{
public:
    /// @brief 不持有上下文的时间线参考位置读取函数。
    using ReferencePositionReader =
        std::size_t (*)(const void* context) noexcept;

    /// @brief 不持有上下文的输入结束通知函数。
    using FinalInputListener = void (*)(void* context) noexcept;

    /// @brief 构造预处理 PCM 播放源。
    /// @param audio 生命周期覆盖节点的不可变 PCM。
    explicit PreparedSampleSourceNode(
        std::shared_ptr<const PreparedTimelineAudio> audio)
        : m_audio(std::move(audio))
        , m_totalFrames(m_audio ? m_audio->numFrames() : 0U)
    {
        auto provider = std::make_unique<ReferenceProviderState>();
        m_activeProvider.store(provider.get(), std::memory_order_seq_cst);
        m_activeProviderOwner = std::move(provider);
    }

    /// @brief 从当前位置读取一个音频 block。
    /// @param buffer 调用方预分配的内部格式缓冲。
    /// @warning
    /// 音频回调热路径：只执行原子访问、固定区间 PCM 复制和逐样本增益，
    /// 不执行分配、锁、文件访问或资源 DSP。
    void process(ice::AudioBuffer& buffer) override
    {
        buffer.clear();
        if ( !m_audio || !m_isPlaying.load(std::memory_order_acquire) ) return;
        if ( buffer.afmt != ice::ICEConfig::internal_format ) return;

        const std::size_t requestedFrames = buffer.num_frames();
        if ( requestedFrames == 0U ) return;

        std::size_t gainedThisBlock{ 0U };
        std::size_t silenceFrames{ 0U };
        bool        startedInsideBlock{ false };

        const std::size_t relativeDelay =
            m_scheduledStartDelayFrames.load(std::memory_order_relaxed);
        if ( relativeDelay > 0U ) {
            if ( relativeDelay >= requestedFrames ) {
                m_scheduledStartDelayFrames.store(
                    relativeDelay - requestedFrames, std::memory_order_relaxed);
                return;
            }
            silenceFrames = relativeDelay;
            m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
            startedInsideBlock = true;
        } else if ( const std::size_t scheduledStart =
                        m_scheduledStartFrame.load(std::memory_order_relaxed);
                    scheduledStart > 0U ) {
            const ReferenceProviderState* provider = acquireReferenceProvider();
            const bool        providerValid = provider && provider->reader;
            const std::size_t currentReference =
                providerValid ? provider->reader(provider->context) : 0U;
            releaseReferenceProvider();

            if ( !providerValid ) return;
            if ( currentReference < scheduledStart ) {
                const std::size_t framesToWait =
                    scheduledStart - currentReference;
                if ( framesToWait >= requestedFrames ) return;

                silenceFrames = framesToWait;
                m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
                startedInsideBlock = true;
            } else {
                m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
            }
        }

        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        if ( startedInsideBlock ) {
            const std::size_t framesToRead = requestedFrames - silenceFrames;
            gainedThisBlock =
                m_audio->read(buffer, playbackPosition, framesToRead);
            shiftDecodedFrames(buffer, silenceFrames, gainedThisBlock);
        } else if ( m_scheduledStartFrame.load(std::memory_order_relaxed) ==
                    0U ) {
            gainedThisBlock =
                m_audio->read(buffer, playbackPosition, requestedFrames);
            if ( gainedThisBlock < requestedFrames ) {
                buffer.clear_from(gainedThisBlock);
            }
        }
        m_playbackPosition.store(
            std::min(playbackPosition + gainedThisBlock, m_totalFrames),
            std::memory_order_relaxed);

        if ( m_playbackPosition.load(std::memory_order_relaxed) >=
             m_totalFrames ) {
            pause();
            notifyFinalInput();
        }

        const float gain = m_volume.load(std::memory_order_relaxed);
        if ( std::abs(gain - 1.0F) > std::numeric_limits<float>::epsilon() ) {
            applyVolume(buffer, gain);
        }
    }

    /// @brief 查询音源是否正在播放。
    [[nodiscard]] bool isplaying() const noexcept
    {
        return m_isPlaying.load(std::memory_order_acquire);
    }

    /// @brief 暂停音源。
    void pause() noexcept
    {
        m_isPlaying.store(false, std::memory_order_release);
    }

    /// @brief 开始或继续播放音源。
    void play() noexcept { m_isPlaying.store(true, std::memory_order_release); }

    /// @brief 设置音源线性音量。
    void setvolume(float value) noexcept
    {
        m_volume.store(value, std::memory_order_relaxed);
    }

    /// @brief 获取下一次读取的源帧。
    [[nodiscard]] std::size_t get_playpos() const noexcept
    {
        return m_playbackPosition.load(std::memory_order_relaxed);
    }

    /// @brief 设置下一次读取的源帧。
    void set_playpos(std::size_t framePosition) noexcept
    {
        m_playbackPosition.store(std::min(framePosition, m_totalFrames),
                                 std::memory_order_relaxed);
        m_finalInputNotified.store(false, std::memory_order_relaxed);
    }

    /// @brief 设置绝对参考时间线起播帧。
    void set_scheduled_start_frame(std::size_t frame) noexcept
    {
        m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
        m_scheduledStartFrame.store(frame, std::memory_order_relaxed);
    }

    /// @brief 设置相对输出起播延迟。
    void set_scheduled_start_delay_frames(std::size_t frames) noexcept
    {
        m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
        m_scheduledStartDelayFrames.store(frames, std::memory_order_relaxed);
    }

    /// @brief 发布绝对调度使用的参考时钟。
    /// @warning 低频控制路径：会分配并回收不可变 provider 状态。
    void set_reference_pos_provider(const void*             context,
                                    ReferencePositionReader reader)
    {
        auto provider     = std::make_unique<ReferenceProviderState>();
        provider->context = reader ? context : nullptr;
        provider->reader  = reader;
        publishReferenceProvider(std::move(provider));
    }

    /// @brief 清除绝对调度参考时钟。
    /// @warning 低频控制路径：会分配并回收不可变 provider 状态。
    void clear_reference_pos_provider()
    {
        publishReferenceProvider(std::make_unique<ReferenceProviderState>());
    }

    /// @brief 设置最后一块有效输入的通知。
    /// @warning 只能在该实例不处于播放状态时修改。
    void set_final_input_listener(void*              context,
                                  FinalInputListener listener) noexcept
    {
        m_finalInputListenerContext = listener ? context : nullptr;
        m_finalInputListener        = listener;
    }

    /// @brief 清除输入结束通知。
    /// @warning 只能在该实例不处于播放状态时修改。
    void clear_final_input_listener() noexcept
    {
        m_finalInputListenerContext = nullptr;
        m_finalInputListener        = nullptr;
    }

    /// @brief 获取预处理 PCM 总帧数。
    [[nodiscard]] std::size_t num_frames() const noexcept
    {
        return m_totalFrames;
    }

private:
    /// @brief 控制线程发布、音频线程只读的参考时钟状态。
    struct ReferenceProviderState {
        /// @brief 不拥有的参考时钟上下文。
        const void* context{ nullptr };
        /// @brief 无异常、无阻塞的参考位置读取函数。
        ReferencePositionReader reader{ nullptr };
    };

    /// @brief 发布新的不可变参考时钟状态。
    void publishReferenceProvider(
        std::unique_ptr<ReferenceProviderState> provider)
    {
        if ( !provider ) {
            provider = std::make_unique<ReferenceProviderState>();
        }

        std::lock_guard<std::mutex> lock(m_providerControlMutex);
        const auto*                 nextAddress = provider.get();
        if ( m_activeProviderOwner ) {
            m_retiredProviders.push_back(std::move(m_activeProviderOwner));
        }
        m_activeProviderOwner = std::move(provider);
        m_activeProvider.store(nextAddress, std::memory_order_seq_cst);
        reclaimRetiredProvidersLocked();
    }

    /// @brief 回收未被音频线程保护的旧参考时钟状态。
    /// @warning 调用方必须持有 m_providerControlMutex。
    void reclaimRetiredProvidersLocked()
    {
        const auto* protectedProvider =
            m_providerHazard.load(std::memory_order_seq_cst);
        std::erase_if(
            m_retiredProviders,
            [protectedProvider](
                const std::unique_ptr<ReferenceProviderState>& provider) {
                return provider.get() != protectedProvider;
            });
    }

    /// @brief 在音频线程取得稳定参考时钟状态。
    /// @warning 音频回调热路径：只执行 lock-free hazard 原子访问。
    [[nodiscard]] const ReferenceProviderState*
    acquireReferenceProvider() noexcept
    {
        const ReferenceProviderState* provider{ nullptr };
        do {
            provider = m_activeProvider.load(std::memory_order_seq_cst);
            m_providerHazard.store(provider, std::memory_order_seq_cst);
        } while ( provider !=
                  m_activeProvider.load(std::memory_order_seq_cst) );
        return provider;
    }

    /// @brief 结束音频线程参考时钟读取临界区。
    void releaseReferenceProvider() noexcept
    {
        m_providerHazard.store(nullptr, std::memory_order_seq_cst);
    }

    /// @brief 通知下游当前播放周期已经没有后续输入。
    void notifyFinalInput() noexcept
    {
        if ( m_finalInputNotified.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }
        if ( m_finalInputListener ) {
            m_finalInputListener(m_finalInputListenerContext);
        }
    }

    /// @brief 把 block 起点解码的 PCM 移到实际起播帧。
    static void shiftDecodedFrames(ice::AudioBuffer& buffer,
                                   std::size_t       silenceFrames,
                                   std::size_t       decodedFrames) noexcept
    {
        float** samples = buffer.raw_ptrs();
        if ( !samples || silenceFrames >= buffer.num_frames() ) return;

        const std::size_t safeDecodedFrames =
            std::min(decodedFrames, buffer.num_frames() - silenceFrames);
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            if ( safeDecodedFrames > 0U ) {
                std::memmove(samples[channel] + silenceFrames,
                             samples[channel],
                             safeDecodedFrames * sizeof(float));
            }
            std::memset(samples[channel], 0, silenceFrames * sizeof(float));
        }
    }

    /// @brief 对当前 block 应用固定线性音量。
    static void applyVolume(ice::AudioBuffer& buffer, float gain) noexcept
    {
        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                samples[channel][frame] *= gain;
            }
        }
    }

    /// @brief 保持预处理 PCM 在全部播放实例期间存活。
    std::shared_ptr<const PreparedTimelineAudio> m_audio;
    /// @brief 构造阶段固定的源总帧数。
    std::size_t m_totalFrames{ 0U };
    /// @brief 下一次读取的 PCM 帧位置。
    std::atomic<std::size_t> m_playbackPosition{ 0U };
    /// @brief 每个播放实例的线性音量。
    std::atomic<float> m_volume{ 1.0F };
    /// @brief 当前实例是否允许输出。
    std::atomic_bool m_isPlaying{ false };
    /// @brief 绝对参考时间线起播帧。
    std::atomic<std::size_t> m_scheduledStartFrame{ 0U };
    /// @brief 相对输出域剩余延迟帧。
    std::atomic<std::size_t> m_scheduledStartDelayFrames{ 0U };
    /// @brief 当前播放周期是否已通知最终输入。
    std::atomic_bool m_finalInputNotified{ false };
    /// @brief 输入结束通知的不拥有上下文。
    void* m_finalInputListenerContext{ nullptr };
    /// @brief 输入结束通知函数。
    FinalInputListener m_finalInputListener{ nullptr };
    /// @brief 当前参考时钟状态的控制线程所有权。
    std::unique_ptr<ReferenceProviderState> m_activeProviderOwner;
    /// @brief 等待越过 hazard 临界区的旧参考时钟状态。
    std::vector<std::unique_ptr<ReferenceProviderState>> m_retiredProviders;
    /// @brief 音频线程当前可见的参考时钟状态。
    std::atomic<const ReferenceProviderState*> m_activeProvider{ nullptr };
    /// @brief 音频线程正在读取的参考时钟状态。
    std::atomic<const ReferenceProviderState*> m_providerHazard{ nullptr };
    /// @brief 串行化控制线程 provider 发布与回收。
    std::mutex m_providerControlMutex;
};

/// @brief 为单个音效播放实例应用固定或线性变化的左右声道增益。
class StereoGainNode final : public ice::IAudioNode
{
public:
    /// @brief 单个实例的原子生命周期状态。
    enum class PlaybackState : std::uint8_t {
        Idle,
        Preparing,
        Playing,
        Stopping,
    };

    /// @brief 构造与播放实例生命周期绑定的双声道增益节点。
    /// @param input 已完成实例内变调处理的稳定输入节点。
    /// @param source 提供原始音效播放进度的稳定源节点。
    /// @param keySoundControls 生命周期覆盖节点的 Key 音控制库。
    StereoGainNode(std::shared_ptr<ice::TimeStretcher>       input,
                   std::shared_ptr<PreparedSampleSourceNode> source,
                   const KeySoundControlBank*                keySoundControls)
        : m_input(std::move(input))
        , m_source(std::move(source))
        , m_keySoundControls(keySoundControls)
    {
        if ( m_source ) {
            m_source->set_final_input_listener(this, &notifySourceFinalInput);
        }
    }

    /// @brief 清除源节点对当前实例的非拥有结束回调。
    ~StereoGainNode() override
    {
        if ( m_source ) {
            m_source->clear_final_input_listener();
        }
    }

    /// @brief 尝试由控制线程独占一个空闲实例。
    /// @return 从 Idle 成功切换到 Preparing 时返回 true。
    bool tryReserve()
    {
        auto expected = PlaybackState::Idle;
        return m_playbackState.compare_exchange_strong(
            expected,
            PlaybackState::Preparing,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }

    /// @brief 为下一次播放配置双声道包络和预计预定等待帧数。
    /// @param envelope 本次播放的左右声道增益包络。
    /// @param scheduledDelayFrames 当前参考位置到目标播放位置的预计帧数。
    /// @param playbackControl 玩家轨道与打击音类别的运行时控制。
    void prepare(const StereoGainEnvelope&      envelope,
                 std::size_t                    scheduledDelayFrames,
                 const KeySoundPlaybackControl& playbackControl)
    {
        m_startLeft.store(std::clamp(envelope.startLeft, 0.0F, 1.0F),
                          std::memory_order_relaxed);
        m_startRight.store(std::clamp(envelope.startRight, 0.0F, 1.0F),
                           std::memory_order_relaxed);
        m_endLeft.store(std::clamp(envelope.endLeft, 0.0F, 1.0F),
                        std::memory_order_relaxed);
        m_endRight.store(std::clamp(envelope.endRight, 0.0F, 1.0F),
                         std::memory_order_relaxed);
        m_remainingDelayFrames.store(scheduledDelayFrames,
                                     std::memory_order_relaxed);
        m_audioStarted.store(false, std::memory_order_relaxed);
        m_finalRequested.store(false, std::memory_order_relaxed);
        m_keySoundPlaybackControl = playbackControl;
        static_cast<void>(m_input->request_discontinuity());
        m_playbackState.store(PlaybackState::Playing,
                              std::memory_order_release);
    }

    /// @brief 停止当前包络并使节点返回静音。
    void deactivate()
    {
        m_playbackState.store(PlaybackState::Stopping,
                              std::memory_order_release);
        m_remainingDelayFrames.store(0U, std::memory_order_relaxed);
        m_audioStarted.store(false, std::memory_order_relaxed);
        m_finalRequested.store(false, std::memory_order_relaxed);
    }

    /// @brief 拉取单个音效实例并按原始采样进度应用左右声道增益。
    /// @param buffer 上游请求的音频缓冲。
    /// @warning SDL 音频回调热路径：每个活跃 HitEffect
    /// 每个缓冲周期执行，只允许原子读取与固定帧遍历；实例回收只切换
    /// lock-free 状态，不得释放对象。
    void process(ice::AudioBuffer& buffer) override
    {
        const auto playbackState =
            m_playbackState.load(std::memory_order_acquire);
        if ( playbackState != PlaybackState::Playing || !m_input ||
             !m_source ) {
            buffer.clear();
            if ( playbackState == PlaybackState::Stopping ) {
                auto expected = PlaybackState::Stopping;
                static_cast<void>(m_playbackState.compare_exchange_strong(
                    expected,
                    PlaybackState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed));
            }
            return;
        }

        const std::size_t playPositionBefore = m_source->get_playpos();
        m_input->process(buffer);
        const std::size_t playPositionAfter = m_source->get_playpos();
        const std::size_t gainedFrames =
            playPositionAfter >= playPositionBefore
                ? playPositionAfter - playPositionBefore
                : 0U;
        const std::size_t totalFrames = m_source->num_frames();

        const std::size_t bufferFrames = buffer.num_frames();
        const std::size_t delayBefore =
            m_remainingDelayFrames.load(std::memory_order_relaxed);
        m_remainingDelayFrames.store(
            delayBefore > bufferFrames ? delayBefore - bufferFrames : 0U,
            std::memory_order_relaxed);

        if ( gainedFrames > 0U ) {
            const bool audioStarted =
                m_audioStarted.load(std::memory_order_relaxed);
            std::size_t outputOffset = 0U;
            if ( !audioStarted && gainedFrames < bufferFrames ) {
                if ( playPositionAfter < totalFrames ) {
                    outputOffset = bufferFrames - gainedFrames;
                } else if ( delayBefore > 0U && delayBefore < bufferFrames ) {
                    outputOffset = delayBefore;
                }
            }
            applyEnvelope(buffer,
                          outputOffset,
                          std::min(gainedFrames, bufferFrames - outputOffset),
                          playPositionBefore);
            m_audioStarted.store(true, std::memory_order_relaxed);
        } else if ( m_finalRequested.load(std::memory_order_relaxed) ) {
            const std::size_t sourceFrame =
                totalFrames > bufferFrames ? totalFrames - bufferFrames : 0U;
            applyEnvelope(buffer, 0U, bufferFrames, sourceFrame);
        }

        if ( m_keySoundControls ) {
            applyRuntimeGain(buffer,
                             m_keySoundControls->effectivePlayerGain(
                                 m_keySoundPlaybackControl));
        }

        if ( totalFrames > 0U && playPositionAfter >= totalFrames &&
             !m_source->isplaying() &&
             !m_finalRequested.load(std::memory_order_acquire) ) {
            requestFinalInput();
        }
        if ( m_finalRequested.load(std::memory_order_acquire) &&
             m_input->is_final_input_drained() ) {
            auto expected = PlaybackState::Playing;
            static_cast<void>(m_playbackState.compare_exchange_strong(
                expected,
                PlaybackState::Idle,
                std::memory_order_acq_rel,
                std::memory_order_relaxed));
        }
    }

private:
    /// @brief 将 SourceNode 的同块结束通知转交给实例变调器。
    /// @param context 当前 StereoGainNode。
    /// @warning 音频回调热路径：只访问稳定指针和 lock-free 原子。
    static void notifySourceFinalInput(void* context) noexcept
    {
        auto* instance = static_cast<StereoGainNode*>(context);
        if ( instance ) {
            instance->requestFinalInput();
        }
    }

    /// @brief 为当前播放周期向实例变调器提交一次 final 输入。
    /// @warning 音频回调热路径：只写入 TimeStretcher 的 lock-free 邮箱。
    void requestFinalInput() noexcept
    {
        if ( !m_finalRequested.exchange(true, std::memory_order_acq_rel) &&
             m_input ) {
            static_cast<void>(m_input->request_final_input());
        }
    }

    /// @brief 对缓冲中对应原始音效采样的区域应用线性双声道增益。
    /// @param buffer 待修改的输出缓冲。
    /// @param outputOffset 有效音效在输出缓冲中的起始帧。
    /// @param frameCount 本次实际读取的音效帧数。
    /// @param sourceFrame 音效区域首帧对应的原始音效帧位置。
    void applyEnvelope(ice::AudioBuffer& buffer, std::size_t outputOffset,
                       std::size_t frameCount, std::size_t sourceFrame) const
    {
        if ( frameCount == 0U || buffer.num_channels() < 2U ) return;

        const float startLeft  = m_startLeft.load(std::memory_order_relaxed);
        const float startRight = m_startRight.load(std::memory_order_relaxed);
        const float endLeft    = m_endLeft.load(std::memory_order_relaxed);
        const float endRight   = m_endRight.load(std::memory_order_relaxed);
        if ( std::abs(startLeft - 1.0F) < 1e-6F &&
             std::abs(startRight - 1.0F) < 1e-6F &&
             std::abs(endLeft - 1.0F) < 1e-6F &&
             std::abs(endRight - 1.0F) < 1e-6F ) {
            return;
        }

        const std::size_t totalFrames = m_source->num_frames();
        const float       progressDivisor =
            totalFrames > 1U ? static_cast<float>(totalFrames - 1U) : 1.0F;
        const float firstProgress = std::clamp(
            static_cast<float>(sourceFrame) / progressDivisor, 0.0F, 1.0F);
        const float progressStep = 1.0F / progressDivisor;
        float leftGain  = startLeft + (endLeft - startLeft) * firstProgress;
        float rightGain = startRight + (endRight - startRight) * firstProgress;
        const float leftStep  = (endLeft - startLeft) * progressStep;
        const float rightStep = (endRight - startRight) * progressStep;

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::size_t frame = 0U; frame < frameCount; ++frame ) {
            samples[0][outputOffset + frame] *=
                std::clamp(leftGain, 0.0F, 1.0F);
            samples[1][outputOffset + frame] *=
                std::clamp(rightGain, 0.0F, 1.0F);
            leftGain += leftStep;
            rightGain += rightStep;
        }
    }

    /// @brief 在变调缓存之后对当前输出 block 应用运行时 Key 音增益。
    /// @warning 音频回调热路径：只执行预分配缓冲的有界逐样本乘法。
    static void applyRuntimeGain(ice::AudioBuffer& buffer, float gain) noexcept
    {
        if ( std::abs(gain - 1.0F) <= std::numeric_limits<float>::epsilon() ) {
            return;
        }
        if ( gain <= 0.0F ) {
            buffer.clear();
            return;
        }

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                samples[channel][frame] *= gain;
            }
        }
    }

    /// @brief 实例内变调器的稳定非空输入。
    std::shared_ptr<ice::TimeStretcher> m_input;
    /// @brief 提供原始采样进度并由所属播放实例稳定持有的源节点。
    std::shared_ptr<PreparedSampleSourceNode> m_source;
    /// @brief 每 block 读取的运行时 Key 音控制库观察指针。
    const KeySoundControlBank* m_keySoundControls{ nullptr };
    /// @brief 当前播放周期的玩家轨道与打击音类别。
    /// @warning 由 m_playbackState 的 release/acquire 顺序发布，播放期间
    /// 不可修改。
    KeySoundPlaybackControl m_keySoundPlaybackControl;
    /// @brief 本次包络的起始左声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_playbackState 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_startLeft{ 1.0F };
    /// @brief 本次包络的起始右声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_playbackState 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_startRight{ 1.0F };
    /// @brief 本次包络的结束左声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_playbackState 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_endLeft{ 1.0F };
    /// @brief 本次包络的结束右声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_playbackState 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_endRight{ 1.0F };
    /// @brief 预计仍需等待的预定播放帧数，用于定位首个非静音缓冲。
    /// @warning 逻辑线程初始化、音频线程递减；原子访问用于安全处理停止与复用。
    std::atomic<std::size_t> m_remainingDelayFrames{ 0U };
    /// @brief 当前实例是否已经产出过音效采样。
    /// @warning 逻辑线程重置、音频线程更新；原子访问用于安全处理停止与复用。
    std::atomic_bool m_audioStarted{ false };
    /// @brief 是否已通知实例变调器当前源已无后续输入。
    /// @warning 仅音频线程置位，控制线程在实例重新准备时复位。
    std::atomic_bool m_finalRequested{ false };
    /// @brief 当前实例的空闲、配置中或播放中状态。
    /// @warning
    /// 控制线程通过 CAS 独占空闲实例，音频线程只在播放结束时改回 Idle；
    /// 该状态机替代回调内互斥锁和容器写入。
    std::atomic<PlaybackState> m_playbackState{ PlaybackState::Idle };
};

static_assert(std::atomic<StereoGainNode::PlaybackState>::is_always_lock_free);

/// @brief 单个可复用的预处理音效播放实例。
struct SoundEffectPool::SFXPlayInstance {
    /// @brief 从共享预处理 PCM 读取的独立播放源。
    std::shared_ptr<PreparedSampleSourceNode> source;

    /// @brief 本次用户级临时变调使用的独立拉伸器。
    std::shared_ptr<ice::TimeStretcher> pitchStretcher;

    /// @brief 每个实例独立的双声道增益包络节点。
    std::shared_ptr<StereoGainNode> stereoGainNode;

    /// @brief 每个实例独立的声道控制总线。
    std::shared_ptr<ice::MixBus> channelMixer;

    /// @brief 本次播放相对资源基础音量的额外倍率。
    float volumeFactor{ 1.0F };
};

SoundEffectPool::SoundEffectPool(std::shared_ptr<ice::AudioTrack> track,
                                 const KeySoundControlBank* keySoundControls)
    : SoundEffectPool(PreparedTimelineAudio::fromTrack(std::move(track)),
                      keySoundControls)
{
}

SoundEffectPool::SoundEffectPool(
    std::shared_ptr<const PreparedTimelineAudio> audio,
    const KeySoundControlBank*                   keySoundControls)
    : m_audio(std::move(audio)), m_keySoundControls(keySoundControls)
{
    m_localMixer = std::make_shared<ice::MixBus>();
}

SoundEffectPool::~SoundEffectPool()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( auto& instance : m_allInstances ) {
        if ( m_localMixer ) {
            m_localMixer->remove_source(instance->channelMixer);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->remove_source(instance->stereoGainNode);
        }
    }
}

std::shared_ptr<ice::MixBus> SoundEffectPool::getMixer() const
{
    return m_localMixer;
}

std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::createInstance()
{
    if ( !m_audio || m_audio->numFrames() == 0U ) {
        return {};
    }

    auto instance    = std::make_shared<SFXPlayInstance>();
    instance->source = std::make_shared<PreparedSampleSourceNode>(m_audio);
    instance->pitchStretcher = std::make_shared<ice::TimeStretcher>();
    instance->channelMixer   = std::make_shared<ice::MixBus>();
    instance->stereoGainNode = std::make_shared<StereoGainNode>(
        instance->pitchStretcher, instance->source, m_keySoundControls);
    instance->pitchStretcher->set_inputnode(instance->source);
    instance->channelMixer->add_source(instance->stereoGainNode);

    if ( m_localMixer ) {
        m_localMixer->add_source(instance->channelMixer);
    }
    return instance;
}

std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::acquireInstance()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( const auto& instance : m_allInstances ) {
        if ( instance && instance->stereoGainNode &&
             instance->stereoGainNode->tryReserve() ) {
            return instance;
        }
    }

    auto instance = createInstance();
    if ( !instance || !instance->stereoGainNode ||
         !instance->stereoGainNode->tryReserve() ) {
        return {};
    }
    m_allInstances.push_back(instance);
    return instance;
}

void SoundEffectPool::init(int count)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( int i = 0; i < count; ++i ) {
        auto instance = createInstance();
        if ( instance ) {
            m_allInstances.push_back(std::move(instance));
        }
    }
}

void SoundEffectPool::play(float volumeFactor)
{
    play(volumeFactor, 0.0);
}

void SoundEffectPool::play(float volumeFactor, double pitchSemitones)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        float playbackVolume = 0.0F;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            instance->volumeFactor = std::isfinite(volumeFactor)
                                         ? std::max(0.0F, volumeFactor)
                                         : 0.0F;
            playbackVolume         = m_effectiveVolume * instance->volumeFactor;
        }
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(pitchSemitones);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_scheduled_start_frame(0);  // 确保没有残留的预定
        node->clear_reference_pos_provider();
        node->set_playpos(static_cast<size_t>(0));
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->prepare(
                StereoGainEnvelope{}, 0U, KeySoundPlaybackControl{});
        }
        node->play();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestInstance = instance;
        }
    }
}

void SoundEffectPool::playScheduled(
    float volumeFactor, std::size_t targetFrame, const void* referenceContext,
    ReferencePositionReader   referenceReader,
    const StereoGainEnvelope& stereoEnvelope, std::size_t scheduledDelayFrames,
    const KeySoundPlaybackControl& playbackControl)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        float playbackVolume = 0.0F;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            instance->volumeFactor = std::isfinite(volumeFactor)
                                         ? std::max(0.0F, volumeFactor)
                                         : 0.0F;
            playbackVolume         = m_effectiveVolume * instance->volumeFactor;
        }
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_playpos(static_cast<size_t>(0));
        node->set_scheduled_start_frame(targetFrame);
        node->set_reference_pos_provider(referenceContext, referenceReader);
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->prepare(
                stereoEnvelope, scheduledDelayFrames, playbackControl);
        }
        node->play();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestInstance = instance;
        }
    }
}

void SoundEffectPool::playScheduledRelative(
    float volumeFactor, std::size_t outputDelayFrames,
    const StereoGainEnvelope&      stereoEnvelope,
    const KeySoundPlaybackControl& playbackControl)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        float playbackVolume = 0.0F;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            instance->volumeFactor = std::isfinite(volumeFactor)
                                         ? std::max(0.0F, volumeFactor)
                                         : 0.0F;
            playbackVolume         = m_effectiveVolume * instance->volumeFactor;
        }
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_playpos(static_cast<size_t>(0));
        node->set_scheduled_start_delay_frames(outputDelayFrames);
        node->clear_reference_pos_provider();
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->prepare(
                stereoEnvelope, outputDelayFrames, playbackControl);
        }
        node->play();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestInstance = instance;
        }
    }
}

/// @brief 停止所有正在播放或预定的音效，并重置状态
void SoundEffectPool::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mtx);

    for ( auto& instance : m_allInstances ) {
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->deactivate();
        }
        instance->source->pause();
        instance->source->set_playpos(static_cast<size_t>(0));
        instance->source->set_scheduled_start_frame(0);
        instance->source->clear_reference_pos_provider();
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
    }
    m_latestInstance.reset();
}

void SoundEffectPool::setVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_volume = volume;
    // 注意：此处不直接更新 node 音量，因为我们不知道当前的全局音量和静音状态。
    // 实际音量更新应通过 updateEffectiveVolume 或在 play 时计算。
}

void SoundEffectPool::updateEffectiveVolume(float globalVolume, bool muted)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_effectiveVolume = muted ? 0.0F : m_volume * globalVolume;
    for ( auto& instance : m_allInstances ) {
        instance->source->setvolume(m_effectiveVolume * instance->volumeFactor);
    }
}

double SoundEffectPool::getDuration() const
{
    if ( !m_audio ) return 0.0;
    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(m_audio->numFrames()) / samplerate;
}

bool SoundEffectPool::usesPreparedAudio(
    const PreparedTimelineAudio* audio) const noexcept
{
    return m_audio.get() == audio;
}

double SoundEffectPool::getLatestPlaybackTime() const
{
    std::shared_ptr<SFXPlayInstance> latest;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        latest = m_latestInstance;
    }

    if ( !latest || !latest->source ) return 0.0;

    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(latest->source->get_playpos()) / samplerate;
}

bool SoundEffectPool::isPlaying() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestInstance || !m_latestInstance->source ) return false;
    return m_latestInstance->source->isplaying();
}

bool SoundEffectPool::isPaused() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestInstance || !m_latestInstance->source ) return false;
    return !m_latestInstance->source->isplaying() &&
           (m_latestInstance->source->get_playpos() > 0);
}

void SoundEffectPool::pause()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestInstance && m_latestInstance->source ) {
        m_latestInstance->source->pause();
    }
}

void SoundEffectPool::resume()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestInstance && m_latestInstance->source ) {
        m_latestInstance->source->play();
    }
}

}  // namespace MMM::Audio
