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

// SoundEffectPool 把一份不可变 PreparedTimelineAudio 扩展成可并发复用的播放
// 实例集合。每个实例拥有独立源游标、变调器、包络节点和声道总线，只有 PCM
// 和只读 KeySound 控制库跨实例共享。
//
// 单个实例的信号链如下：
//
// PreparedSampleSourceNode -> TimeStretcher -> StereoGainNode -> channelMixer
//
// SourceNode 负责绝对或相对起播、PCM 游标和资源基础音量；TimeStretcher 负责
// 本次即时音效的临时变调；StereoGainNode 负责双声道包络、运行时 KeySound
// 增益和实例生命周期；channelMixer 保留声道模式入口并汇入池级 m_localMixer。
//
// 控制线程可在旧 voice 尚未收到停止确认时立即触发新播放，因此实例状态采用
// Idle、Preparing、Playing、Stopping 四态。只有 Idle 能被 CAS 预留；Stopping
// 必须由音频回调处理一个 block 后转回 Idle，防止旧停止命令覆盖新配置。
//
// 回调热路径不修改实例容器、不持有 mutex、不分配对象。池扩容、provider 发布
// 和 mixer 图连接只发生在控制路径。回调通过原子状态与固定 shared_ptr 图边访问
// 已经完整构造的实例。
//
// 绝对时间线调度使用外部参考帧；绕过主变速器的音效使用相对输出延迟。两种
// 模式互斥存储，设置其中一种会清除另一种，避免复用 voice 时残留旧起播条件。
//
// PreparedSampleSourceNode 的 provider 使用轻量 hazard 指针保护。控制线程发布
// 新的不可变 provider 后暂存旧对象，只有它不再等于音频线程 hazard 时才回收。
// 这避免回调中使用 shared_ptr 原子复制或互斥锁。

