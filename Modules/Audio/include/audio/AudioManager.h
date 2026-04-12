#pragma once

#include "mmm/project/AudioResource.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace ice
{
class AudioPool;
class SDLPlayer;
class SourceNode;
class ThreadPool;
class MixBus;
class TimeStretcher;
}  // namespace ice

namespace MMM::Audio
{

class SoundEffectPool;

/**
 * @brief 播放状态
 */
enum class PlaybackStatus { Stopped, Playing, Paused };

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

    /// @brief 加载 BGM
    /// @param filePath 音频文件绝对路径
    /// @param config 轨道详细配置 (从项目文件读取)
    /// @return 是否加载成功
    bool loadBGM(const std::string& filePath, const AudioTrackConfig& config);

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

    /// @brief 设置特定 SFX 池的音量
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

    /// @brief 实时更新 SFX 路由策略 (决定音效是否跟随主音轨拉伸器)
    void updateSFXSyncSpeedRouting(bool syncSpeed);

    /// @brief 获取特定 SFX 池最近一次播放进度
    double getSFXPlaybackTime(const std::string& key) const;

    /// @brief 预加载音效文件
    /// @param key 标识符（如 "hiteffect.note"）
    /// @param filePath 音效文件绝对路径
    /// @param defaultVolume 初始默认音量
    /// @return 是否加载成功
    bool preloadSoundEffect(const std::string& key, const std::string& filePath,
                            float defaultVolume = 1.0f);

    /// @brief 播放指定 key 的音效
    /// @param key 标识符
    /// @param volumeFactor 额外音量倍率 (默认 1.0)
    void playSoundEffect(const std::string& key, float volumeFactor = 1.0f);

    /// @brief 在指定时间播放音效（预测系统使用）
    /// @param key 标识符
    /// @param targetTime 目标播放时间 (秒)
    /// @param volumeFactor 额外音量倍率
    void playSoundEffectScheduled(const std::string& key, double targetTime,
                                  float volumeFactor = 1.0f);

private:
    AudioManager();
    ~AudioManager();

    std::unique_ptr<ice::ThreadPool> m_threadPool;
    std::unique_ptr<ice::AudioPool>  m_audioPool;
    std::unique_ptr<ice::SDLPlayer>  m_player;

    std::shared_ptr<ice::SourceNode>    m_bgmSource;
    std::shared_ptr<ice::TimeStretcher> m_stretcher;
    std::shared_ptr<ice::MixBus>        m_mainMixer;
    std::shared_ptr<ice::MixBus>        m_preStretcherMixer;

    std::unordered_map<std::string, std::shared_ptr<SoundEffectPool>>
        m_sfxPools;

    PlaybackStatus m_status{ PlaybackStatus::Stopped };
    float          m_mainTrackVolume{ 0.5f };
    bool           m_mainTrackMuted{ false };
    float          m_globalVolume{ 1.0f };
    double         m_speed{ 1.0 };

    std::unordered_map<std::string, bool> m_sfxMutes;
};

}  // namespace MMM::Audio
