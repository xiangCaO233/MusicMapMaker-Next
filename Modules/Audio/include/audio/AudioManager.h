#pragma once

#include "audio/StereoGainEnvelope.h"
#include "config/EditorSettings.h"
#include "mmm/project/AudioResource.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ice
{
class AudioPool;
class ALPlayer;
class IReceiver;
class SDLPlayer;
class AudioTrack;
class SourceNode;
class ThreadPool;
class MixBus;
class TimeStretcher;
class GraphicEqualizer;
}  // namespace ice

namespace MMM::Audio
{

class SoundEffectPool;
class AudioTimelineMixerNode;
class PreparedTimelineAudio;
struct BackgroundSpectrumLevels;
class BackgroundSpectrumAnalyzer;
class BackgroundSpectrumCaptureNode;

/**
 * @brief 播放状态
 */
enum class PlaybackStatus {
    /// @brief 已停止播放。
    Stopped,

    /// @brief 正在播放。
    Playing,

    /// @brief 已暂停播放。
    Paused
};

/**
 * @brief EQ 频段预设
 */
enum class EQPreset {
    /// @brief 不启用 EQ。
    None,

    /// @brief 十段 EQ：31、62、125、250、500、1k、2k、4k、8k、16k。
    TenBand,

    /// @brief 十五段
    /// EQ：25、40、63、100、160、250、400、630、1k、1.6k、2.5k、4k、6.3k、10k、16k。
    FifteenBand
};

/** @brief 双声道输出模式。 */
enum class MixerChannelMode {
    Stereo,           ///< 保持原始立体声输出。
    MuteLeft,         ///< 静音左声道，仅保留右声道。
    MuteRight,        ///< 静音右声道，仅保留左声道。
    CopyLeftToRight,  ///< 将左声道复制到右声道，两侧都播放左声道。
    CopyRightToLeft   ///< 将右声道复制到左声道，两侧都播放右声道。
};

/// @brief 交给 AudioManager 准备的单个自动采样事件。
struct AudioTimelineLoadEvent {
    /// @brief 谱面内稳定且唯一的采样物件标识。
    std::uint64_t eventId{ 0U };

    /// @brief 项目音频资源的稳定标识。
    std::string resourceKey;

    /// @brief 音频资源的绝对文件路径。
    std::string filePath;

    /// @brief 已合并物件 offset 的实际起播时间，单位为秒，允许为负。
    double effectiveStartSeconds{ 0.0 };

    /// @brief 采样物件自身的线性音量。
    float eventVolume{ 1.0F };

    /// @brief 被引用音频资源的持久化配置。
    AudioTrackConfig resourceConfig;
};

/// @brief 音频时间线加载诊断类型。
enum class AudioTimelineLoadDiagnosticCode : std::uint8_t {
    /// @brief AudioManager 尚未初始化，无法构造音频图。
    AudioSystemUnavailable,

    /// @brief 文件路径为空、无法探测或完整解码结果为空。
    MissingResource,

    /// @brief 起播时间不是有限数值，已按零秒恢复。
    InvalidStartTime
};

/// @brief 单个时间线加载问题及其来源。
struct AudioTimelineLoadDiagnostic {
    /// @brief 诊断类型。
    AudioTimelineLoadDiagnosticCode code{
        AudioTimelineLoadDiagnosticCode::MissingResource
    };

    /// @brief 相关采样物件标识。
    std::uint64_t eventId{ 0U };

    /// @brief 相关项目音频资源标识。
    std::string resourceKey;

    /// @brief 相关资源文件路径。
    std::string filePath;

    /// @brief 可直接展示给用户的诊断说明。
    std::string message;
};

/// @brief 构造复合音频时间线的结果。
struct AudioTimelineLoadResult {
    /// @brief 音频图是否已成功替换；即使没有有效片段也可为 true。
    bool success{ false };

    /// @brief 第一阶段提交给解码池的唯一非空文件路径数量。
    std::size_t requestedSourceCount{ 0U };

    /// @brief 第二阶段成功建立的唯一资源 DSP 结果数量。
    std::size_t preparedResourceCount{ 0U };

    /// @brief 成功载入调度表的采样物件数量。
    std::size_t loadedClipCount{ 0U };

    /// @brief 因资源缺失而未进入调度表的采样物件数量。
    std::size_t missingClipCount{ 0U };

    /// @brief 可展示给用户或写入导入报告的非致命诊断。
    std::vector<AudioTimelineLoadDiagnostic> diagnostics;
};

/// @brief 音频输出设备列表项。
struct AudioOutputDevice {
    /// @brief 后端报告的设备名称；为空时表示系统默认设备。
    std::string name;

    /// @brief 是否为“系统默认设备”占位项。
    bool isDefault{ false };
};

/**
 * @brief 音频管理器，封装 IonCachyEngine 的核心功能
 */
class AudioManager
{
public:
    static AudioManager& instance();

    /// @brief 初始化音频后端和引擎
    void init();

    /// @brief 关闭音频引擎
    void shutdown();

    /// @brief 切换音频播放后端。
    /// @param backend 目标播放后端。
    /// @return 切换成功时返回 true。
    bool setPlaybackBackend(Config::AudioPlaybackBackend backend);

