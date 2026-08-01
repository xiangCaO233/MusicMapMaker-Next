#pragma once

#include "audio/AudioTimelineClock.h"
#include "audio/AudioTimelineTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ice
{
class AudioTrack;
}  // namespace ice

namespace MMM::Audio
{

class KeySoundControlBank;

/// @brief 下一段时间线输入结束处的语义。
enum class AudioTimelineInputBoundaryKind : std::uint8_t {
    None,
    Discontinuity,
    Final,
};

/// @brief 音频线程在拉取时间线前取得的连续输入区间。
struct AudioTimelineInputBoundary {
    /// @brief 当前 transport 可连续处理且不跨边界的帧数。
    std::size_t frameCount{ 0U };
    /// @brief frameCount 末端的循环跳转或自然结束语义。
    AudioTimelineInputBoundaryKind kind{ AudioTimelineInputBoundaryKind::None };
};

/// @brief 已在非实时线程固定下来的只读时间线 PCM 数据。
///
/// 原始音轨会在工厂函数中完成解码等待并取得稳定只读视图；经过资源级 DSP
/// 的音轨则由本对象持有处理后的 PCM。音频回调只执行边界检查和内存复制。
class PreparedTimelineAudio final
{
public:
    /// @brief 从已完成缓存的 IonCachy 音轨创建只读 PCM 视图。
    /// @param track 保持底层缓存存活的音轨。
    /// @return 音轨为空或没有可读 PCM 时返回空指针。
    /// @warning 低频资源路径：可能等待后台解码完成，禁止在音频回调中调用。
    [[nodiscard]] static std::shared_ptr<const PreparedTimelineAudio> fromTrack(
        std::shared_ptr<ice::AudioTrack> track);

    /// @brief 从离线 DSP 结果创建自有 PCM。
    /// @param channels 按声道分离且帧数一致的 PCM。
    /// @param sourceOwner 可选的原始音轨所有者，用于跨时间线与 HitEffect
    ///        缓存复用时保持同一解码资源存活。
    /// @return 输入无有效声道或帧时返回空指针。
    /// @warning 低频资源路径：会转移并整理 PCM 内存。
    [[nodiscard]] static std::shared_ptr<const PreparedTimelineAudio>
    fromOwnedChannels(std::vector<std::vector<float>>  channels,
                      std::shared_ptr<ice::AudioTrack> sourceOwner = {});

    PreparedTimelineAudio(const PreparedTimelineAudio&)            = delete;
    PreparedTimelineAudio& operator=(const PreparedTimelineAudio&) = delete;
    PreparedTimelineAudio(PreparedTimelineAudio&&)                 = delete;
    PreparedTimelineAudio& operator=(PreparedTimelineAudio&&)      = delete;

    /// @brief 构造引用外部缓存的 PCM；供工厂函数建立稳定视图。
    PreparedTimelineAudio(std::shared_ptr<ice::AudioTrack>    track,
                          std::vector<std::span<const float>> channelViews);

    /// @brief 构造自有 PCM；供离线资源处理器转移结果。
    /// @param channels 按声道分离的 PCM。
    /// @param sourceOwner 可选的原始音轨所有者。
    PreparedTimelineAudio(std::vector<std::vector<float>>  channels,
                          std::shared_ptr<ice::AudioTrack> sourceOwner);

    /// @brief 获取 PCM 帧数。
    [[nodiscard]] std::size_t numFrames() const noexcept;

    /// @brief 获取 PCM 声道数。
    [[nodiscard]] std::size_t numChannels() const noexcept;

    /// @brief 获取指定声道的只读 PCM。
    /// @param channel 声道索引。
    /// @return 越界时返回空 span。
    [[nodiscard]] std::span<const float> channel(
        std::size_t channel) const noexcept;