/// @brief 根据主时间线倍率选择同步音效的调度帧域。
/// @param targetTimelineFrame 音效应触发的目标谱面帧。
/// @param currentTimelineFrame 提交操作时的当前谱面帧。
/// @param previewSpeed 当前主时间线预览倍率。
/// @param syncSpeed true 时音效随主变速器同步。
/// @return 绝对时间线帧或相对输出延迟计划。
SoundEffectSchedulePlan planSoundEffectSchedule(
    std::size_t targetTimelineFrame, std::size_t currentTimelineFrame,
    double previewSpeed, bool syncSpeed) noexcept
{
    if ( syncSpeed ) {
        // 同步音效进入主拉伸器之前，直接沿用谱面绝对帧。
        return {
            .mode  = SoundEffectScheduleMode::AbsoluteTimelineFrame,
            .frame = targetTimelineFrame,
        };
    }

    const std::size_t timelineDelay =
        targetTimelineFrame > currentTimelineFrame
            ? targetTimelineFrame - currentTimelineFrame
            : 0U;
    // 已经过目标点的非同步音效立即触发，不产生无符号下溢。
    const long double safeSpeed =
        std::isfinite(previewSpeed) && previewSpeed > 0.0
            ? static_cast<long double>(previewSpeed)
            : 1.0L;
    // 非有限或非正倍率没有有效墙钟换算语义，使用中性倍率兜底。
    const long double outputDelay =
        static_cast<long double>(timelineDelay) / safeSpeed;
    constexpr long double MAX_DELAY =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    // 先在 long double 域计算和钳制，避免超范围直接转为 size_t。
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
///
/// 节点支持相对输出延迟和绝对参考时间线两种起播方式。读完最后一帧时只暂停
/// 自身并通知下游 final；实例是否可复用仍由 StereoGainNode 等待拉伸器排空。
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
        // 初始空 provider 使 active 指针始终指向受控对象，而不是临时 nullptr。
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
        // 所有提前返回都基于已清零缓冲，保证输出静音而非残留上次内容。
        buffer.clear();
        if ( !m_audio || !m_isPlaying.load(std::memory_order_acquire) ) return;
        if ( buffer.afmt != ice::ICEConfig::internal_format ) return;

        const std::size_t requestedFrames = buffer.num_frames();
        if ( requestedFrames == 0U ) return;

        std::size_t gainedThisBlock{ 0U };
        std::size_t silenceFrames{ 0U };
        bool        startedInsideBlock{ false };

        // 相对输出延迟优先，它不依赖可能已经失效的外部参考时钟。
        const std::size_t relativeDelay =
            m_scheduledStartDelayFrames.load(std::memory_order_relaxed);
        if ( relativeDelay > 0U ) {
            if ( relativeDelay >= requestedFrames ) {
                // 整个 block 尚未到起点，只递减剩余延迟并保持静音。
                m_scheduledStartDelayFrames.store(
                    relativeDelay - requestedFrames, std::memory_order_relaxed);
                return;
            }
            silenceFrames = relativeDelay;
            // 起点落在本 block 内，记录前置静音长度并开始读取 PCM。
            m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
            startedInsideBlock = true;
        } else if ( const std::size_t scheduledStart =
                        m_scheduledStartFrame.load(std::memory_order_relaxed);
                    scheduledStart > 0U ) {
            // hazard 临界区只覆盖 reader 调用，随后立即解除保护。
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

                // 绝对起点落入当前 block，前段静音、后段从 PCM 零帧开始。
                silenceFrames = framesToWait;
                m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
                startedInsideBlock = true;
            } else {
                // 当前参考已经到达或越过目标，本 block 立即开始。
                m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
            }
        }

        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        if ( startedInsideBlock ) {
            // PCM 从 buffer 起点写入，再整体右移到真实输出偏移。
            const std::size_t framesToRead = requestedFrames - silenceFrames;
            gainedThisBlock =
                m_audio->read(buffer, playbackPosition, framesToRead);
            shiftDecodedFrames(buffer, silenceFrames, gainedThisBlock);
        } else if ( m_scheduledStartFrame.load(std::memory_order_relaxed) ==
                    0U ) {
            // 无等待状态时直接填满当前 block，尾部不足部分显式清零。
            gainedThisBlock =
                m_audio->read(buffer, playbackPosition, requestedFrames);
            if ( gainedThisBlock < requestedFrames ) {
                buffer.clear_from(gainedThisBlock);
            }
        }
        m_playbackPosition.store(
            std::min(playbackPosition + gainedThisBlock, m_totalFrames),
            std::memory_order_relaxed);
        // 游标按实际读取帧推进并钳制总长度，静音等待不消耗源 PCM。

        if ( m_playbackPosition.load(std::memory_order_relaxed) >=
             m_totalFrames ) {
            // final 每个播放周期只通知一次，set_playpos 为下次播放复位。
            pause();
            notifyFinalInput();
        }

        const float gain = m_volume.load(std::memory_order_relaxed);
        // 单位增益直接跳过逐样本乘法，常见默认路径保持最小开销。
        if ( std::abs(gain - 1.0F) > std::numeric_limits<float>::epsilon() ) {
            applyVolume(buffer, gain);
        }
    }

    /// @brief 查询音源是否正在播放。
    /// @return 控制线程最近发布的播放许可。
    [[nodiscard]] bool isplaying() const noexcept
    {
        return m_isPlaying.load(std::memory_order_acquire);
    }

    /// @brief 暂停音源。
    /// @warning 音频回调可在源结束时调用，只执行 release 原子写。
    void pause() noexcept
    {
        m_isPlaying.store(false, std::memory_order_release);
    }

    /// @brief 开始或继续播放音源。
    /// @warning 控制路径在其余播放参数配置完成后最后调用。
    void play() noexcept { m_isPlaying.store(true, std::memory_order_release); }

    /// @brief 设置音源线性音量。
    /// @param value 已组合池增益和本次播放倍率的线性值。
    void setvolume(float value) noexcept
    {
        m_volume.store(value, std::memory_order_relaxed);
    }

    /// @brief 获取下一次读取的源帧。
    /// @return 已消费的 PreparedTimelineAudio 帧位置。
    [[nodiscard]] std::size_t get_playpos() const noexcept
    {
        return m_playbackPosition.load(std::memory_order_relaxed);
    }

    /// @brief 设置下一次读取的源帧。
    /// @param framePosition 目标源帧，超过尾端时钳制。
    void set_playpos(std::size_t framePosition) noexcept
    {
        // 新游标代表新的播放周期，允许下一次尾端重新发送 final。
        m_playbackPosition.store(std::min(framePosition, m_totalFrames),
                                 std::memory_order_relaxed);
        m_finalInputNotified.store(false, std::memory_order_relaxed);
    }

    /// @brief 设置绝对参考时间线起播帧。
    /// @param frame 目标参考帧；零表示无需等待。
    void set_scheduled_start_frame(std::size_t frame) noexcept
    {
        // 两种调度模式互斥，先清相对延迟再发布绝对帧。
        m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
        m_scheduledStartFrame.store(frame, std::memory_order_relaxed);
    }

    /// @brief 设置相对输出起播延迟。
    /// @param frames 从下一输出 block 起计算的等待帧数。
    void set_scheduled_start_delay_frames(std::size_t frames) noexcept
    {
        // 相对模式不读取外部时钟，清除可能残留的绝对目标。
        m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
        m_scheduledStartDelayFrames.store(frames, std::memory_order_relaxed);
    }

    /// @brief 发布绝对调度使用的参考时钟。
    /// @param context reader 使用的不拥有上下文。
    /// @param reader 音频回调可安全调用的无异常读取函数。
    /// @warning 低频控制路径：会分配并回收不可变 provider 状态。
    void set_reference_pos_provider(const void*             context,
                                    ReferencePositionReader reader)
    {
        // reader 为空时 context 也清空，禁止保留无消费者的悬空地址。
        auto provider     = std::make_unique<ReferenceProviderState>();
        provider->context = reader ? context : nullptr;
        provider->reader  = reader;
        publishReferenceProvider(std::move(provider));
    }

    /// @brief 清除绝对调度参考时钟。
    /// @warning 低频控制路径：会分配并回收不可变 provider 状态。
    void clear_reference_pos_provider()
    {
        // 发布空状态而非原子写 nullptr，沿用同一 hazard 生命周期协议。
        publishReferenceProvider(std::make_unique<ReferenceProviderState>());
    }

    /// @brief 设置最后一块有效输入的通知。
    /// @param context listener 使用的不拥有上下文。
    /// @param listener 同一音频 block 内调用的无异常通知函数。
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
        // 析构前先断开回调，避免源节点保留外层 StereoGainNode 地址。
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
    /// @param provider 新状态所有权；为空时替换为无 reader 状态。
    /// @warning 控制路径持锁并可能分配退休容器，禁止从回调调用。
    void publishReferenceProvider(
        std::unique_ptr<ReferenceProviderState> provider)
    {
        if ( !provider ) {
            provider = std::make_unique<ReferenceProviderState>();
        }

        std::lock_guard<std::mutex> lock(m_providerControlMutex);
        // 先保存新对象地址，再移动所有权，地址在 unique_ptr 移动后保持稳定。
        const auto* nextAddress = provider.get();
        if ( m_activeProviderOwner ) {
            // 旧对象先退休，不能在音频线程可能已经读取地址时立即析构。
            m_retiredProviders.push_back(std::move(m_activeProviderOwner));
        }
        m_activeProviderOwner = std::move(provider);
        m_activeProvider.store(nextAddress, std::memory_order_seq_cst);
        // active 发布完成后，只回收当前 hazard 没有保护的旧对象。
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
                // hazard 最多保护一个 provider，其余退休项均已不可达。
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
            // 先读取 active，再发布 hazard，最后复查 active 未在窗口内变化。
            provider = m_activeProvider.load(std::memory_order_seq_cst);
            m_providerHazard.store(provider, std::memory_order_seq_cst);
        } while ( provider !=
                  m_activeProvider.load(std::memory_order_seq_cst) );
        // 循环退出保证返回地址仍由 active owner 或退休列表持有。
        return provider;
    }

    /// @brief 结束音频线程参考时钟读取临界区。
    void releaseReferenceProvider() noexcept
    {
        // reader 调用结束后清空 hazard，允许下一次控制发布回收旧对象。
        m_providerHazard.store(nullptr, std::memory_order_seq_cst);
    }

    /// @brief 通知下游当前播放周期已经没有后续输入。
    void notifyFinalInput() noexcept
    {
        // exchange 保证源尾端即使被重复观察，也只向拉伸器通知一次。
        if ( m_finalInputNotified.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }
        if ( m_finalInputListener ) {
            // listener 和 context 在实例非播放状态下成对设置或清除。
            m_finalInputListener(m_finalInputListenerContext);
        }
    }

    /// @brief 把 block 起点解码的 PCM 移到实际起播帧。
    /// @param buffer PCM 当前从零偏移开始的输出缓冲。
    /// @param silenceFrames block 内起播前应保留的静音帧数。
    /// @param decodedFrames 实际写入 buffer 起点的有效帧数。
    /// @warning 音频回调热路径，只执行按声道 memmove 和 memset。
    static void shiftDecodedFrames(ice::AudioBuffer& buffer,
                                   std::size_t       silenceFrames,
                                   std::size_t       decodedFrames) noexcept
    {
        float** samples = buffer.raw_ptrs();
        if ( !samples || silenceFrames >= buffer.num_frames() ) return;

        const std::size_t safeDecodedFrames =
            std::min(decodedFrames, buffer.num_frames() - silenceFrames);
        // memmove 允许源和目标区间重叠，必须先右移再清零前缀。
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
    /// @param buffer 待原地缩放的内部格式缓冲。
    /// @param gain 已规范化的实例线性增益。
    /// @warning 音频回调热路径，复杂度与声道数乘 block 帧数成正比。
    static void applyVolume(ice::AudioBuffer& buffer, float gain) noexcept
    {
        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        // 所有声道应用相同资源基础增益，立体声包络由下游节点处理。
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
///
/// 节点位于实例 TimeStretcher 之后，因此既能等待拉伸器 final 排空，也能把
/// KeySound 实时控制应用到变调后的完整输出。包络进度仍以原始源帧计算，保证
/// 临时变调不会改变音效左右声道运动相对于资源内容的位置。
class StereoGainNode final : public ice::IAudioNode
{
public:
    /// @brief 单个实例的原子生命周期状态。
    ///
    /// Preparing 由控制线程独占配置窗口，Playing 发布完整参数，Stopping 等待
    /// 回调确认清空，Idle 才允许下一次预留。
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
            // 源结束通知直接请求本实例 stretcher final，保持在同一个音频
            // block。
            m_source->set_final_input_listener(this, &notifySourceFinalInput);
        }
    }

    /// @brief 清除源节点对当前实例的非拥有结束回调。
    ~StereoGainNode() override
    {
        if ( m_source ) {
            // source 由实例共同持有，析构前必须去除指向 this 的观察回调。
            m_source->clear_final_input_listener();
        }
    }

    /// @brief 尝试由控制线程独占一个空闲实例。
    /// @return 从 Idle 成功切换到 Preparing 时返回 true。
    bool tryReserve()
    {
        auto expected = PlaybackState::Idle;
        // 成功 acquire 使后续配置看到上次回调清理；失败只需 relaxed 观察值。
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
        // 包络端点独立钳制，防止异常映射放大或翻转任一声道。
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
        // 每个播放周期重置首块定位和 final drain 状态。
        m_audioStarted.store(false, std::memory_order_relaxed);
        m_finalRequested.store(false, std::memory_order_relaxed);
        m_keySoundPlaybackControl = playbackControl;
        // 复用 stretcher 前丢弃上一播放周期可能残留的内部历史。
        static_cast<void>(m_input->request_discontinuity());
        m_playbackState.store(PlaybackState::Playing,
                              std::memory_order_release);
        // release 是配置提交点，回调 acquire 后才能读取上述非原子控制值。
    }

    /// @brief 停止当前包络并使节点返回静音。
    ///
    /// 控制线程只发布 Stopping 并清周期状态，音频回调下一次 process 才转 Idle。
    void deactivate()
    {
        // release 保证回调先观察停止，不再拉取本实例输入。
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
        // 非 Playing 状态始终输出静音；Stopping 在这个安全点确认回收。
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
                // CAS 防止意外状态变化时把新周期覆盖成 Idle。
            }
            return;
        }

        const std::size_t playPositionBefore = m_source->get_playpos();
        // 用拉取前后源游标差得到真实 PCM 帧数，不把 stretcher 尾部算入源进度。
        m_input->process(buffer);
        const std::size_t playPositionAfter = m_source->get_playpos();
        const std::size_t gainedFrames =
            playPositionAfter >= playPositionBefore
                ? playPositionAfter - playPositionBefore
                : 0U;
        // 防御游标重置，避免无符号减法下溢扩大成巨量有效帧。
        const std::size_t totalFrames = m_source->num_frames();

        const std::size_t bufferFrames = buffer.num_frames();
        const std::size_t delayBefore =
            m_remainingDelayFrames.load(std::memory_order_relaxed);
        // 预计延迟只辅助首块包络定位，实际起播仍由源节点调度决定。
        m_remainingDelayFrames.store(
            delayBefore > bufferFrames ? delayBefore - bufferFrames : 0U,
            std::memory_order_relaxed);

        if ( gainedFrames > 0U ) {
            const bool audioStarted =
                m_audioStarted.load(std::memory_order_relaxed);
            std::size_t outputOffset = 0U;
            if ( !audioStarted && gainedFrames < bufferFrames ) {
                // 首个有效 block 可能含前置静音，需要定位 PCM 在输出中的偏移。
                if ( playPositionAfter < totalFrames ) {
                    // 非尾块时，有效帧位于 block 末端。
                    outputOffset = bufferFrames - gainedFrames;
                } else if ( delayBefore > 0U && delayBefore < bufferFrames ) {
                    // 极短资源首块即尾块时，用提交阶段记录的延迟消除歧义。
                    outputOffset = delayBefore;
                }
            }
            applyEnvelope(buffer,
                          outputOffset,
                          std::min(gainedFrames, bufferFrames - outputOffset),
                          playPositionBefore);
            // 包络仅覆盖本次真实源 PCM 区域，不缩放前置静音。
            m_audioStarted.store(true, std::memory_order_relaxed);
        } else if ( m_finalRequested.load(std::memory_order_relaxed) ) {
            // 源已结束后 stretcher 仍可能输出尾部，沿用资源末段包络。
            const std::size_t sourceFrame =
                totalFrames > bufferFrames ? totalFrames - bufferFrames : 0U;
            applyEnvelope(buffer, 0U, bufferFrames, sourceFrame);
        }

        if ( m_keySoundControls ) {
            // 每个 block 读取一次完整快照，使已排定 voice 响应最新静音和增益。
            applyRuntimeGain(buffer,
                             m_keySoundControls->effectivePlayerGain(
                                 m_keySoundPlaybackControl));
        }

        if ( totalFrames > 0U && playPositionAfter >= totalFrames &&
             !m_source->isplaying() &&
             !m_finalRequested.load(std::memory_order_acquire) ) {
            // 源通知是主路径，此检查覆盖边界实现未回调时的防御情况。
            requestFinalInput();
        }
        if ( m_finalRequested.load(std::memory_order_acquire) &&
             m_input->is_final_input_drained() ) {
            // 只有 final 尾部完全排空后才归还实例，避免下一播放截断尾音。
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
        // context 生命周期由 SFXPlayInstance 的稳定图所有权覆盖。
        auto* instance = static_cast<StereoGainNode*>(context);
        if ( instance ) {
            instance->requestFinalInput();
        }
    }

    /// @brief 为当前播放周期向实例变调器提交一次 final 输入。
    /// @warning 音频回调热路径：只写入 TimeStretcher 的 lock-free 邮箱。
    void requestFinalInput() noexcept
    {
        // exchange 把来自源回调和防御检查的重复通知合并为一次请求。
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
        // 单声道或空区间没有左右包络语义，保持原样返回。
        if ( frameCount == 0U || buffer.num_channels() < 2U ) return;

        const float startLeft  = m_startLeft.load(std::memory_order_relaxed);
        const float startRight = m_startRight.load(std::memory_order_relaxed);
        const float endLeft    = m_endLeft.load(std::memory_order_relaxed);
        const float endRight   = m_endRight.load(std::memory_order_relaxed);
        if ( std::abs(startLeft - 1.0F) < 1e-6F &&
             std::abs(startRight - 1.0F) < 1e-6F &&
             std::abs(endLeft - 1.0F) < 1e-6F &&
             std::abs(endRight - 1.0F) < 1e-6F ) {
            // 全单位包络是常见路径，跳过后续进度计算和逐帧钳制。
            return;
        }

        const std::size_t totalFrames = m_source->num_frames();
        // 单帧资源使用一作为除数，使起点进度稳定为零。
        const float progressDivisor =
            totalFrames > 1U ? static_cast<float>(totalFrames - 1U) : 1.0F;
        const float firstProgress = std::clamp(
            static_cast<float>(sourceFrame) / progressDivisor, 0.0F, 1.0F);
        const float progressStep = 1.0F / progressDivisor;
        // 从本 block 对应源帧的精确包络值开始，跨 block 保持连续。
        float leftGain  = startLeft + (endLeft - startLeft) * firstProgress;
        float rightGain = startRight + (endRight - startRight) * firstProgress;
        const float leftStep  = (endLeft - startLeft) * progressStep;
        const float rightStep = (endRight - startRight) * progressStep;

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        // 只修改前两个声道，其他声道保留上游内容和既有路由策略。
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
        // 单位增益跳过遍历，静音直接 clear，普通增益才逐样本处理。
        if ( std::abs(gain - 1.0F) <= std::numeric_limits<float>::epsilon() ) {
            return;
        }
        if ( gain <= 0.0F ) {
            buffer.clear();
            return;
        }

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        // KeySound 控制属于整个实例，统一作用于缓冲中的所有声道。
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

/// @brief 从完整解码音轨构造可复用音效池。
/// @param track 作为不可变 PCM 来源的音轨。
/// @param keySoundControls 可为空的运行时 KeySound 控制库。
///
/// 委托构造会把 AudioTrack 包装为 PreparedTimelineAudio，使后续实例创建只面对
/// 一种 PCM 接口。
SoundEffectPool::SoundEffectPool(std::shared_ptr<ice::AudioTrack> track,
                                 const KeySoundControlBank* keySoundControls)
    : SoundEffectPool(PreparedTimelineAudio::fromTrack(std::move(track)),
                      keySoundControls)
{
}

/// @brief 从已准备资源 PCM 构造可复用音效池。
/// @param audio 可与自动采样时间线共享的不可变 PCM。
/// @param keySoundControls 生命周期覆盖本池的控制库观察指针。
SoundEffectPool::SoundEffectPool(
    std::shared_ptr<const PreparedTimelineAudio> audio,
    const KeySoundControlBank*                   keySoundControls)
    : m_audio(std::move(audio)), m_keySoundControls(keySoundControls)
{
    // 本地总线在池生命周期内地址稳定，AudioManager 可一次接入上层图。
    m_localMixer = std::make_shared<ice::MixBus>();
}

/// @brief 断开池内所有实例的混音图边。
/// @warning 低频析构路径，持有池控制互斥量并修改 MixBus 来源。
SoundEffectPool::~SoundEffectPool()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( auto& instance : m_allInstances ) {
        // 先从池总线移除实例总线，再断开实例内部包络节点。
        if ( m_localMixer ) {
            m_localMixer->remove_source(instance->channelMixer);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->remove_source(instance->stereoGainNode);
        }
    }
}