    /// @brief 获取当前正在使用的音频播放后端。
    /// @return 当前播放后端。
    Config::AudioPlaybackBackend getPlaybackBackend() const;

    /// @brief 枚举当前播放后端可用的输出设备。
    /// @return 输出设备列表，第一项始终为系统默认设备。
    /// @warning 低频 UI 路径：可能查询系统音频后端，调用方必须缓存结果，
    /// 禁止在每帧热路径中重复枚举。
    std::vector<AudioOutputDevice> listOutputDevices() const;

    /// @brief 设置当前播放后端使用的输出设备。
    /// @param deviceName 设备名称；空字符串表示系统默认设备。
    /// @return 成功切换并持久化时返回 true。
    /// @warning 低频后端切换路径：会重建音频播放器，禁止在播放热路径中调用。
    bool setOutputDeviceName(const std::string& deviceName);

    /// @brief 获取当前播放后端配置的输出设备名称。
    /// @return 设备名称；空字符串表示系统默认设备。
    const std::string& getOutputDeviceName() const;

    /// @brief 设置 OpenAL 后端空间化输出参数。
    /// @param config OpenAL 空间化配置。
    /// @return 参数已接受时返回 true；非 OpenAL 后端会缓存参数并返回 false。
    bool setOpenALSpatialConfig(const Config::OpenALSpatialConfig& config);

    /// @brief 获取当前 OpenAL 空间化输出配置。
    /// @return OpenAL 空间化配置。
    const Config::OpenALSpatialConfig& getOpenALSpatialConfig() const;

    /// @brief 加载完整自动采样时间线并替换当前主播放时钟。
    /// @param events 待缓存和调度的自动采样事件。
    /// @param chartEndSeconds 玩家物件等非音频内容决定的谱面结束时间。
    /// @param fingerprint 调用方生成的稳定完整时间线指纹。
    /// @return 图替换状态、有效片段数量和逐事件诊断。
    /// @warning
    /// 低频资源路径：会访问文件系统、等待引用资源完成解码并执行资源级离线
    /// DSP，只能在谱面加载或时间线提交时调用，禁止放入 UI、逻辑 update
    /// 或音频回调热路径。
    [[nodiscard]] AudioTimelineLoadResult loadAudioTimeline(
        const std::vector<AudioTimelineLoadEvent>& events,
        double chartEndSeconds, const std::string& fingerprint);

    /// @brief 卸载自动采样时间线并断开主时间线音频图。
    /// @warning 低频资源切换路径；会停止全部已调度 HitEffect。
    void unloadAudioTimeline();

    /// @brief 获取当前完整时间线的稳定指纹。
    /// @return 未加载时间线时返回空字符串。
    [[nodiscard]] const std::string& getLoadedAudioTimelineFingerprint() const;

    /// @brief 获取当前成功进入调度表的采样片段数量。
    [[nodiscard]] std::size_t getLoadedAudioTimelineClipCount() const;

    /// @brief 获取上次加载时缺失的采样片段数量。
    [[nodiscard]] std::size_t getMissingAudioTimelineClipCount() const;

    /// @brief 判断当前是否存在已构造的时间线时钟。
    [[nodiscard]] bool hasLoadedAudioTimeline() const;

    /// @brief 启用主时间线半开区间循环。
    /// @param startSeconds 循环起点，单位为秒。
    /// @param endSeconds 循环排除终点，单位为秒。
    /// @return 参数有效且已提交到时间线时返回 true。
    bool setAudioTimelineLoop(double startSeconds, double endSeconds);

    /// @brief 关闭主时间线循环。
    void clearAudioTimelineLoop();

    /// @brief 加载单文件 BGM 的兼容入口。
    /// @param filePath 音频文件绝对路径。
    /// @param config 轨道详细配置。
    /// @return 单文件成功进入自动采样调度表时返回 true。
    /// @warning
    /// 此接口仅物化一个零秒自动采样；其 SourceNode 不再充当主播放时钟。
    /// 资源自身 playbackSpeed、pitch 和 EQ 会离线应用于该片段，不会提升为
    /// 复合时间线全局预览参数。
    bool loadBGM(const std::string& filePath, const AudioTrackConfig& config);

    /// @brief 兼容入口：卸载当前主自动采样时间线。
    void unloadBGM();

    /// @brief 开始/恢复播放
    void play();

    /// @brief 暂停播放
    void pause();

    /// @brief 停止播放并回到起始位置
    void stop();

    /// @brief 跳转到指定时间
    /// @param seconds 秒
    void seek(double seconds);

    /// @brief 获取当前播放状态
    PlaybackStatus getStatus() const;

    /// @brief 获取当前播放时间 (秒)
    double getCurrentTime() const;

    /// @brief 获取总时长 (秒)
    double getTotalTime() const;

    /// @brief 将音轨加载到独立试听通道，不替换当前 BGM。
    /// @param filePath 音频文件绝对路径。
    /// @param config 试听音轨的音量、静音和初始倍速配置。
    /// @return 加载并接入主混音器成功时返回 true。
    /// @warning
    /// 低频资源路径：可能触发音频解码缓存加载，禁止在每帧热路径中调用。
    bool loadAuditionTrack(const std::string&      filePath,
                           const AudioTrackConfig& config);

