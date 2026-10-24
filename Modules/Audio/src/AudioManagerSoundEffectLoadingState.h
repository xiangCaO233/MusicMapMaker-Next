#pragma once

#include "audio/AudioManager.h"
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

namespace MMM::Audio
{
/// @brief 可按需解码的音效资源描述。
struct AudioManager::RegisteredSoundEffect {
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
struct AudioManager::QueuedSoundEffectLoad {
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
struct AudioManager::PreparedSoundEffectLoad {
    /// @brief 音效资源标识。
    std::string key;

    /// @brief 登记修订号。
    std::uint64_t revision{ 0U };

    /// @brief 探测成功后创建的异步解码音轨。
    std::shared_ptr<ice::AudioTrack> track;

    /// @brief 后台线程完成资源 DSP 后生成的只读 PCM。
    std::shared_ptr<const PreparedTimelineAudio> preparedAudio;
};

/// @brief 音效后台加载的稳定私有状态，仅由管理器实现文件包含。
/// @details 构造管理器时分配一次，init、shutdown 和皮肤重载不替换此对象。
/// 控制线程维护登记、令牌和任务；工作线程仅在结果锁内发布准备结果。
/// @warning 关闭时必须先在不持有结果锁的情况下等待任务，再销毁资源池。
/// 成员顺序沿用原布局的声明顺序，任务句柄先于结果锁和结果队列销毁。
struct AudioManager::SoundEffectLoadingState {
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
};
}  // namespace MMM::Audio