/// @brief 获取供上层音频图连接的池级混音总线。
/// @return 池生命周期内稳定的 MixBus 共享所有权。
std::shared_ptr<ice::MixBus> SoundEffectPool::getMixer() const
{
    return m_localMixer;
}

/// @brief 创建并连接一个尚未预留的播放实例。
/// @return 完整图已接入池总线的新实例；资源为空时返回空指针。
/// @warning 低频控制路径，会分配节点并修改 MixBus 来源。
std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::createInstance()
{
    // 空 PCM 不创建无法自然完成的永久占位 voice。
    if ( !m_audio || m_audio->numFrames() == 0U ) {
        return {};
    }

    auto instance = std::make_shared<SFXPlayInstance>();
    // 按信号链顺序创建节点，所有 shared_ptr 在加入图前已经稳定。
    instance->source = std::make_shared<PreparedSampleSourceNode>(m_audio);
    instance->pitchStretcher = std::make_shared<ice::TimeStretcher>();
    instance->channelMixer   = std::make_shared<ice::MixBus>();
    instance->stereoGainNode = std::make_shared<StereoGainNode>(
        instance->pitchStretcher, instance->source, m_keySoundControls);
    instance->pitchStretcher->set_inputnode(instance->source);
    instance->channelMixer->add_source(instance->stereoGainNode);

    if ( m_localMixer ) {
        // 只把末端 channelMixer 暴露给池总线，内部节点不重复接入。
        m_localMixer->add_source(instance->channelMixer);
    }
    return instance;
}