    /// @brief 卸载独立试听音轨并断开其混音节点。
    void unloadAuditionTrack();

    /// @brief 开始或恢复独立试听音轨播放。
    void playAudition();

    /// @brief 暂停独立试听音轨播放。
    void pauseAudition();

    /// @brief 停止独立试听音轨并回到起始位置。
    void stopAudition();

    /// @brief 跳转独立试听音轨播放位置。
    /// @param seconds 目标时间，单位为秒。
    void seekAudition(double seconds);

    /// @brief 获取独立试听音轨的当前播放状态。
    /// @return 当前试听播放状态。
    PlaybackStatus getAuditionStatus() const;

    /// @brief 获取独立试听音轨当前播放时间。
    /// @return 当前播放时间，单位为秒。
    double getAuditionCurrentTime() const;

    /// @brief 获取独立试听音轨总时长。
    /// @return 总时长，单位为秒。
    double getAuditionTotalTime() const;

    /// @brief 设置独立试听音轨播放倍率。
    /// @param speed 目标播放倍率。
    void setAuditionPlaybackSpeed(double speed);

    /// @brief 获取独立试听音轨请求的播放倍率。
    /// @return 当前请求的播放倍率。
    double getAuditionPlaybackSpeed() const;

    /// @brief 获取独立试听拉伸器实际生效的播放倍率。
    /// @return 当前实际播放倍率。
    double getActualAuditionPlaybackSpeed() const;

    /// @brief 获取独立试听通道当前加载的音频路径。
    /// @return 音频文件路径；未加载时返回空字符串。
    const std::string& getLoadedAuditionPath() const;

    /// @brief 设置复合时间线主增益 (0.0 ~ 1.0)
    void setMainTrackVolume(float volume);

    /// @brief 获取复合时间线当前主增益
    float getMainTrackVolume() const;

    /// @brief 设置复合时间线主静音状态
    void setMainTrackMute(bool muted);

    /// @brief 获取复合时间线主静音状态
    bool isMainTrackMuted() const;

    /// @brief 设置全局音量 (0.0 ~ 1.0)
    void setGlobalVolume(float volume);

    /// @brief 获取全局音量
    float getGlobalVolume() const;

    /// @brief 设置全局静音状态
    void setGlobalMute(bool muted);

    /// @brief 获取全局是否静音
    bool isGlobalMuted() const;

    /// @brief 获取输出电平 (左声道 0.0 ~ 1.0)
    float getOutputLevelL() const;

    /// @brief 获取输出电平 (右声道 0.0 ~ 1.0)
    float getOutputLevelR() const;

    /// @brief 获取 BGM 增益静音状态
    bool isBGMGainMuted() const;
    /// @brief 设置 BGM 增益静音
    void setBGMGainMute(bool muted);

    /// @brief 获取 SFX 增益静音状态
    bool isSFXGainMuted() const;
    /// @brief 设置 SFX 增益静音
    void setSFXGainMute(bool muted);

    /// @brief 获取交互音效增益静音状态
    bool isInteractionSFXGainMuted() const;
    /// @brief 设置交互音效增益静音
    void setInteractionSFXGainMute(bool muted);

    /// @brief 获取主音轨 (BGM) 的实时电平 (L)
    float getMainTrackLevelL() const;
    /// @brief 获取主音轨 (BGM) 的实时电平 (R)
    float getMainTrackLevelR() const;

    /// @brief 刷新背景频谱使用的实时立体声频段。
    /// @param bandCount 每个声道请求的频段数。
    /// @param includeHitEffects 是否混入实际播放的 HitEffect 总线。
    /// @return 内部稳定保存的归一化频段电平。
    /// @warning 逻辑更新热路径：启用背景频谱时每次主画布快照调用一次，执行固定
    /// 2048 点 FFT；返回引用只在下一次调用前保持内容不变。
    [[nodiscard]] const BackgroundSpectrumLevels& updateBackgroundSpectrum(
        std::size_t bandCount, bool includeHitEffects);

    /// @brief 获取特定音效池的实时电平 (L)
    float getSFXPoolLevelL(const std::string& key) const;
    /// @brief 获取特定音效池的实时电平 (R)
    float getSFXPoolLevelR(const std::string& key) const;

    /// @brief 设置主混音器左声道静音
    void setMainMixerLeftMute(bool muted);
    /// @brief 获取主混音器左声道是否静音
    bool isMainMixerLeftMuted() const;

    /// @brief 设置主混音器右声道静音
    void setMainMixerRightMute(bool muted);
    /// @brief 获取主混音器右声道是否静音
    bool isMainMixerRightMuted() const;

    /// @brief 设置主混音器双声道输出模式。
    /// @param mode 目标声道输出模式。
    void setMainMixerChannelMode(MixerChannelMode mode);

    /// @brief 获取主混音器双声道输出模式。
    /// @return 当前声道输出模式。
    MixerChannelMode getMainMixerChannelMode() const;

    /// @brief 设置播放倍率 (0.5 ~ 2.0)
    void setPlaybackSpeed(double speed);

    /// @brief 获取当前播放倍率
    double getPlaybackSpeed() const;

    /// @brief 获取实际生效的播放倍率
    double getActualPlaybackSpeed() const;

