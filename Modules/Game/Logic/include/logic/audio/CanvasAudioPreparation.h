#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace ice
{
class ThreadPool;
}

namespace MMM
{
class Project;
}

namespace MMM::Audio
{
struct PreparedAudioTimeline;
}

namespace MMM::Logic
{
struct SessionContext;

/// @brief 管理活动画布的异步时间线准备与提交。
/// @details
/// 后台仅持有描述事件的副本。活动会话、播放意图和全局音频图由逻辑侧管理。
/// @note
/// 拥有者在会话锁内改变目标、申请准备和提交结果。只有逻辑线程可访问任务槽；
/// 退出时在逻辑线程结束后移交回收责任，音频服务和线程池需保留到回收完成。
class CanvasAudioPreparation
{
public:
    /// @brief 使旧任务失效，并撤回离开画布的等待及续播标记。
    /// @param previous 原活动会话，调用方可以先取出需要继承的播放意图。
    /// @warning 会话身份变更时调用；不等待 worker，不访问其 future。
    void invalidate(SessionContext* previous = nullptr);

    /// @brief 为当前活动画布登记冷准备，同时保留其编辑和定位范围。
    /// @param context 当前活动会话。
    /// @param shouldResume 初始续播意图，准备期间允许被播放命令覆盖。
    void defer(SessionContext& context, bool shouldResume);

    /// @brief 当前有待启动的准备，或已有可回收结果。
    /// @warning 每轮逻辑调用。只查询逻辑线程持有的标记与零时长的就绪状态。
    [[nodiscard]] bool needsAdvance() const;

    /// @brief 推进当前目标的任务，或提交通过复核的结果。
    /// @param context 当前活动会话，无有效目标时传空。
    /// @param project 解析本地资源的项目，协作会话优先使用自身项目。
    /// @param pool 允许提交后台准备的共享池，空指针时结束等待。
    /// @return 已更新活动会话的实际播放状态时返回 true。
    /// @warning 逻辑轮次仅在任务进入或退出时改变状态，不等待任务或同步解码。
    bool advance(SessionContext* context, const Project* project,
                 ice::ThreadPool* pool);

    /// @brief 停止逻辑循环后回收后台任务。保留仍有效的准备意图供重新启动。
    /// @warning 退出路径可阻塞等待解码收尾，禁止在逻辑循环和持有会话锁时调用。
    void finishWorker();

private:
    /// @brief 单项任务与开始准备时的验证条件。
    struct Job {
        /// @brief 只由逻辑端取得结果，后台不写入会话或持有注册表。
        std::future<std::shared_ptr<Audio::PreparedAudioTimeline>> result;
        /// @brief 准备所属的活动目标代次。
        std::uint64_t generation{};
        /// @brief 同一活动目标内失效的依据，例如资源编辑或改名。
        std::string fingerprint;
    };

    /// @brief 当前最多一个进行中或待回收的后台任务。
    Job m_job;
    /// @brief 当前目标代次，由会话锁序列化，可来自同步切换入口。
    std::uint64_t m_generation{};
    /// @brief 最新需要冷准备的目标代次，只由逻辑线程更新。
    std::optional<std::uint64_t> m_pendingGeneration;
};
}  // namespace MMM::Logic