/// @brief 预留一个空闲实例，不足时在控制路径扩容。
/// @return 状态已进入 Preparing、可安全配置的实例。
/// @warning 低频播放提交路径，会持锁遍历实例并可能分配新图节点。
std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::acquireInstance()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    // 按稳定创建顺序复用，第一个成功 CAS 的 Idle 实例由本次调用独占。
    for ( const auto& instance : m_allInstances ) {
        if ( instance && instance->stereoGainNode &&
             instance->stereoGainNode->tryReserve() ) {
            return instance;
        }
    }

    auto instance = createInstance();
    // 所有旧实例忙或正在停止时扩容，新实例仍需通过相同状态机预留。
    if ( !instance || !instance->stereoGainNode ||
         !instance->stereoGainNode->tryReserve() ) {
        return {};
    }
    m_allInstances.push_back(instance);
    // 容器只在控制锁下增长，音频回调从 MixBus 不可变来源快照访问节点。
    return instance;
}

/// @brief 预热指定数量的空闲播放实例。
/// @param count 希望提前创建的实例数；非正值不执行操作。
/// @warning 初始化低频路径，会分配音频节点并修改 MixBus 来源。
void SoundEffectPool::init(int count)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( int i = 0; i < count; ++i ) {
        // createInstance 失败时跳过，不把空指针放入实例容器。
        auto instance = createInstance();
        if ( instance ) {
            m_allInstances.push_back(std::move(instance));
        }
    }
}