    /// @brief 将指定帧区间复制到调用方预分配的缓冲区。
    /// @param buffer 目标缓冲区。
    /// @param startFrame 源起始帧。
    /// @param frameCount 最多复制帧数。
    /// @return 实际复制帧数。
    /// @warning 音频回调热路径；不执行分配、锁、解码或文件访问。
    std::size_t read(ice::AudioBuffer& buffer, std::size_t startFrame,
                     std::size_t frameCount) const noexcept;

private:
    /// @brief 保持原始 AudioTrack 解码缓存生命周期。
    ///
    /// @warning
    /// 共享所有权仅在低频构造和销毁时复制；音频回调不会复制 shared_ptr。
    std::shared_ptr<ice::AudioTrack> m_sourceOwner;
    /// @brief 离线 DSP 结果的自有 PCM。
    std::vector<std::vector<float>> m_ownedChannels;
    /// @brief 指向原始缓存或自有 PCM 的稳定声道视图。
    std::vector<std::span<const float>> m_channelViews;
    /// @brief 所有有效声道共同拥有的帧数。
    std::size_t m_frameCount{ 0U };
};

/// @brief 已在非实时线程完成解析和缓存的时间线音频片段。
struct PreparedTimelineClip {
    /// @brief 谱面采样物件的稳定标识。
    AudioTimelineEventId eventId{ 0U };
    /// @brief 用于诊断和资源映射的稳定资源键。
    std::string sourceKey;
    /// @brief 片段在统一音频时间线上的起始帧，允许为负数。
    AudioTimelineFrame startFrame{ 0 };
    /// @brief 相对玩家轨道区的零基 BGM 轨道索引。
    std::uint32_t bgmTrackIndex{ 0U };
    /// @brief 片段自身的线性音量。
    float volume{ 1.0F };
    /// @brief 已完整缓存并应用资源级 DSP 的只读音频数据。
    ///
    /// @warning
    /// 共享所有权仅用于保证音频回调期间缓存不被资源线程释放；该指针只在构造
    /// 和销毁节点时复制，process 热路径不会复制 shared_ptr。
    std::shared_ptr<const PreparedTimelineAudio> audio;
};

/// @brief 将多个自动采样按统一时间线混合的实时音频节点。
///
/// 片段表在构造期间完成过滤和排序，之后保持不可变。播放控制由单个逻辑线程
/// 写入序列锁邮箱，并在音频 block 边界应用；回调内不执行分配、排序、锁、
/// 文件访问或资源解码。
class AudioTimelineMixerNode final : public ice::IAudioNode
{
public:
    /// @brief 不持有上下文的时间线自然结束通知函数。
    using FinalInputListener = void (*)(void* context) noexcept;

    /// @brief 构造多采样时间线节点。
    /// @param clips 已在非实时线程完整缓存的音频片段。
    /// @param requestedTimelineEndFrame
    /// 谱面物件决定的排除结束帧；最终结束帧还会
    ///        自动包含所有片段的实际结束位置。
    /// @param maximumProcessFrames 回调内单次缓存读取的最大帧数。
    /// @param keySoundControls 生命周期覆盖本节点的运行时 Key 音控制库。
    AudioTimelineMixerNode(
        std::vector<PreparedTimelineClip> clips,
        AudioTimelineFrame                requestedTimelineEndFrame,
        std::size_t                       maximumProcessFrames,
        const KeySoundControlBank*        keySoundControls = nullptr);
    /// @brief 回收控制线程延迟释放的调度状态。
    ~AudioTimelineMixerNode() override;

    AudioTimelineMixerNode(const AudioTimelineMixerNode&)            = delete;
    AudioTimelineMixerNode& operator=(const AudioTimelineMixerNode&) = delete;
    AudioTimelineMixerNode(AudioTimelineMixerNode&&)                 = delete;
    AudioTimelineMixerNode& operator=(AudioTimelineMixerNode&&)      = delete;

    /// @brief 在非实时线程准备并发布一份新的不可变调度状态。
    /// @param clips 已完整缓存的新片段。
    /// @param requestedTimelineEndFrame 谱面物件决定的排除结束帧。
    /// @param maximumProcessFrames 回调内单次缓存读取的最大帧数。
    /// @return 本次发布的单调调度代次。
    /// @warning
    /// 低频控制路径：允许分配和排序；音频线程只在下一个 block
    /// 起点交换指针，旧状态由控制线程延迟回收。
    std::uint64_t replaceSchedule(std::vector<PreparedTimelineClip> clips,
                                  AudioTimelineFrame requestedTimelineEndFrame,
                                  std::size_t        maximumProcessFrames);

    /// @brief 在非实时控制线程释放全部已由音频线程退役的调度状态。
    /// @return 本次完成释放的调度状态数量。
    ///
    /// 音频线程只负责把旧状态压入无锁退役栈；调用方应在时间线替换后的低频
    /// 轮询路径重复调用，直至音频 block 已接管新状态并由本函数完成资源释放。
    ///
    /// @warning
    /// 低频控制路径：可能析构完整 PCM 缓存和共享资源；允许与音频回调并发，
    /// 但只允许单个非实时控制线程调用，禁止放入音频回调或渲染热路径。
    [[nodiscard]] std::size_t reclaimRetiredSchedules() noexcept;

