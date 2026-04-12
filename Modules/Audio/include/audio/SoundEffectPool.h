#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace ice
{
class AudioTrack;
class MixBus;
class SourceNode;
}  // namespace ice

namespace MMM::Audio
{

/**
 * @brief 音效池，用于管理同一音效的多个并发播放实例
 */
class SoundEffectPool : public std::enable_shared_from_this<SoundEffectPool>
{
public:
    /// @brief 构造音效池
    /// @param track 音轨资源
    SoundEffectPool(std::shared_ptr<ice::AudioTrack> track);

    ~SoundEffectPool();

    /// @brief 获取该音效池的局部混音器输出节点，供外部路由
    std::shared_ptr<ice::MixBus> getMixer() const;

    /// @brief 预分配节点 (必须在构造后调用一次)
    /// @param count 初始数量
    void init(int count = 8);

    /// @brief 播放一次音效
    /// @param volume 播放音量
    void play(float volume);

    /// @brief 在指定时间播放音效
    /// @param volume 播放音量
    /// @param targetFrame 目标帧位置（基于 BGM）
    /// @param refProvider 参考源位置提供者
    void playScheduled(float volume, size_t targetFrame,
                       std::function<size_t()> refProvider);

    /// @brief 释放节点回池 (供回调内部调用)
    void releaseNode(std::shared_ptr<ice::SourceNode> node);

    /// @brief 设置池内所有节点的音量 (由 AudioManager 调用，会考虑全局音量)
    void setVolume(float volume);

    /// @brief 更新当前所有播放节点的音量 (当全局音量变化或静音状态变化时)
    void updateEffectiveVolume(float globalVolume, bool muted);

    /// @brief 获取池内音量 (原始音量，不含全局增益)
    float getVolume() const { return m_volume; }

    /// @brief 获取音效总时长 (秒)
    double getDuration() const;

    /// @brief 获取最近一次播放的进度 (秒)
    double getLatestPlaybackTime() const;

private:
    std::shared_ptr<ice::AudioTrack> m_track;
    std::shared_ptr<ice::MixBus>     m_localMixer;

    std::queue<std::shared_ptr<ice::SourceNode>>  m_readyQueue;
    std::vector<std::shared_ptr<ice::SourceNode>> m_allNodes;
    std::shared_ptr<ice::SourceNode>              m_latestNode;
    mutable std::mutex                            m_mtx;

    float m_volume{ 1.0f };

    class SFXPlayCallback;
};

}  // namespace MMM::Audio
