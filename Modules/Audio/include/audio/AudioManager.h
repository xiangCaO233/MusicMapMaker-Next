#pragma once

#include "config/EditorSettings.h"
#include "mmm/project/AudioResource.h"
#include <cstddef>
#include <memory>
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

/** @brief 主混音器双声道输出模式。 */
enum class MixerChannelMode {
    Stereo,           ///< 保持原始立体声输出。
    MuteLeft,         ///< 静音左声道，仅保留右声道。
    MuteRight,        ///< 静音右声道，仅保留左声道。
    CopyLeftToRight,  ///< 将左声道复制到右声道，两侧都播放左声道。
    CopyRightToLeft   ///< 将右声道复制到左声道，两侧都播放右声道。
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

    /// @brief 加载 BGM
    /// @param filePath 音频文件绝对路径
    /// @param config 轨道详细配置 (从项目文件读取)
    /// @return 是否加载成功
    bool loadBGM(const std::string& filePath, const AudioTrackConfig& config);

    /// @brief 卸载当前 BGM 轨道，并从混音图中断开。
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

    /// @brief 设置主音轨音量 (0.0 ~ 1.0)
    void setMainTrackVolume(float volume);

    /// @brief 获取主音轨当前音量
    float getMainTrackVolume() const;

    /// @brief 设置主音轨静音状态
    void setMainTrackMute(bool muted);

    /// @brief 获取主音轨是否静音
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

    /// @brief 设置主音轨音高偏移 (半音，-24.0 ~ +24.0)
    void setPlaybackPitch(double semitones);

    /// @brief 获取主音轨音高偏移
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

    /// @brief 为主音轨创建均衡器
    /// @param preset 预设类型
    void createMainTrackEQ(EQPreset preset);

    /// @brief 销毁主音轨均衡器
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

    /// @brief 实时更新打击音效路由策略。
    /// @param syncSpeed 是否让 hiteffect.* 音效跟随主音轨拉伸器。
    void updateSFXSyncSpeedRouting(bool syncSpeed);

    /// @brief 获取特定 SFX 池最近一次播放进度
    double getSFXPlaybackTime(const std::string& key) const;

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
    void playSoundEffectScheduled(const std::string& key, double targetTime,
                                  float volumeFactor = 1.0f);

    /// @brief 清空并停止所有正在播放和预定的音效
    void clearAllScheduledSoundEffects();

    /// @brief 获取当前加载的 BGM 轨道数据 (用于可视化)
    std::shared_ptr<ice::AudioTrack> getBGMTrack() const;

    /// @brief 获取当前加载的 BGM 文件路径。
    /// @return 当前 BGM 文件路径；未加载时返回空字符串。
    const std::string& getLoadedBGMPath() const;

    /// @brief 获取当前 BGM 文件的规范化绝对路径键。
    /// @return 与 Session 主音轨同步键格式一致的路径键；未加载时为空。
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

    /// @brief 刷新所有音效池当前播放节点的有效音量。
    void refreshSFXEffectiveVolumes();

    /// @brief 刷新独立试听源的有效音量。
    void refreshAuditionTrackVolume();

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

    /// @brief 当前主音轨播放源节点。
    std::shared_ptr<ice::SourceNode> m_bgmSource;

    /// @brief 当前主音轨图形均衡器节点。
    std::shared_ptr<ice::GraphicEqualizer> m_mainEQ;

    /// @brief 当前主音轨 EQ 预设。
    EQPreset m_mainEQPreset{ EQPreset::None };

    /// @brief 当前主音轨时间拉伸节点。
    std::shared_ptr<ice::TimeStretcher> m_stretcher;

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

    /// @brief 已加载的音效池表。
    std::unordered_map<std::string, std::shared_ptr<SoundEffectPool>>
        m_sfxPools;

    /// @brief 音效文件开头到有效出声点的延迟表，单位为秒。
    std::unordered_map<std::string, double> m_sfxLeadInSeconds;

    /// @brief 当前主音轨播放状态。
    PlaybackStatus m_status{ PlaybackStatus::Stopped };

    /// @brief 当前主音轨音量。
    float m_mainTrackVolume{ 0.5f };

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

    /// @brief 当前独立试听通道请求的播放倍率。
    double m_auditionSpeed{ 1.0 };

    /// @brief 音效池静音状态表。
    std::unordered_map<std::string, bool> m_sfxMutes;
};

}  // namespace MMM::Audio