/// @brief 以原始音高立即播放一次音效。
/// @param volumeFactor 相对于池基础音量的本次倍率。
void SoundEffectPool::play(float volumeFactor)
{
    play(volumeFactor, 0.0);
}

/// @brief 以指定临时音高立即播放一次音效。
/// @param volumeFactor 相对于池基础音量的本次倍率。
/// @param pitchSemitones 本次实例的半音偏移。
/// @warning 播放提交路径，可能因无 Idle voice 而扩容并修改音频图。
void SoundEffectPool::play(float volumeFactor, double pitchSemitones)
{
    // acquireInstance 返回 Preparing 实例，此后先完整配置再发布 play。
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        float playbackVolume = 0.0F;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            // 保存每实例倍率，使全局音量变化时能重新组合有效音量。
            instance->volumeFactor = std::isfinite(volumeFactor)
                                         ? std::max(0.0F, volumeFactor)
                                         : 0.0F;
            playbackVolume         = m_effectiveVolume * instance->volumeFactor;
        }
        if ( instance->pitchStretcher ) {
            // 即时播放允许调用方临时变调，资源级 DSP 已在 PCM 中处理。
            instance->pitchStretcher->set_pitch_semitones(pitchSemitones);
        }
        if ( instance->channelMixer ) {
            // 复用实例可能曾用于其他路由，每次提交恢复明确立体声模式。
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        // 清除两种调度和参考 provider，确保复用实例从当前 block 立即开始。
        node->set_scheduled_start_frame(0);
        node->clear_reference_pos_provider();
        node->set_playpos(static_cast<size_t>(0));
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            // 默认单位包络且不绑定 KeySound 轨道控制。
            instance->stereoGainNode->prepare(
                StereoGainEnvelope{}, 0U, KeySoundPlaybackControl{});
        }
        node->play();
        // play 是 release 发布点，必须位于所有实例参数配置之后。
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            // latest 只服务控制查询，不参与实例所有权或调度选择。
            m_latestInstance = instance;
        }
    }
}

