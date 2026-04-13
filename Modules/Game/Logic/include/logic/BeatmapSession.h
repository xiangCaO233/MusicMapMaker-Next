#pragma once

#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "logic/SyncClock.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/session/EditorAction.h"
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
     * @brief 获取当前工具类型
     */
    EditTool getCurrentTool() const { return m_currentTool; }

    /**
     * @brief 获取操作栈
     */
    EditorActionStack& getActionStack() { return m_actionStack; }

    /**
     * @brief 获取相关注册表 (用于 Action 执行)
     */
    entt::registry& getNoteRegistry() { return m_noteRegistry; }
    entt::registry& getTimelineRegistry() { return m_timelineRegistry; }

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

    /**
     * @brief 暴露同步辅助方法给外部 Action
     */
    void syncHitIndex();
    void rebuildHitEvents();
    void syncBeatmap();

    struct SnapResult {
        bool   isSnapped{ false };
        double snappedTime{ 0.0 };
        int    numerator{ 0 };
        int    denominator{ 1 };
    };

private:
    /**
     * @brief 计算给定时间点在特定视口下的磁吸结果
     * @param rawTime 原始时间点
     * @param mouseY 鼠标当前的 Y 坐标（用于阈值判断）
     * @param camera 所在的视口信息
     * @param config 当前编辑器配置
     * @param bpmEvents 已排序的 BPM 事件列表
     * @return 磁吸结果
     */
    SnapResult getSnapResult(
        double rawTime, float mouseY, const CameraInfo& camera,
        const Config::EditorConfig&                  config,
        const std::vector<const TimelineComponent*>& bpmEvents) const;

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
     * @brief 更新框选状态 (仅在 m_isSelecting 为 true 时执行)
     */
    void updateMarqueeSelection();

    // --- 指令处理器 (Command Handlers) ---
    void handleCommand(const CmdUpdateEditorConfig& cmd);
    void handleCommand(const CmdUpdateViewport& cmd);
    void handleCommand(const CmdSetPlayState& cmd);
    void handleCommand(const CmdLoadBeatmap& cmd);
    void handleCommand(const CmdSetHoveredEntity& cmd);
    void handleCommand(const CmdSelectEntity& cmd);
    void handleCommand(const CmdStartDrag& cmd);
    void handleCommand(const CmdUpdateDrag& cmd);
    void handleCommand(const CmdEndDrag& cmd);
    void handleCommand(const CmdUpdateTrackCount& cmd);
    void handleCommand(const CmdSeek& cmd);
    void handleCommand(const CmdSetPlaybackSpeed& cmd);
    void handleCommand(const CmdChangeTool& cmd);
    void handleCommand(const CmdSetMousePosition& cmd);
    void handleCommand(const CmdScroll& cmd);
    void handleCommand(const CmdStartMarquee& cmd);
    void handleCommand(const CmdUpdateMarquee& cmd);
    void handleCommand(const CmdEndMarquee& cmd);
    void handleCommand(const CmdUndo& cmd);
    void handleCommand(const CmdRedo& cmd);
    void handleCommand(const CmdCopy& cmd);
    void handleCommand(const CmdCut& cmd);
    void handleCommand(const CmdPaste& cmd);
    void handleCommand(const CmdSaveBeatmap& cmd);
    void handleCommand(const CmdPackBeatmap& cmd);
    void handleCommand(const CmdUpdateTimelineEvent& cmd);
    void handleCommand(const CmdDeleteTimelineEvent& cmd);
    void handleCommand(const CmdCreateTimelineEvent& cmd);

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

    /// @brief 缓存的最新音频硬件位置 (秒)
    double m_lastAudioPos{ 0.0 };

    /// @brief 记录上次音频位置更新事件发生时的系统高精度时间 (秒)
    double m_lastAudioSysTime{ 0.0 };

    /// @brief 同步时钟
    SyncClock m_syncClock;

    /// @brief 同步计时器 (秒)
    double m_syncTimer{ 0.0 };

    /// @brief 谱面是否正在播放
    bool m_isPlaying{ false };

    /// @brief 当前激活的编辑工具
    EditTool m_currentTool{ EditTool::Move };

    /// @brief 当前鼠标在视口中的状态
    std::string  m_mouseCameraId;
    glm::vec2    m_lastMousePos{ 0.0f, 0.0f };
    entt::entity m_hoveredEntity{ entt::null };
    int32_t      m_hoveredPart{ 0 };
    int32_t      m_hoveredSubIndex{ -1 };
    bool         m_isMouseInCanvas{ false };
    bool         m_isDragging{ false };
    double       m_previewHoverTime{ 0.0 };
    double       m_previewEdgeScrollVelocity{ 0.0 };

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
    /// @brief 拖拽发起时的部位
    HoverPart m_draggedPart{ HoverPart::None };
    /// @brief 拖拽发起时的子索引
    int m_draggedSubIndex{ -1 };
    /// @brief 拖拽开始时的状态备份 (用于撤销)
    std::optional<NoteComponent> m_dragInitialNote;
    /// @brief 拖拽发起时的摄像机 ID
    std::string m_dragCameraId;

    /// @brief 框选状态
    bool        m_isSelecting{ false };
    bool        m_hasMarqueeSelection{ false };
    std::string m_selectionCameraId;
    double      m_selectionStartTime{ 0.0 };
    float       m_selectionStartTrack{ 0.0f };
    double      m_selectionEndTime{ 0.0 };
    float       m_selectionEndTrack{ 0.0f };

    /// @brief 剪切板内容
    struct ClipboardItem {
        NoteComponent note;
    };
    std::vector<ClipboardItem> m_clipboard;

    /// @brief 背景图片的尺寸缓存
    glm::vec2 m_bgSize{ 0.0f, 0.0f };

    /// @brief 预排序的所有音符时间事件
    std::vector<System::HitFXSystem::HitEvent> m_hitEvents;

    /// @brief 下一个等待触发的视觉事件索引
    size_t m_nextHitIndex{ 0 };

    /// @brief 下一个等待触发的音频预测事件索引
    size_t m_nextPredictHitIndex{ 0 };

    /// @brief 打击音效与视觉特效系统
    System::HitFXSystem m_hitFXSystem;

    Event::ScopedSubscription<Event::AudioFinishedEvent> m_audioFinishedToken;
    Event::ScopedSubscription<Event::AudioPositionEvent> m_audioPositionToken;

    /// @brief 操作栈管理器
    EditorActionStack m_actionStack;
};

}  // namespace MMM::Logic
