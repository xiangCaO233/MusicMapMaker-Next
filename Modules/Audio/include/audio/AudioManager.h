#pragma once

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
    /// @return 是否加载成功
    bool loadBGM(const std::string& filePath);

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

    /// @brief 设置音量 (0.0 ~ 1.0)
    void setVolume(float volume);

    /// @brief 获取当前音量
    float getVolume() const;

    /// @brief 设置播放倍率 (0.5 ~ 2.0)
    void setPlaybackSpeed(double speed);

    /// @brief 获取当前播放倍率
    double getPlaybackSpeed() const;

    /// @brief 预加载音效文件
    /// @param key 标识符（如 "hiteffect.note"）
    /// @param filePath 音效文件绝对路径
    /// @return 是否加载成功
    bool preloadSoundEffect(const std::string& key,
                            const std::string& filePath);

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
    AudioManager() = default;
    ~AudioManager();

    std::unique_ptr<ice::ThreadPool> m_threadPool;
    std::unique_ptr<ice::AudioPool>  m_audioPool;
    std::unique_ptr<ice::SDLPlayer>  m_player;

    std::shared_ptr<ice::SourceNode>    m_bgmSource;
    std::shared_ptr<ice::TimeStretcher> m_stretcher;
    std::shared_ptr<ice::MixBus>        m_mixer;

    std::unordered_map<std::string, std::shared_ptr<SoundEffectPool>>
        m_sfxPools;

    PlaybackStatus m_status{ PlaybackStatus::Stopped };
    float          m_volume{ 0.5f };
    double         m_speed{ 1.0 };
};

}  // namespace MMM::Audio