    /// @brief 设置复合时间线全局预览音高偏移 (半音，-24.0 ~ +24.0)
    void setPlaybackPitch(double semitones);

    /// @brief 获取复合时间线全局预览音高偏移
    double getPlaybackPitch() const;

    /// @brief 主音轨时间拉伸质量。
    enum class StretchQuality {
        /// @brief 快速质量，优先降低处理成本。
        Fast,

        /// @brief 平衡质量，在性能和音质之间折中。
        Balanced,

        /// @brief 较细质量，优先提升音质。
        Finer,

        /// @brief 最佳质量，使用最高质量设置。
        Best
    };

    /// @brief 设置主音轨拉伸质量
    void setPlaybackQuality(StretchQuality quality);

    /// @brief 获取主音轨拉伸质量
    StretchQuality getPlaybackQuality() const;

    /// @brief 设置 BGM 全局增益 (0.0 ~ 1.0)
    void setBGMGain(float gain);
    /// @brief 获取 BGM 全局增益
    float getBGMGain() const;

    /// @brief 设置 SFX 全局增益 (0.0 ~ 1.0)
    void setSFXGain(float gain);
    /// @brief 获取 SFX 全局增益
    float getSFXGain() const;

    /// @brief 设置交互音效全局增益 (0.0 ~ 1.0)
    void setInteractionSFXGain(float gain);
    /// @brief 获取交互音效全局增益
    float getInteractionSFXGain() const;

    // --- EQ 相关接口 ---

    /// @brief 为复合时间线创建全局预览均衡器
    /// @param preset 预设类型
    void createMainTrackEQ(EQPreset preset);

    /// @brief 销毁复合时间线全局预览均衡器
    void destroyMainTrackEQ();

    /// @brief 设置指定频段的增益
    /// @param bandIndex 频段索引
    /// @param gainDb 增益 (dB)
    void setMainTrackEQBandGain(size_t bandIndex, float gainDb);

    /// @brief 获取指定频段的增益
    /// @param bandIndex 频段索引
    /// @return 增益 (dB)
    float getMainTrackEQBandGain(size_t bandIndex) const;

    /// @brief 设置指定频段的 Q 值
    /// @param bandIndex 频段索引
    /// @param q Q 值
    void setMainTrackEQBandQ(size_t bandIndex, float q);

    /// @brief 获取指定频段的 Q 值
    /// @param bandIndex 频段索引
    /// @return Q 值
    float getMainTrackEQBandQ(size_t bandIndex) const;

    /// @brief 获取当前 EQ 频段数量
    size_t getMainTrackEQBandCount() const;

    /// @brief 获取指定频段的中心频率
    /// @param bandIndex 频段索引
    /// @return 中心频率 (Hz)
    float getMainTrackEQBandFrequency(size_t bandIndex) const;

    /// @brief 获取当前是否启用了 EQ
    bool isMainTrackEQEnabled() const;

    /// @brief 获取当前 EQ 预设类型
    EQPreset getMainTrackEQPreset() const;

    /// @brief 获取 EQ 在指定频率处的幅频响应 (dB)
    /// @param frequency 频率 (Hz)
    float getMainTrackEQResponse(float frequency) const;

    // --- SFX 相关接口 ---
    /// @param key 标识符
    /// @param volume 音量
    /// @param isPermanent 是否为常驻音效 (若为真，则保存到软件配置)
    void setSFXPoolVolume(const std::string& key, float volume,
                          bool isPermanent = false);

    /// @brief 设置特定 SFX 池的静音状态
    void setSFXPoolMute(const std::string& key, bool muted,
                        bool isPermanent = false);

    /// @brief 获取特定 SFX 池的音量
    float getSFXPoolVolume(const std::string& key) const;

    /// @brief 获取特定 SFX 池是否静音
    bool getSFXPoolMute(const std::string& key) const;

    /// @brief 获取特定 SFX 池的时长
    double getSFXDuration(const std::string& key) const;

    /// @brief 诊断指定音效池是否与自动采样共用同一份预处理 PCM。
    /// @param key 项目 Effect 资源标识。
    /// @return 池与当前资源 DSP 弱缓存指向同一对象时返回 true。
    /// @warning 低频测试与诊断接口：会查询哈希表并锁定 weak_ptr。
    [[nodiscard]] bool isSFXUsingSharedTimelineAudio(
        const std::string& key) const;

    /// @brief 实时更新打击音效路由策略。
    /// @param syncSpeed 是否让 hiteffect.* 音效跟随主音轨拉伸器。
    void updateSFXSyncSpeedRouting(bool syncSpeed);

    /// @brief 获取特定 SFX 池最近一次播放进度
    double getSFXPlaybackTime(const std::string& key) const;

    /// @brief 登记可按需加载的音效文件，不执行解码或创建混音节点。
    /// @param key 音效标识符。
    /// @param filePath 音效文件绝对路径。
    /// @param defaultVolume 首次加载时使用的默认音量。
    /// @param leadInSeconds 文件开头到有效出声点的延迟。
    /// @warning 低频资源登记路径：只更新内存描述，不访问文件系统。
    void registerSoundEffect(const std::string& key,
                             const std::string& filePath,
                             float              defaultVolume = 1.0f,
                             double             leadInSeconds = 0.0);