    /// @brief 设置在自然结束 block 内同步触发的下游 final 通知。
    /// @param context 生命周期覆盖音频后端的非拥有上下文。
    /// @param listener 无异常、无阻塞、无分配的通知函数。
    /// @warning 只能在音频后端启动前或停止后修改。
    void setFinalInputListener(void*              context,
                               FinalInputListener listener) noexcept;

    /// @brief 请求从当前位置播放。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void play() noexcept;

    /// @brief 请求冻结整条时间线。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void pause() noexcept;

    /// @brief 请求停止并回到时间线零点。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void stop() noexcept;

    /// @brief 请求跳转到指定有符号时间线帧。
    /// @param frame 目标时间线帧。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void seek(AudioTimelineFrame frame) noexcept;

    /// @brief 请求启用半开区间循环。
    /// @param range 有效范围必须满足 startFrame 小于 endFrame。
    /// @return 请求有效时返回 true。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    [[nodiscard]] bool setLoop(AudioTimelineLoopRange range) noexcept;

    /// @brief 请求关闭循环。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void clearLoop() noexcept;

    /// @brief 获取最近一个已完成音频 block 的时间线位置。
    /// @return 下一 block 起始的有符号时间线帧。
    [[nodiscard]] AudioTimelineFrame positionFrame() const noexcept;

    /// @brief 获取位置、时间戳、状态与纪元来自同一发布点的时间线时钟快照。
    /// @return 读取竞争过于激烈时返回 valid=false，调用方应继续推演上一锚点。
    /// @warning
    /// 逻辑热路径：只执行有界序列锁读取；禁止改成阻塞等待或无界自旋。
    [[nodiscard]] AudioTimelineClockSnapshot clockSnapshot() const noexcept;

    /// @brief 获取当前音频 block 开始时的时间线位置。
    /// @return 主时间线节点开始生成最近 block 时的位置。
    ///
    /// @warning
    /// 音频线程写入、同一混音 block 内后续音效源读取；依赖 MixBus
    /// 保持来源插入顺序，供预定 HitEffect 计算 block 内起播偏移。
    [[nodiscard]] AudioTimelineFrame blockStartFrame() const noexcept;

    /// @brief 在拉取上游前取得不跨循环或自然结束点的连续输入范围。
    /// @param maximumFrames 本次最多需要的时间线输入帧数。
    /// @return 连续帧数及其末端语义。
    ///
    /// @warning
    /// 音频回调热路径：只能由实际调用 process 的同一音频线程调用。该函数会
    /// 在不推进时间的前提下接收控制邮箱，供下游变速器按边界拆分输入。
    [[nodiscard]] AudioTimelineInputBoundary prepareInputBoundary(
        std::size_t maximumFrames) noexcept;

    /// @brief 获取最近一个已完成音频 block 的播放状态。
    [[nodiscard]] AudioTimelinePlaybackState state() const noexcept;

    /// @brief 获取逻辑线程最近请求的播放状态。
    /// @return 尚未到达 block 边界时也立即反映最新控制请求。
    [[nodiscard]] AudioTimelinePlaybackState requestedState() const noexcept;

    /// @brief 获取最近一个已完成音频 block 的传输纪元。
    [[nodiscard]] std::uint64_t epoch() const noexcept;

    /// @brief 判断非循环时间线是否已到达复合结束位置。
    [[nodiscard]] bool finished() const noexcept;

    /// @brief 获取谱面与全部采样共同决定的排除结束帧。
    [[nodiscard]] AudioTimelineFrame timelineEndFrame() const noexcept;

    /// @brief 获取排序并过滤后的有效采样数量。
    [[nodiscard]] std::size_t clipCount() const noexcept;

    /// @brief 设置复合时间线在逐事件音量之后应用的主增益。
    /// @param gain 非负线性增益；非有限值按零处理。
    /// @warning
    /// 逻辑线程写、音频线程逐 block 读取；relaxed 原子用于避免播放热路径加锁。
    void setMasterGain(float gain) noexcept;

    /// @brief 获取当前请求的复合时间线主增益。
    [[nodiscard]] float masterGain() const noexcept;

    /// @brief 获取复合时间线最近 block 的左声道峰值。
    [[nodiscard]] float leftLevel() const noexcept;

    /// @brief 获取复合时间线最近 block 的右声道峰值。
    [[nodiscard]] float rightLevel() const noexcept;

