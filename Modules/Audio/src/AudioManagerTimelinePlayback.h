#pragma once

#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"

#include <mutex>
#include <unordered_map>

namespace MMM::Audio
{
/// @brief 管理预备片段、提交状态和复用 PCM，渲染节点继续由音频图保活。
/// @details 控制侧持有可修改的调度数据。回调仅操作已存在的节点和其发布的
/// 不可变调度，不在这里分配或等待。
struct AudioManager::TimelinePlayback {
    /// @brief 解码唯一文件并生成可提交片段，不修改活动混音图。
    /// @warning 文件工作线程可执行。缓存未命中时允许等待解码及离线处理。
    std::shared_ptr<PreparedAudioTimeline> prepare(
        const std::vector<AudioTimelineLoadEvent>& events, ice::AudioPool* pool,
        ice::ThreadPool*                              threadPool,
        const std::atomic<Config::AudioDecodingMode>& decodingMode);

    /// @brief 将预备片段安装为下一代调度，并同步管理器的增益和试听路由。
    /// @warning 逻辑控制线程调用，不等待设备回调。
    AudioTimelineLoadResult install(
        AudioManager& manager, PreparedAudioTimeline& prepared,
        double chartEndSeconds, const std::string& fingerprint,
        const std::vector<AudioTimelineStop>& stops);

    /// @brief 停止播放并发布空调度。
    /// @warning 仅在低频卸载且 waitForRetirement 为真时有界等待设备回调。
    void unload(AudioManager& manager, bool waitForRetirement);

    /// @brief 查找或建立自动采样与 HitEffect 共用的资源 DSP PCM。
    /// @param filePath 音频文件绝对路径。
    /// @param track 已完成解码的原始音轨。
    /// @param resourceConfig 资源 DSP 配置。
    /// @param preparedCandidate 后台线程可预先提供的处理结果。
    /// @return 缓存命中或处理成功时返回共享只读 PCM。
    /// @warning 低频控制路径：缓存未命中且无候选时会执行完整离线 DSP。
    std::shared_ptr<const PreparedTimelineAudio> getOrPrepareResource(
        const std::string&                           filePath,
        const std::shared_ptr<ice::AudioTrack>&      track,
        const AudioTrackConfig&                      resourceConfig,
        std::shared_ptr<const PreparedTimelineAudio> preparedCandidate = {});

    /// @brief 取得已发布的资源级 PCM；锁外使用结果。
    [[nodiscard]] std::shared_ptr<const PreparedTimelineAudio>
    findPreparedResource(const std::string& cacheKey) const;

    /// @brief 发布分析资源并增加代次，旧资源句柄在锁外释放。
    void publishAnalysisSource(std::shared_ptr<ice::AudioTrack> track,
                               double                           duration);
    /// @brief 复制当前分析快照，不访问设备或解码器。
    [[nodiscard]] BgmAnalysisSource analysisSource() const;

    /// @brief 清理已经过期的 DSP 弱引用。
    void pruneResourceCache();

    /// @brief 已停止生产者以后清空缓存，用于音频系统关闭。
    void clearResourceCache();

    /// @brief 当前加载的主音轨文件路径。
    std::string m_singleFilePath;

    /// @brief 当前主音轨文件的规范化绝对路径键。
    std::string m_syncKey;

    /// @brief 当前自动采样时间线的唯一传输和混音节点。
    std::shared_ptr<AudioTimelineMixerNode> m_node;

    /// @brief 稳定输出节点中是否已经提交一份谱面时间线。
    bool m_loaded{ false };

    /// @brief 当前完整自动采样时间线的稳定指纹。
    std::string m_fingerprint;

    /// @brief 当前有效自动采样片段数量。
    std::size_t m_clipCount{ 0U };

    /// @brief 未应用运行时逐轨静音的当前自动采样片段表。
    std::vector<PreparedTimelineClip> m_baseClips;
    /// @brief 当前谱面含暂停事件时强制 Key 音走统一拉伸前级。
    /// @warning 只由逻辑控制线程读写，音频线程消费独立不可变调度。
    bool m_hasStops{ false };

    /// @brief 当前自动采样时间线由谱面内容要求的排除结束帧。
    std::int64_t m_requestedEndFrame{ 0 };

    /// @brief 当前自动采样时间线每个回调 block 的最大帧数。
    std::size_t m_maximumProcessFrames{ 1U };

    /// @brief 上次加载时缺失的自动采样片段数量。
    std::size_t m_missingClipCount{ 0U };

private:
    /// @brief 保护分析来源交换，不用于加载、设备访问或实时回调。
    mutable std::mutex m_bgmAnalysisMutex;
    /// @brief 独立发布的可视化音轨、时长和代次。
    BgmAnalysisSource m_bgmAnalysisSource;

    /// @brief 跨相邻时间线提交复用的资源级 DSP 弱缓存项。
    struct CachedTimelineResourceAudio {
        /// @brief 生成 PCM 时对应的原始音轨。
        std::weak_ptr<ice::AudioTrack> sourceTrack;

        /// @brief 已完成资源级 DSP 的只读 PCM。
        std::weak_ptr<const PreparedTimelineAudio> preparedAudio;
    };

    /// @brief 短时保护后台准备与控制侧查询共用的弱缓存，锁内禁止执行 DSP。
    mutable std::mutex m_resourceCacheMutex;

    /// @brief 按文件和资源 DSP 配置索引的弱缓存。
    ///
    /// 弱引用避免时间线卸载后常驻大块 PCM；提交新时间线时，旧节点仍保持强
    /// 引用，因此连续编辑可以直接复用未变化资源。
    std::unordered_map<std::string, CachedTimelineResourceAudio>
        m_resourceCache;
};
}  // namespace MMM::Audio