/// @brief 按外部绝对参考帧排定一次同步音效。
/// @param volumeFactor 相对于池基础音量的本次倍率。
/// @param targetFrame 外部参考时间线中的目标帧。
/// @param referenceContext referenceReader 使用的不拥有上下文。
/// @param referenceReader 音频回调读取当前参考帧的函数。
/// @param stereoEnvelope 本次左右声道包络。
/// @param scheduledDelayFrames 提交时估算的等待帧，仅用于首块包络定位。
/// @param playbackControl 本次 voice 的 KeySound 轨道与类别控制。
/// @warning 播放提交路径，可能扩容并发布新的参考 provider。
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
            // 同步音效跟随主时间线变速，不在实例内再次改变音高。
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_playpos(static_cast<size_t>(0));
        // 绝对目标和 provider 成对设置，source 在每个 block 比较参考帧。
        node->set_scheduled_start_frame(targetFrame);
        node->set_reference_pos_provider(referenceContext, referenceReader);
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            // estimated delay 不决定起播，只解决首尾同块时的包络输出偏移。
            instance->stereoGainNode->prepare(
                stereoEnvelope, scheduledDelayFrames, playbackControl);
        }
        node->play();
        // 最后发布 source 播放许可，回调才能看到已配置 provider 与包络。
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestInstance = instance;
        }
    }
}