    /// @brief 生成一个设备音频 block。
    /// @param buffer 由上游预分配的输出缓冲区。
    /// @warning
    /// 音频回调热路径；禁止引入分配、锁、文件系统、资源解码、完整排序或日志。
    void process(ice::AudioBuffer& buffer) override;

private:
    /// @brief 控制邮箱中的目标播放命令。
    enum class PlaybackCommand : std::uint8_t {
        Stop,
        Play,
        Pause,
    };

    /// @brief 单个音频 block 使用的完整不可变片段表与线程内传输状态。
    struct ScheduleState {
        /// @brief 构造已排序片段对应的预分配调度状态。
        /// @param preparedClips 已完成过滤和排序的片段。
        /// @param requestedTimelineEndFrame 谱面物件决定的排除结束帧。
        /// @param maximumProcessFrames 单次缓存读取最大帧数。
        /// @param scheduleGeneration 当前调度在节点内的单调代次。
        ScheduleState(std::vector<PreparedTimelineClip> preparedClips,
                      AudioTimelineFrame requestedTimelineEndFrame,
                      std::size_t        maximumProcessFrames,
                      std::uint64_t      scheduleGeneration);

        /// @brief 有效且已排序的预备片段。
        std::vector<PreparedTimelineClip> clips;
        /// @brief 只由音频回调线程推进的确定性传输核心。
        AudioTimelineTransport transport;
        /// @brief 当前调度在同一 Mixer 节点内的单调代次。
        std::uint64_t generation{ 0U };
        /// @brief 谱面和采样共同决定的排除结束帧。
        AudioTimelineFrame timelineEndFrame{ 0 };
        /// @brief 单次缓存读取允许的最大帧数。
        std::size_t maximumProcessFrames{ 1U };
        /// @brief 回调外预分配的音频源读取缓存。
        ice::AudioBuffer sourceScratch;
        /// @brief 回调外预分配的活跃片段结果缓存。
        std::vector<AudioTimelineActiveSpan> activeSpanScratch;
        /// @brief 音频线程退役栈中的下一个状态。
        ScheduleState* nextRetired{ nullptr };
    };

    /// @brief 过滤、规范化并稳定排序预备片段。
    static std::vector<PreparedTimelineClip> prepareClips(
        std::vector<PreparedTimelineClip> clips);

    /// @brief 从已排序片段构造传输调度描述。
    static std::vector<TimelineClipSpec> buildClipSpecs(
        const std::vector<PreparedTimelineClip>& clips);

    /// @brief 计算谱面与全部音频片段的复合结束帧。
    static AudioTimelineFrame calculateTimelineEndFrame(
        const std::vector<PreparedTimelineClip>& clips,
        AudioTimelineFrame requestedTimelineEndFrame) noexcept;

    /// @brief 在 block 起点接管控制线程发布的新调度状态。
    /// @warning 音频热路径：只交换无锁指针并重建常数个传输状态。
    void applyPendingSchedule() noexcept;

    /// @brief 将旧调度状态压入控制线程回收的无锁退役栈。
    /// @param state 不再由音频线程访问的旧状态。
    /// @warning 音频热路径：只执行 lock-free 指针 CAS，不释放内存。
    void retireSchedule(ScheduleState* state) noexcept;

    /// @brief 按最近控制请求初始化刚交换进来的传输状态。
    /// @param state 新的音频线程状态。
    /// @warning 音频热路径：只读取固定数量原子邮箱并修改局部传输状态。
    void initializeReplacementTransport(ScheduleState& state) noexcept;

    /// @brief 标记时间线自然结束并通知下游提交当前 final 输入。
    /// @warning 音频热路径：每次自然结束只调用一次轻量函数指针。
    void markFinishedAndNotify() noexcept;

    /// @brief 在 block 边界读取稳定控制快照并更新传输状态。
    /// @warning 音频热路径；仅执行有界原子读取和常数时间状态修改。
    void applyPendingControls() noexcept;

    /// @brief 捕获控制应用完成且尚未处理循环边界时的传输纪元。
    /// @warning 音频热路径；只读取当前音频线程私有传输状态。
    void captureControlEpoch() noexcept;

    /// @brief 稳定读取当前请求的循环邮箱。
    /// @param enabled 返回循环是否启用。
    /// @param startFrame 返回循环包含起点。
    /// @param endFrame 返回循环排除终点。
    /// @return 邮箱没有并发写入且读取完整时返回 true。
    /// @warning 逻辑热路径；只执行一次有界序列锁读取。
    [[nodiscard]] bool tryReadRequestedLoop(
        bool& enabled, AudioTimelineFrame& startFrame,
        AudioTimelineFrame& endFrame) const noexcept;