    /// @brief 登记使用项目资源完整 DSP 配置的按需音效。
    /// @param key 项目 Effect 资源标识。
    /// @param filePath 音效文件绝对路径。
    /// @param resourceConfig 与自动采样共享的资源级配置。
    /// @param leadInSeconds 文件开头到有效出声点的延迟。
    /// @warning
    /// 低频资源登记路径：只更新描述和后台队列；不会在调用线程解码或执行
    /// DSP。volume/mute 保持为播放增益，其余资源 DSP 由共享 PCM 缓存应用。
    void registerSoundEffect(const std::string&      key,
                             const std::string&      filePath,
                             const AudioTrackConfig& resourceConfig,
                             double                  leadInSeconds = 0.0);

    /// @brief 确保已登记音效完成解码并接入混音器。
    /// @param key 音效标识符。
    /// @return 已加载或成功加载时返回 true。
    /// @warning 低频显式加载路径：可能访问文件系统并等待解码，禁止在每帧
    /// UI、渲染、逻辑 update 或音频回调中调用。
    bool ensureSoundEffectLoaded(const std::string& key);

    /// @brief 将物件绑定音效加入后台按需加载队列。
    /// @param key 谱面物件绑定的项目音效资源标识。
    /// @return 已加载、已排队或成功加入队列时返回 true。
    /// @warning 逻辑预读热路径：仅访问内存登记表并对首次出现的资源排队，
    /// 不得访问文件系统或等待解码。
    bool queueBoundNoteSoundEffectLoad(const std::string& key);

    /// @brief 推进后台音效加载任务并回收已退役的时间线调度资源。
    /// @param maxPreparedPerUpdate 单次调用最多接入的音效数量。
    /// @warning
    /// 逻辑轮询路径：单次只启动固定数量后台任务，文件探测和解码均在线程池
    /// 执行。无退役状态时只执行一次无锁指针读取；时间线发生替换时可能在
    /// 控制线程析构 PCM 缓存，禁止放入音频回调或渲染路径。
    void updateQueuedSoundEffectLoads(std::size_t maxPreparedPerUpdate = 2U);

    /// @brief 查询指定音效是否已经完成解码并创建音效池。
    /// @param key 音效标识符。
    /// @return 音效池已存在时返回 true。
    bool isSoundEffectLoaded(const std::string& key) const;

    /// @brief 预加载音效文件
    /// @param key 标识符（如 "hiteffect.note"）
    /// @param filePath 音效文件绝对路径
    /// @param defaultVolume 初始默认音量
    /// @param leadInSeconds 文件开头到有效出声点的延迟，调度时会提前抵消
    /// @return 是否加载成功
    bool preloadSoundEffect(const std::string& key, const std::string& filePath,
                            float  defaultVolume = 1.0f,
                            double leadInSeconds = 0.0);

    /// @brief 卸载并释放指定 key 的音效
    /// @param key 标识符
    void unloadSoundEffect(const std::string& key);

    /// @brief 停止并释放所有已加载音效池。
    /// @warning 低频资源重载路径：皮肤热切换时调用，会清空所有 SFX pool
    /// 和调度状态，禁止放入播放热路径。
    void clearSoundEffects();

    /// @brief 播放指定 key 的音效
    /// @param key 标识符
    /// @param volumeFactor 额外音量倍率 (默认 1.0)
    /// @param pitchSemitones 本次播放的音高偏移，单位为半音。
    void playSoundEffect(const std::string& key, float volumeFactor = 1.0f,
                         double pitchSemitones = 0.0);

    /// @brief 获取指定 key 的音效是否正在播放
    bool isSFXPlaying(const std::string& key) const;

    /// @brief 获取指定 key 的音效是否处于暂停状态
    bool isSFXPaused(const std::string& key) const;

    /// @brief 暂停指定 key 的音效播放
    void pauseSoundEffect(const std::string& key);

    /// @brief 恢复指定 key 的音效播放
    void resumeSoundEffect(const std::string& key);

    /// @brief 在指定时间播放音效（预测系统使用）
    /// @param key 标识符
    /// @param targetTime 目标播放时间 (秒)
    /// @param volumeFactor 额外音量倍率
    /// @param stereoEnvelope 本次播放的线性双声道增益包络；仅 SDL
    /// 后端应用。
    void playSoundEffectScheduled(
        const std::string& key, double targetTime, float volumeFactor = 1.0f,
        const StereoGainEnvelope& stereoEnvelope = {});

    /// @brief 清空并停止所有正在播放和预定的音效
    void clearAllScheduledSoundEffects();

    /// @brief 获取当前时间线首个成功加载的轨道数据，供旧可视化入口兼容。
    std::shared_ptr<ice::AudioTrack> getBGMTrack() const;

    /// @brief 获取单片段兼容时间线的文件路径。
    /// @return 复合时间线或未加载时返回空字符串。
    const std::string& getLoadedBGMPath() const;

    /// @brief 获取当前时间线兼容同步键。
    /// @return 完整时间线指纹；未加载时为空。
    const std::string& getLoadedBGMSyncKey() const;

    /// @brief 获取独立试听文件的规范化绝对路径键。
    /// @return 与 Session 主音轨同步键格式一致的路径键；未加载时为空。
    const std::string& getLoadedAuditionSyncKey() const;

