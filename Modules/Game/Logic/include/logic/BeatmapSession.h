#pragma once

#include "logic/LogicCommands.h"
#include <concurrentqueue.h>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>

// 前置声明
namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

struct CameraInfo {
    std::string id;
    float       viewportWidth{ 800.0f };
    float       viewportHeight{ 600.0f };
};
class BeatMap;
}  // namespace MMM::Logic

namespace MMM::Logic
{

/**
 * @brief 谱面逻辑会话核心
 *
 * 持有 ECS 注册表，处理逻辑线程中的业务更新，接收 UI 发送的指令队列，
 * 并将结果输出到同步缓冲区。一个 Session 对应一个正在编辑/游玩的谱面实例。
 */
class BeatmapSession
{
public:
    BeatmapSession();
    ~BeatmapSession() = default;

    // 禁用拷贝与移动
    BeatmapSession(BeatmapSession&&)                 = delete;
    BeatmapSession(const BeatmapSession&)            = delete;
    BeatmapSession& operator=(BeatmapSession&&)      = delete;
    BeatmapSession& operator=(const BeatmapSession&) = delete;

    /**
     * @brief 推送指令到无锁队列（由 UI 线程调用）
     */
    void pushCommand(LogicCommand&& cmd);

    /**
     * @brief 每帧更新（由 Logic 线程调用）
     * @param dt 距离上一帧的时间间隔（秒）
     */
    void update(double dt);

private:
    /**
     * @brief 加载新谱面数据到 ECS 中
     */
    void loadBeatmap(std::shared_ptr<MMM::BeatMap> beatmap);

    /**
     * @brief 处理队列中堆积的所有指令
     */
    void processCommands();

    /**
     * @brief 执行逻辑计算和生成渲染快照
     */
    void updateECSAndRender();

    /// @brief ECS 注册表：音符
    entt::registry m_noteRegistry;

    /// @brief ECS 注册表：时间线 (包含各种变速、BPM 变化事件)
    entt::registry m_timelineRegistry;

    /// @brief 无锁指令队列，UI线程发，逻辑线程收
    moodycamel::ConcurrentQueue<LogicCommand> m_commandQueue;

    /// @brief 谱面当前播放时间 (秒)
    double m_currentTime{ 0.0 };

    /// @brief 谱面是否正在播放
    bool m_isPlaying{ false };

    /// @brief 所有注册的摄像机(视口)信息
    std::unordered_map<std::string, CameraInfo> m_cameras;

    /// @brief 当前正在拖拽的实体
    entt::entity m_draggedEntity{ entt::null };
    /// @brief 拖拽发起时的摄像机 ID
    std::string m_dragCameraId;
};

}  // namespace MMM::Logic
