#pragma once

#include "common/EditTool.h"
#include "common/EditorTrackLayout.h"
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
#include <thread>
#include <vector>

namespace MMM::Logic
{
/// @brief EditorEngine 唯一拥有的长期私有状态，禁止暴露给 UI 或画布。
/// @details 只移动成员的存放位置，保留原初始化次序、锁及原子内存序。
/// @warning 逻辑热路径借用此稳定实例；仅在引擎构造时分配，stop 后才销毁。
struct EditorEngine::Impl {
    /// @brief 独立于应用共享线程池的逻辑线程。
    /// @warning 生命周期路径：仅在 start/stop
    /// 低频路径写入；停止请求仍由 m_running 发布。
    std::thread m_loopThread;

    /// @brief 线程运行标志
    /// @warning 逻辑热路径/原子：loop 每次迭代读取、stop
    /// 写入；需要跨线程停止信号，使用 acquire/release。
    std::atomic<bool> m_running{ false };

    /// @brief UI 写、逻辑读的画布切换邮箱；禁止在锁内访问会话或音频。
    std::mutex m_activeCanvasRequestMutex;
    /// @brief 最近的焦点目标，反复请求同一相机不会重新发起准备。
    std::string m_requestedActiveCamera;
    /// @brief 每次目标变化时由 UI 发布，逻辑每轮 relaxed 读取后在邮箱锁内取值。
    /// @warning 热路径只读序号，目标字符串始终由邮箱锁保护。
    std::atomic<std::uint64_t> m_activeCanvasRequestRevision{ 0U };
    /// @brief 逻辑线程已处理的最新邮箱修订。
    std::uint64_t m_consumedActiveCanvasRevision{ 0U };
    /// @brief 仅逻辑线程保存当前准备任务；停止逻辑线程后先回收再关闭音频。
    std::future<std::shared_ptr<Audio::PreparedAudioTimeline>>
        m_canvasAudioPreparation;
    /// @brief 准备任务使用的稳定相机身份。
    std::string m_preparingCamera;
    /// @brief 准备任务使用的时间线指纹。
    std::string m_preparingTimelineFingerprint;
    /// @brief 将同一相机的 A→B→A 识别为新事务。
    std::uint64_t m_preparingCanvasRevision{ 0U };
    /// @brief 每次活动目标变化时失效；仅在会话锁下递增。
    std::uint64_t m_canvasAudioGeneration{ 0U };
    /// @brief 仅当活动画布需要音频时才轮询会话锁和后台就绪状态。
    bool m_hasPendingCanvasAudio{ false };

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
    std::atomic<EditTool> m_currentTool{ EditTool::Marquee };

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

    /// @brief 活动谱面的玩家轨道数，零同时表示活动会话未载谱。
    /// @warning 逻辑线程每 update 及会话切换时写入，UI 每帧 relaxed 读取；
    /// 菜单和工具栏只需标量，不能为此等待会话注册表长锁。
    std::atomic<int32_t> m_activeBeatmapTrackCount{ 0 };

    /// @brief 独立保护活动轨道布局的短锁，锁内仅允许读写值。
    /// @warning UI 弹层逐帧读取；只在布局变化时写入，锁内不获取会话锁。
    mutable std::mutex m_activeTrackLayoutMutex;
    /// @brief 同一代发布的完整轨道布局，避免三个范围分开读取时跨谱面混合。
    Common::EditorTrackLayout m_activeTrackLayout;

    /// @brief 活动会话是否选中了 Note 或 Sample，供菜单显示可用状态。
    /// @warning 逻辑线程每 update 写入、UI 每帧读取 relaxed；选择只在
    /// 会话命令消费后生效，原子值避免菜单检查等待逻辑更新的长锁。
    std::atomic<bool> m_activeHasChartObjectSelection{ false };

    /// @brief 当前活动会话的框选、拖动和画笔状态，同一字节一次性发布。
    /// @warning 会话更新与切换持同一会话锁写入，UI 热路径 relaxed 读取。
    /// 该值只用于快捷键路由，原子读取避免等待会话整轮更新，不发布其他对象。
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    std::atomic<std::uint8_t> m_activeGestureBits{ 0U };




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
