#pragma once

#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "logic/SyncClock.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "mmm/note/Note.h"
#include "mmm/timing/Timing.h"
#include <concurrentqueue.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
     * @param config 当前编辑器配置
     */
    void update(double dt, const Config::EditorConfig& config);

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
     * @param config 当前编辑器配置
     */
    void updateECSAndRender(const Config::EditorConfig& config);

    /**
     * @brief 根据当前视觉时间同步打击事件索引
     * 通常在 Seek 或开始播放时调用。
     */
    void syncHitIndex();

    /// @brief ECS 注册表：音符
    entt::registry m_noteRegistry;

    /// @brief ECS 注册表：时间线 (包含各种变速、BPM 变化事件)
    entt::registry m_timelineRegistry;

    /// @brief 无锁指令队列，UI线程发，逻辑线程收
    moodycamel::ConcurrentQueue<LogicCommand> m_commandQueue;

    /// @brief 谱面当前播放时间 (秒)
    double m_currentTime{ 0.0 };

    /// @brief 谱面当前视觉渲染时间 (秒)
    double m_visualTime{ 0.0 };

    /// @brief 同步时钟
    SyncClock m_syncClock;

    /// @brief 同步计时器 (秒)
    double m_syncTimer{ 0.0 };

    /// @brief 谱面是否正在播放
    bool m_isPlaying{ false };

    /// @brief 当前激活的编辑工具
    EditTool m_currentTool{ EditTool::Move };

    /// @brief 当前鼠标在视口中的状态
    std::string m_mouseCameraId;
    float       m_mouseX{ 0.0f };
    float       m_mouseY{ 0.0f };
    bool        m_isMouseInCanvas{ false };

    /// @brief 当前轨道数
    int32_t m_trackCount{ 12 };

    /// @brief 当前加载的谱面
    std::shared_ptr<MMM::BeatMap> m_currentBeatmap;

    /// @brief 当前编辑器配置缓存
    Config::EditorConfig m_lastConfig;

    /// @brief 所有注册的摄像机(视口)信息
    std::unordered_map<std::string, CameraInfo> m_cameras;

    /// @brief 当前正在拖拽的实体
    entt::entity m_draggedEntity{ entt::null };
    /// @brief 拖拽发起时的摄像机 ID
    std::string m_dragCameraId;

    /// @brief 背景图片的尺寸缓存
    glm::vec2 m_bgSize{ 0.0f, 0.0f };

    /// @brief 预排序的所有音符时间事件
    std::vector<System::HitFXSystem::HitEvent> m_hitEvents;

    /// @brief 下一个等待触发的音频事件索引
    size_t m_nextHitIndex{ 0 };

    /// @brief 打击音效与视觉特效系统
    System::HitFXSystem m_hitFXSystem;
};

}  // namespace MMM::Logic
