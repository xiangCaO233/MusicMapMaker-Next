#pragma once

#include "common/EditTool.h"
#include "common/NoteColor.h"
#include "config/EditorConfig.h"
#include "config/FrameLimitPreference.h"
#include "logic/EditorClipboard.h"
#include "logic/EditorEngine.h"
#include "logic/RenderSyncRegistry.h"
#include "logic/SessionRegistry.h"
#include "mmm/project/AudioTrackType.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <glm/vec4.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{
/// @brief EditorEngine 唯一拥有的长期私有状态，禁止暴露给 UI 或画布。
/// @details 只移动成员的存放位置，保留原初始化次序、锁及原子内存序。
/// @warning 逻辑热路径借用此稳定实例；仅在引擎构造时分配，stop 后才销毁。
struct EditorEngine::Impl {
    /// @brief 逻辑循环在线程池中的任务句柄。
    /// @warning 生命周期路径：仅在 start/stop
    /// 低频路径写入；逻辑循环本身长期占用一个共享线程池工作线程。
    std::future<void> m_loopFuture;

    /// @brief 线程运行标志
    /// @warning 逻辑热路径/原子：loop 每次迭代读取、stop
    /// 写入；需要跨线程停止信号，使用 acquire/release。
    std::atomic<bool> m_running{ false };

    /// @brief 多画布会话注册表，封装 Session 列表、活跃索引和 cameraId 分配。
    SessionRegistry m_sessionRegistry;

    /// @brief 保护编辑器配置完整对象的读写，防止 UI
    /// 拖拽更新与逻辑线程复制并发。
    mutable std::mutex m_editorConfigMutex;

    /// @brief 串行化完整的配置提交流程，避免不同事件线程并发改写
    /// AppConfig 中含动态容器的配置对象。
    std::recursive_mutex m_editorConfigUpdateMutex;

    /// @brief 编辑器配置，由 m_editorConfigMutex 保护。
    Config::EditorConfig m_editorConfig;

    /// @brief 当前项目对软件级谱面自动备份配置的可选覆盖。
    /// @warning UI/逻辑线程低频读写，由 m_editorConfigMutex
    /// 与配置修订共同保护。
    std::optional<Config::AutoBackupConfig> m_projectAutoBackupOverride;

    /// @brief 当前编辑器配置修订号。
    /// @warning 逻辑/UI 热路径原子：UI 配置提交或相机交互时 release
    /// 递增，逻辑 loop 每轮 acquire
    /// 读取；用于避免未变化时复制含容器的完整配置。
    std::atomic<std::uint64_t> m_editorConfigRevision{ 0 };

    /// @brief 当前全局编辑工具。
    /// @warning 逻辑/UI 热路径/原子：UI 命令写入、会话 update
    /// 读取；只传递枚举状态，使用 relaxed。
    std::atomic<EditTool> m_currentTool{ EditTool::Move };

    /// @brief 编辑器级画笔配色，各会话创建或重新激活时从此状态恢复。
    std::array<std::optional<glm::vec4>, NOTE_COLOR_SLOT_COUNT>
        m_brushNoteColors{};
    /// @brief 各槽是否由单槽显式染色命令设置，不能从 RGBA 数值反推。
    std::array<bool, NOTE_COLOR_SLOT_COUNT> m_brushNoteColorsExplicit{};

    /// @brief 编辑器是否已经收到过画笔配色命令。
    bool m_brushNoteColorsInitialized{ false };

    /// @brief 当前项目音频工具选中的稳定资源 ID。
    std::string m_brushAudioResourceId;

    /// @brief 当前项目音频工具选中的资源类型。
    AudioTrackType m_brushAudioTrackType{ AudioTrackType::Effect };

    /// @brief 当前项目音频工具为新建物件设置的音量倍率。
    float m_brushAudioVolume{ 1.0F };

    /// @brief 逻辑线程用于节流判断的帧率限制模式缓存。
    /// @warning 逻辑热路径/原子：loop 每次迭代读取；只缓存配置枚举，使用
    /// relaxed 降低栅栏成本。
    std::atomic<Config::FrameLimitPreference> m_frameLimitPreference{
        Config::FrameLimitPreference::Refresh2x
    };

    /// @brief 渲染同步注册表，封装同步缓冲区、图集 UV 映射和视口尺寸缓存。
    RenderSyncRegistry m_renderSyncRegistry;

    /// @brief 非活跃 Session 最近一次生成渲染快照的时间点，按 Session
    /// 索引存储。
    /// @warning 逻辑热路径：每 update
    /// 读取和低频写入；用于限制后台画布快照频率。
    std::vector<std::chrono::steady_clock::time_point>
        m_backgroundSessionUpdateTimes;

    /// @brief 编辑器级剪贴板组件，封装剪贴板内容、来源 Session 和剪切状态。
    EditorClipboard m_clipboard;


    /// @brief 活动会话实际播放状态的轻量发布值，后台 follower 不计入。
    /// @warning 逻辑线程每 update 写入、UI 每帧读取 relaxed；仅供工具栏
    /// 与播放切换命令判断，使用原子是为了避开会话更新期间的长锁等待。
    std::atomic<bool> m_activePlaybackPlaying{ false };

    /// @brief 活动会话的谱面存在状态，供逐帧菜单可用性判定。
    /// @warning 逻辑线程每 update 写入、UI 每帧读取 relaxed；避免菜单读取
    /// currentBeatmap 时等待逻辑线程整个 update 周期持有的注册表锁。
    std::atomic<bool> m_activeHasBeatmap{ false };

    /// @brief 活动会话是否选中了 Note 或 Sample，供菜单显示可用状态。
    /// @warning 逻辑线程每 update 写入、UI 每帧读取 relaxed；选择只在
    /// 会话命令消费后生效，原子值避免菜单检查等待逻辑更新的长锁。
    std::atomic<bool> m_activeHasChartObjectSelection{ false };




    /// @brief 当前是否存在至少两个使用同一主音轨的非 Logo Session。
    /// @warning 逻辑热路径/原子：同步入口每次读取；低频路径写入，只承载
    /// 同步候选脏状态，使用 relaxed。
    std::atomic<bool> m_hasMainAudioSyncPeers{ false };

    /// @brief 等待 UI 线程聚焦的 Session 索引。
    /// @warning 逻辑/UI 热路径原子：createSession 低频写入，CanvasTabManager
    /// 每帧消费。
    std::atomic<int32_t> m_pendingFocusSessionIndex{ -1 };

    /// @brief 项目工作区恢复后待激活的 Session 索引，仅由逻辑线程读写。
    int32_t m_pendingWorkspaceActiveIndex{ -1 };

    /// @brief 上次执行同主音轨同步时的活跃 Session 索引。
    int32_t m_lastMainAudioSyncActiveIndex{ -1 };

    /// @brief 上次执行同主音轨同步时的活跃 Session 逻辑时间。
    double m_lastMainAudioSyncTime{ 0.0 };

    /// @brief 逻辑线程更新计数器，用于 UPS 计算
    uint32_t m_logicUpdateCount{ 0 };

    /// @brief 上次统计 UPS 的单调时钟时间点。
    std::chrono::steady_clock::time_point m_lastUpsTime;
};
}  // namespace MMM::Logic