    /// @brief 使指定音频文件的解码缓存失效。
    /// @param filePath UTF-8 音频文件绝对路径。
    void invalidateTrackCache(const std::string& filePath);

    /// @brief 加载或复用音频资源池中的轨道，供离线分析工具读取。
    /// @param filePath 音频文件绝对路径。
    /// @return 加载成功时返回音频轨道；失败时返回空指针。
    /// @warning 低频分析路径：可能触发音频解码缓存加载，严禁在每帧
    /// UI、渲染或逻辑热路径中调用。
    std::shared_ptr<ice::AudioTrack> loadTrackForAnalysis(
        const std::string& filePath);

private:
    /// @brief 构造音频管理器并读取持久化音量配置。
    AudioManager();

    /// @brief 析构音频管理器。
    ~AudioManager();

    /// @brief 根据音效类型获取当前有效基础音量。
    /// @param key 音效池标识。
    /// @return 已包含全局音量和对应总线增益的基础音量。
    float getSFXEffectiveGain(const std::string& key) const;

    /// @brief 判断指定音效是否应接入打击音效总线。
    /// @param key 音效资源标识。
    /// @return 内置打击音效或谱面绑定采样返回 true。
    bool usesHitEffectRouting(const std::string& key) const;

    /// @brief 使用已完成资源 DSP 的 PCM 创建音效池并接入混音图。
    /// @param key 已登记的音效资源标识。
    /// @param preparedAudio 与自动采样时间线共享的只读 PCM。
    /// @return 成功接入或已存在时返回 true。
    bool attachSoundEffectPool(
        const std::string&                           key,
        std::shared_ptr<const PreparedTimelineAudio> preparedAudio);

    /// @brief 断开并释放音效池，但保留资源登记、静音和后台修订状态。
    /// @param key 音效资源标识。
    void detachSoundEffectPool(const std::string& key);

    /// @brief 统一登记皮肤音效或项目 Effect。
    /// @param key 音效资源标识。
    /// @param filePath 音频文件绝对路径。
    /// @param resourceConfig 资源配置。
    /// @param leadInSeconds 有效声音前导秒数。
    /// @param usesProjectResourceConfig 是否以项目配置而非 AppConfig 为权威。
    void registerSoundEffectImpl(const std::string&      key,
                                 const std::string&      filePath,
                                 const AudioTrackConfig& resourceConfig,
                                 double                  leadInSeconds,
                                 bool usesProjectResourceConfig);

    /// @brief 查找或建立自动采样与 HitEffect 共用的资源 DSP PCM。
    /// @param filePath 音频文件绝对路径。
    /// @param track 已完成解码的原始音轨。
    /// @param resourceConfig 资源 DSP 配置。
    /// @param preparedCandidate 后台线程可预先提供的处理结果。
    /// @return 缓存命中或处理成功时返回共享只读 PCM。
    /// @warning 低频控制路径：缓存未命中且无候选时会执行完整离线 DSP。
    std::shared_ptr<const PreparedTimelineAudio>
    getOrPrepareAudioTimelineResource(
        const std::string&                           filePath,
        const std::shared_ptr<ice::AudioTrack>&      track,
        const AudioTrackConfig&                      resourceConfig,
        std::shared_ptr<const PreparedTimelineAudio> preparedCandidate = {});

    /// @brief 等待所有后台音效文件探测任务完成。
    /// @warning 仅允许在 AudioManager 关闭路径调用，会阻塞等待线程池任务。
    void waitForQueuedSoundEffectLoads();

    /// @brief 刷新所有音效池当前播放节点的有效音量。
    void refreshSFXEffectiveVolumes();

    /// @brief 刷新独立试听源的有效音量。
    void refreshAuditionTrackVolume();

    /// @brief 刷新自动采样时间线的主总线有效音量。
    void refreshAudioTimelineVolume();

    /// @brief 清除主时间拉伸器的历史样本。
    /// @warning
    /// 低频播放控制路径：只写入 lock-free discontinuity 邮箱，禁止每帧调用。
    void resetMainTimeStretcher();

    /// @brief 创建并启动指定播放后端。
    /// @param backend 目标播放后端。
    /// @param allowDefaultDeviceFallback 指定设备打开失败时是否回退到默认设备。
    /// @return 成功创建并启动时返回 true。
    bool createPlaybackBackend(Config::AudioPlaybackBackend backend,
                               bool allowDefaultDeviceFallback = true);

    /// @brief 停止并释放当前播放后端。
    void destroyPlaybackBackend();

    /// @brief 获取指定后端配置的输出设备名称。
    /// @param backend 目标播放后端。
    /// @return 设备名称；空字符串表示系统默认设备。
    const std::string& getConfiguredOutputDeviceName(
        Config::AudioPlaybackBackend backend) const;

    /// @brief 更新指定后端配置的输出设备名称。
    /// @param backend 目标播放后端。
    /// @param deviceName 设备名称；空字符串表示系统默认设备。
    void setConfiguredOutputDeviceName(Config::AudioPlaybackBackend backend,
                                       const std::string&           deviceName);