/// @brief 按输出域相对延迟排定一次非同步音效。
/// @param volumeFactor 相对于池基础音量的本次倍率。
/// @param outputDelayFrames 从下一输出 block 起计算的等待帧数。
/// @param stereoEnvelope 本次左右声道包络。
/// @param playbackControl 本次 voice 的 KeySound 轨道与类别控制。
/// @warning 播放提交路径，可能因无 Idle voice 而扩容。
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
            // 相对计划已把主预览倍率换算进延迟，实例保持零半音。
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_playpos(static_cast<size_t>(0));
        // 相对延迟不需要外部时钟，显式清除复用实例的 provider。
        node->set_scheduled_start_delay_frames(outputDelayFrames);
        node->clear_reference_pos_provider();
        node->setvolume(playbackVolume);
        if ( instance->stereoGainNode ) {
            // 同一延迟同时交给 source 调度和包络首块定位。
            instance->stereoGainNode->prepare(
                stereoEnvelope, outputDelayFrames, playbackControl);
        }
        node->play();
        // 所有配置完成后再发布 Playing，避免回调观察半配置实例。
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestInstance = instance;
        }
    }
}

/// @brief 停止所有正在播放或预定的音效，并重置状态。
///
/// 每个实例先进入 Stopping，再停止源、清调度、复位变调和声道模式。实例不会在
/// 控制线程直接标记 Idle；必须等音频回调观察 Stopping 后才能安全复用。
/// @warning 低频完整停止路径，持锁遍历池内全部实例并发布 provider。
void SoundEffectPool::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mtx);

    for ( auto& instance : m_allInstances ) {
        if ( instance->stereoGainNode ) {
            // 先阻止下游继续拉取，随后再重置上游源状态。
            instance->stereoGainNode->deactivate();
        }
        instance->source->pause();
        instance->source->set_playpos(static_cast<size_t>(0));
        instance->source->set_scheduled_start_frame(0);
        // 清除绝对目标后再发布空 provider，复用时不会读取旧上下文。
        instance->source->clear_reference_pos_provider();
        if ( instance->pitchStretcher ) {
            // 临时变调属于单次播放状态，停止后恢复中性半音。
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
    }
    m_latestInstance.reset();
    // 清除查询视图，不影响 m_allInstances 保留预热 voice 供后续复用。
}