    /// @brief 发布回调线程的最新传输快照。
    /// @warning
    /// 音频热路径；只执行固定数量 lock-free 序列锁原子操作，禁止加入锁、
    /// 分配或无界重试。
    void publishTransportSnapshot() noexcept;

    /// @brief 混合当前位置起始且不跨越循环或结束边界的一段输出。
    /// @param output 输出缓冲区。
    /// @param outputStartFrame 输出缓冲区内的起始偏移。
    /// @param frameCount 本段帧数，必须不超过预分配缓存容量。
    /// @warning 音频热路径；只读取完整缓存并写入预分配内存。
    void mixSegment(ice::AudioBuffer& output, std::size_t outputStartFrame,
                    std::size_t frameCount);

    /// @brief 应用主增益并发布当前输出 block 峰值。
    /// @param output 已完成逐片段混合的输出缓冲。
    /// @warning 音频热路径；只执行固定声道样本遍历和 relaxed 原子写入。
    void applyMasterGainAndPublishLevels(ice::AudioBuffer& output) noexcept;

    /// @brief 请求播放命令并发布一个完整序列锁写入。
    void requestPlaybackCommand(PlaybackCommand command) noexcept;

    /// @brief 运行时 BGM 区域与逐轨控制的稳定观察指针。
    /// @warning AudioManager 持有者生命周期必须覆盖本节点；音频回调
    /// 每个活跃片段每 block 解引用，禁止替换为共享所有权。
    const KeySoundControlBank* m_keySoundControls{ nullptr };

    /// @brief 当前只由音频线程访问的调度状态。
    ScheduleState* m_scheduleState{ nullptr };
    /// @brief 下一个由控制线程分配的调度代次。
    ///
    /// @warning 只允许单个非实时控制线程调用 replaceSchedule 并递增。
    std::uint64_t m_nextScheduleGeneration{ 1U };
    /// @brief 当前 block 应用控制后、处理循环边界前的传输纪元。
    std::uint64_t m_controlEpoch{ 0U };
    /// @brief prepareInputBoundary 已为紧随其后的 process 固定输入区间。
    ///
    /// @warning
    /// 仅由同一音频线程读写；用于阻止边界查询与实际拉取之间接收第二批控制命令。
    bool m_hasPreparedInputBoundary{ false };
    /// @brief 已固定的下一次 process 帧数。
    std::size_t m_preparedInputFrameCount{ 0U };
    /// @brief 控制线程发布、音频线程在 block 起点接管的待用状态。
    ///
    /// @warning 单控制线程写、单音频线程交换；裸指针原子必须 lock-free。
    std::atomic<ScheduleState*> m_pendingSchedule{ nullptr };
    /// @brief 音频线程压入、控制线程低频回收的状态栈。
    ///
    /// @warning 音频线程只执行 CAS，不在回调中析构或释放状态。
    std::atomic<ScheduleState*> m_retiredSchedules{ nullptr };
    /// @brief 最近发布调度的复合结束帧。
    std::atomic<AudioTimelineFrame> m_publishedTimelineEndFrame{ 0 };
    /// @brief 最近发布调度的有效片段数量。
    std::atomic<std::size_t> m_publishedClipCount{ 0U };
    /// @brief 自然结束通知的非拥有上下文。
    void* m_finalInputListenerContext{ nullptr };
    /// @brief 当前 block 末尾的自然结束通知函数。
    FinalInputListener m_finalInputListener{ nullptr };

    /// @brief 播放命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_playbackCommandSequence{ 0U };
    /// @brief 播放命令邮箱值。
    ///
    /// @warning 受 m_playbackCommandSequence 序列锁保护。
    std::atomic<PlaybackCommand> m_requestedPlaybackCommand{
        PlaybackCommand::Stop
    };
    /// @brief 播放命令提交时的 steady_clock 纳秒时间戳。
    ///
    /// @warning 受 m_playbackCommandSequence 序列锁保护。
    std::atomic<std::int64_t> m_requestedPlaybackSteadyTimeNanoseconds{ 0 };
    /// @brief 音频线程最后应用的播放命令版本。
    std::uint64_t m_appliedPlaybackCommandSequence{ 0U };
    /// @brief 最近一致发布快照已应用的播放命令版本。
    ///
    /// @warning 音频线程写、逻辑线程在时钟序列锁内读取。
    std::atomic<std::uint64_t> m_publishedAppliedPlaybackCommandSequence{ 0U };