    /// @brief 将缓存的 OpenAL 空间化参数应用到当前后端。
    /// @return 当前后端为 OpenAL 并成功应用时返回 true。
    bool applyOpenALSpatialConfig();

    /// @brief 音频后台线程池。
    /// @warning 生命周期由 Runtime::AppThreadPool 和 GameLoop
    /// 管理；AudioManager 只在低频加载/解码路径解引用，不拥有也不释放。
    ice::ThreadPool* m_threadPool{ nullptr };

    /// @brief 音频资源池，负责加载和缓存音频文件。
    std::unique_ptr<ice::AudioPool> m_audioPool;

    /// @brief 当前播放后端抽象接收器。
    std::unique_ptr<ice::IReceiver> m_player;

    /// @brief 当前播放后端类型。
    Config::AudioPlaybackBackend m_playbackBackend{
        Config::AudioPlaybackBackend::SDL
    };

    /// @brief SDL 后端配置的输出设备名称；为空时使用系统默认设备。
    std::string m_sdlOutputDeviceName;

    /// @brief OpenAL 后端配置的输出设备名称；为空时使用系统默认设备。
    std::string m_openALOutputDeviceName;

    /// @brief 当前 OpenAL 后端观察指针，不拥有对象。
    ice::ALPlayer* m_openALPlayer{ nullptr };

    /// @brief OpenAL 空间化输出配置缓存。
    Config::OpenALSpatialConfig m_openALSpatialConfig;

    /// @brief 当前加载的主音轨数据。
    std::shared_ptr<ice::AudioTrack> m_bgmTrack;

    /// @brief 当前加载的主音轨文件路径。
    std::string m_bgmPath;

    /// @brief 当前主音轨文件的规范化绝对路径键。
    std::string m_bgmSyncKey;

    /// @brief 当前自动采样时间线的唯一传输和混音节点。
    std::shared_ptr<AudioTimelineMixerNode> m_audioTimelineNode;

    /// @brief 稳定输出节点中是否已经提交一份谱面时间线。
    bool m_audioTimelineLoaded{ false };

    /// @brief 当前完整自动采样时间线的稳定指纹。
    std::string m_audioTimelineFingerprint;

    /// @brief 当前有效自动采样片段数量。
    std::size_t m_audioTimelineClipCount{ 0U };

    /// @brief 上次加载时缺失的自动采样片段数量。
    std::size_t m_missingAudioTimelineClipCount{ 0U };

    /// @brief 跨相邻时间线提交复用的资源级 DSP 弱缓存项。
    struct CachedTimelineResourceAudio {
        /// @brief 生成 PCM 时对应的原始音轨。
        std::weak_ptr<ice::AudioTrack> sourceTrack;

        /// @brief 已完成资源级 DSP 的只读 PCM。
        std::weak_ptr<const PreparedTimelineAudio> preparedAudio;
    };

    /// @brief 按文件和资源 DSP 配置索引的弱缓存。
    ///
    /// 弱引用避免时间线卸载后常驻大块 PCM；提交新时间线时，旧节点仍保持强
    /// 引用，因此连续编辑可以直接复用未变化资源。
    std::unordered_map<std::string, CachedTimelineResourceAudio>
        m_audioTimelineResourceCache;

    /// @brief 当前主音轨图形均衡器节点。
    std::shared_ptr<ice::GraphicEqualizer> m_mainEQ;

    /// @brief 当前主音轨 EQ 预设。
    EQPreset m_mainEQPreset{ EQPreset::None };

    /// @brief 当前主音轨时间拉伸节点。
    std::shared_ptr<ice::TimeStretcher> m_stretcher;

    /// @brief 当前 BGM 分支的实时频谱采样节点。
    std::shared_ptr<BackgroundSpectrumCaptureNode> m_bgmSpectrumCapture;

    /// @brief 当前独立试听音轨数据。
    std::shared_ptr<ice::AudioTrack> m_auditionTrack;

    /// @brief 当前独立试听音轨文件路径。
    std::string m_auditionPath;

    /// @brief 当前独立试听音轨文件的规范化绝对路径键。
    std::string m_auditionSyncKey;

    /// @brief 当前独立试听播放源节点。
    std::shared_ptr<ice::SourceNode> m_auditionSource;

    /// @brief 当前独立试听时间拉伸节点。
    std::shared_ptr<ice::TimeStretcher> m_auditionStretcher;

    /// @brief 主输出混音器。
    std::shared_ptr<ice::MixBus> m_mainMixer;

    /// @brief 变速器前级混音器，用于 BGM、EQ 和可选同步变速音效。
    std::shared_ptr<ice::MixBus> m_preStretcherMixer;

    /// @brief 仅汇总 hiteffect.* 音效的独立混音器。
    std::shared_ptr<ice::MixBus> m_hitEffectMixer;

    /// @brief HitEffect 总线的实时频谱采样节点。
    std::shared_ptr<BackgroundSpectrumCaptureNode> m_hitEffectSpectrumCapture;

    /// @brief 逻辑线程使用的固定容量实时频谱分析器。
    std::unique_ptr<BackgroundSpectrumAnalyzer> m_backgroundSpectrumAnalyzer;

    /// @brief 可按需解码的音效资源描述。
    struct RegisteredSoundEffect {
        /// @brief 音效文件绝对路径。
        std::string m_filePath;