/// @brief 设置池的资源基础音量。
/// @param volume 资源配置的线性音量。
///
/// 本函数只保存基础值；全局音量和静音必须由 updateEffectiveVolume 一次组合，
/// 避免在两个入口分别更新实例而产生短暂不一致。
void SoundEffectPool::setVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_volume = volume;
    // 当前实例音量留待掌握完整全局状态的 updateEffectiveVolume 更新。
}

/// @brief 更新池级有效音量并同步全部已有实例。
/// @param globalVolume 已组合上层音量分组的线性倍率。
/// @param muted true 时强制整个池静音。
/// @warning 低频音量设置路径，持锁遍历全部实例；禁止每帧重复调用。
void SoundEffectPool::updateEffectiveVolume(float globalVolume, bool muted)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_effectiveVolume = muted ? 0.0F : m_volume * globalVolume;
    // 每实例保留自己的触发倍率，池级变化只重新组合公共部分。
    for ( auto& instance : m_allInstances ) {
        instance->source->setvolume(m_effectiveVolume * instance->volumeFactor);
    }
}

/// @brief 获取预处理 PCM 的资源时长。
/// @return 统一采样率下的秒数；资源或采样率无效时返回零。
double SoundEffectPool::getDuration() const
{
    if ( !m_audio ) return 0.0;
    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(m_audio->numFrames()) / samplerate;
}

/// @brief 判断本池是否直接共享指定预处理 PCM 对象。
/// @param audio 待比较的非拥有地址。
/// @return 对象身份完全相同时返回 true。
bool SoundEffectPool::usesPreparedAudio(
    const PreparedTimelineAudio* audio) const noexcept
{
    return m_audio.get() == audio;
}

/// @brief 获取最近一次提交实例的源播放进度。
/// @return 播放进度秒数；没有最近实例或采样率无效时返回零。
///
/// 只在锁内复制 shared_ptr，帧位置读取和换算在锁外完成，缩短控制锁持有时间。
double SoundEffectPool::getLatestPlaybackTime() const
{
    std::shared_ptr<SFXPlayInstance> latest;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        // 局部强引用保证解锁后实例在完成换算前仍存活。
        latest = m_latestInstance;
    }

    if ( !latest || !latest->source ) return 0.0;

    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(latest->source->get_playpos()) / samplerate;
}

/// @brief 查询最近一次提交实例的源节点是否仍在读取 PCM。
/// @return 最近实例存在且源处于播放许可时返回 true。
bool SoundEffectPool::isPlaying() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestInstance || !m_latestInstance->source ) return false;
    return m_latestInstance->source->isplaying();
}

/// @brief 查询最近实例是否停在非零源位置。
/// @return 最近实例未播放且游标大于零时返回 true。
///
/// 自然结束也可能满足该条件；此接口保留既有“非零位置且未播放”兼容语义。
bool SoundEffectPool::isPaused() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestInstance || !m_latestInstance->source ) return false;
    return !m_latestInstance->source->isplaying() &&
           (m_latestInstance->source->get_playpos() > 0);
}

/// @brief 暂停最近一次提交的实例。
///
/// 只暂停源节点，不改变池中其他并发 voice，也不释放 StereoGainNode 状态。
void SoundEffectPool::pause()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestInstance && m_latestInstance->source ) {
        m_latestInstance->source->pause();
    }
}

/// @brief 恢复最近一次提交的实例。
///
/// 只恢复源读取许可；已完成并排空的实例应通过新的 play 调用重新配置。
void SoundEffectPool::resume()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestInstance && m_latestInstance->source ) {
        m_latestInstance->source->play();
    }
}

}  // namespace MMM::Audio
