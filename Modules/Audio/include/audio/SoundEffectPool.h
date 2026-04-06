#pragma once

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
    /// @param mixer 混合器，音效节点将加入其中
    SoundEffectPool(std::shared_ptr<ice::AudioTrack> track,
                    std::shared_ptr<ice::MixBus>     mixer);

    ~SoundEffectPool();

    /// @brief 预分配节点 (必须在构造后调用一次)
    /// @param count 初始数量
    void init(int count = 8);

    /// @brief 播放一次音效
    /// @param volume 播放音量
    void play(float volume);

    /// @brief 释放节点回池 (供回调内部调用)
    void releaseNode(std::shared_ptr<ice::SourceNode> node);

private:
    std::shared_ptr<ice::AudioTrack> m_track;
    std::shared_ptr<ice::MixBus>     m_mixer;

    std::queue<std::shared_ptr<ice::SourceNode>>  m_readyQueue;
    std::vector<std::shared_ptr<ice::SourceNode>> m_allNodes;
    std::mutex                                    m_mtx;

    class SFXPlayCallback;
};

}  // namespace MMM::Audio