        /// @brief 与自动采样共用的完整资源配置。
        AudioTrackConfig m_resourceConfig;

        /// @brief 首次加载使用的默认音量。
        float m_defaultVolume{ 1.0f };

        /// @brief 文件开头到有效出声点的延迟，单位为秒。
        double m_leadInSeconds{ 0.0 };

        /// @brief 是否由谱面物件绑定并需要接入打击音效总线。
        bool m_isBoundNoteSound{ false };

        /// @brief volume/mute 是否由项目 AudioResource 配置直接控制。
        bool m_usesProjectResourceConfig{ false };

        /// @brief 排除 volume/mute 的资源 DSP 缓存键。
        std::string m_processingCacheKey;

        /// @brief 本次登记的唯一修订号，用于丢弃过期后台加载结果。
        std::uint64_t m_revision{ 0U };
    };

    /// @brief 等待线程池探测的音效加载请求。
    struct QueuedSoundEffectLoad {
        /// @brief 音效资源标识。
        std::string key;

        /// @brief 登记时的音频文件绝对路径。
        std::string filePath;

        /// @brief 登记时固定的完整资源 DSP 配置。
        AudioTrackConfig resourceConfig;

        /// @brief 登记修订号。
        std::uint64_t revision{ 0U };
    };

    /// @brief 文件探测完成并等待逻辑线程接入混音图的音效。
    struct PreparedSoundEffectLoad {
        /// @brief 音效资源标识。
        std::string key;

        /// @brief 登记修订号。
        std::uint64_t revision{ 0U };

        /// @brief 探测成功后创建的异步解码音轨。
        std::shared_ptr<ice::AudioTrack> track;

        /// @brief 后台线程完成资源 DSP 后生成的只读 PCM。
        std::shared_ptr<const PreparedTimelineAudio> preparedAudio;
    };

    /// @brief 已登记音效描述表；登记本身不创建解码数据或混音节点。
    std::unordered_map<std::string, RegisteredSoundEffect>
        m_registeredSoundEffects;

    /// @brief 等待提交到后台线程池的物件绑定音效请求。
    std::deque<QueuedSoundEffectLoad> m_queuedSoundEffectLoads;

    /// @brief 已排队或正在探测的音效及其登记修订号。
    std::unordered_map<std::string, std::uint64_t> m_pendingSoundEffectLoads;

    /// @brief 后台探测任务产生的待接入结果。
    std::deque<PreparedSoundEffectLoad> m_preparedSoundEffectLoads;

    /// @brief 保护后台任务写入待接入结果队列。
    std::mutex m_preparedSoundEffectLoadsMutex;

    /// @brief 后台文件探测任务句柄；关闭音频系统前必须全部等待完成。
    std::vector<std::future<void>> m_soundEffectLoadTasks;

    /// @brief 当前正在后台探测文件的任务数量。
    std::size_t m_activeSoundEffectLoadCount{ 0U };

    /// @brief 下一个音效登记修订号。
    std::uint64_t m_nextSoundEffectRevision{ 1U };

    /// @brief 已加载的音效池表。
    std::unordered_map<std::string, std::shared_ptr<SoundEffectPool>>
        m_sfxPools;

    /// @brief 音效文件开头到有效出声点的延迟表，单位为秒。
    std::unordered_map<std::string, double> m_sfxLeadInSeconds;

    /// @brief 当前主音轨音量。
    float m_mainTrackVolume{ 1.0f };

    /// @brief 当前主音轨是否静音。
    bool m_mainTrackMuted{ false };

    /// @brief 当前独立试听音轨音量。
    float m_auditionTrackVolume{ 1.0f };

    /// @brief 当前独立试听音轨是否静音。
    bool m_auditionTrackMuted{ false };

    /// @brief 当前独立试听通道播放状态。
    PlaybackStatus m_auditionStatus{ PlaybackStatus::Stopped };

    /// @brief 当前全局音量。
    float m_globalVolume{ 1.0f };

    /// @brief 当前全局是否静音。
    bool m_globalMuted{ false };

    /// @brief 当前 BGM 全局增益。
    float m_bgmGain{ 1.0f };

    /// @brief 当前 BGM 增益是否静音。
    bool m_bgmGainMuted{ false };

    /// @brief 当前 SFX 全局增益。
    float m_sfxGain{ 1.0f };

    /// @brief 当前 SFX 增益是否静音。
    bool m_sfxGainMuted{ false };

    /// @brief 当前交互音效全局增益。
    float m_interactionSfxGain{ 1.0f };

    /// @brief 当前交互音效增益是否静音。
    bool m_interactionSfxGainMuted{ false };

    /// @brief 当前请求的播放倍率。
    double m_speed{ 1.0 };

    /// @brief 当前复合时间线全局预览音高。
    double m_playbackPitch{ 0.0 };

    /// @brief 当前复合时间线全局拉伸质量。
    StretchQuality m_playbackQuality{ StretchQuality::Finer };

    /// @brief 当前独立试听通道请求的播放倍率。
    double m_auditionSpeed{ 1.0 };

    /// @brief 音效池静音状态表。
    std::unordered_map<std::string, bool> m_sfxMutes;
};

}  // namespace MMM::Audio