    /// @brief Seek 命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_seekSequence{ 0U };
    /// @brief Seek 目标帧邮箱值。
    ///
    /// @warning 受 m_seekSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedSeekFrame{ 0 };
    /// @brief Seek 命令提交时的 steady_clock 纳秒时间戳。
    ///
    /// @warning 受 m_seekSequence 序列锁保护。
    std::atomic<std::int64_t> m_requestedSeekSteadyTimeNanoseconds{ 0 };
    /// @brief 音频线程最后应用的 Seek 版本。
    std::uint64_t m_appliedSeekSequence{ 0U };
    /// @brief 最近一次已完成 block 应用的 Seek 版本。
    ///
    /// @warning
    /// 音频线程发布、逻辑线程读取；用于在暂停期间立即显示尚未被回调接收的
    /// Seek 目标，避免为推进控制邮箱而伪造静音音频 block。
    std::atomic<std::uint64_t> m_publishedAppliedSeekSequence{ 0U };

    /// @brief 循环命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_loopSequence{ 0U };
    /// @brief 循环起始帧邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedLoopStartFrame{ 0 };
    /// @brief 循环排除结束帧邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedLoopEndFrame{ 0 };
    /// @brief 循环启用邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<bool> m_requestedLoopEnabled{ false };
    /// @brief 音频线程最后应用的循环命令版本。
    std::uint64_t m_appliedLoopSequence{ 0U };

    /// @brief 回调发布给逻辑线程的最新位置。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<AudioTimelineFrame> m_publishedPositionFrame{ 0 };
    /// @brief 最近一次 process 开始生成输出时的时间线位置。
    ///
    /// @warning
    /// 音频线程写入、同一混音 block 内后续音效源读取；relaxed
    /// 顺序配合固定来源执行顺序使用。
    std::atomic<AudioTimelineFrame> m_publishedBlockStartFrame{ 0 };
    /// @brief 回调发布给逻辑线程的最新状态。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<AudioTimelinePlaybackState> m_publishedState{
        AudioTimelinePlaybackState::Stopped
    };
    /// @brief 回调发布给逻辑线程的最新纪元。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<std::uint64_t> m_publishedEpoch{ 0U };
    /// @brief 最近一致快照中控制应用完成时的传输纪元。
    ///
    /// @warning 只允许在 m_clockSnapshotSequence 保护下读取。
    std::atomic<std::uint64_t> m_publishedControlEpoch{ 0U };
    /// @brief 最近一致快照中的调度代次。
    ///
    /// @warning 只允许在 m_clockSnapshotSequence 保护下读取。
    std::atomic<std::uint64_t> m_publishedScheduleGeneration{ 0U };
    /// @brief 回调发布给逻辑线程的自然结束标记。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<bool> m_publishedFinished{ false };

    /// @brief 一致时钟快照的序列锁版本。
    ///
    /// @warning
    /// 音频线程单写、逻辑线程多读；写入奇数表示发布中，偶数表示完整快照。
    std::atomic<std::uint64_t> m_clockSnapshotSequence{ 0U };
    /// @brief 最近一致快照发布时的 steady_clock 纳秒时间戳。
    ///
    /// @warning 只允许在 m_clockSnapshotSequence 保护下读取。
    std::atomic<std::int64_t> m_publishedSteadyTimeNanoseconds{ 0 };
    /// @brief 最近一致快照中的自然结束标记。
    ///
    /// @warning 只允许在 m_clockSnapshotSequence 保护下读取。
    std::atomic<bool> m_clockPublishedFinished{ false };

    /// @brief 逐事件音量之后应用的复合时间线主增益。
    ///
    /// @warning
    /// 逻辑线程写、音频线程逐 block 读取；relaxed 顺序只要求获取最新独立值。
    std::atomic<float> m_masterGain{ 1.0F };

    /// @brief 最近输出 block 的左声道峰值。
    ///
    /// @warning 音频线程写、逻辑线程读；relaxed 顺序只提供独立电平快照。
    std::atomic<float> m_leftLevel{ 0.0F };

    /// @brief 最近输出 block 的右声道峰值。
    ///
    /// @warning 音频线程写、逻辑线程读；relaxed 顺序只提供独立电平快照。
    std::atomic<float> m_rightLevel{ 0.0F };
};

}  // namespace MMM::Audio
