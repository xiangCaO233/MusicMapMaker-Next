#pragma once

#include "config/EditorConfig.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "logic/SyncClock.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/session/EditorAction.h"
#include "mmm/beatmap/BeatMap.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Logic
{

/// @brief 相机/视口信息
struct CameraInfo {
    std::string id;                        ///< 视口唯一标识符
    float       viewportWidth{ 800.0f };   ///< 视口宽度
    float       viewportHeight{ 600.0f };  ///< 视口高度
};

/// @brief 矩形框选区域
struct MarqueeBox {
    double      startTime{ 0.0 };    ///< 起始逻辑时间
    double      endTime{ 0.0 };      ///< 结束逻辑时间
    float       startTrack{ 0.0f };  ///< 起始轨道索引
    float       endTrack{ 0.0f };    ///< 结束轨道索引
    std::string cameraId;            ///< 所属视口 ID
};

/// @brief 剪贴板条目
struct ClipboardItem {
    NoteComponent note;  ///< 复制的音符组件数据
};

/// @brief 共享的上下文状态，记录了当前会话的所有运行时数据，供各个 Controller
/// 和 Tool 访问。
struct SessionContext {
    // --- 核心状态 ---
    entt::registry noteRegistry;      ///< 音符实体的 ECS 注册表
    entt::registry timelineRegistry;  ///< 时间轴事件(BPM等)的 ECS 注册表

    double  currentTime{ 0.0 };  ///< 当前逻辑播放时间 (秒)
    double  visualTime{ 0.0 };   ///< 当前平滑视觉渲染时间 (考虑偏移)
    bool    isPlaying{ false };  ///< 是否正在播放
    int32_t trackCount{ 12 };    ///< 当前轨道总数

    std::shared_ptr<MMM::BeatMap> currentBeatmap;  ///< 当前载入的谱面对象
    Config::EditorConfig          lastConfig;      ///< 最近一次同步的编辑器配置
    std::unordered_map<std::string, CameraInfo>
              cameras;               ///< 当前所有活跃视口的信息
    glm::vec2 bgSize{ 0.0f, 0.0f };  ///< 背景图原始尺寸

    // --- 音频与播放状态 ---
    double    lastAudioPos{ 0.0 };         ///< 最近一次音频同步包中的时间戳
    double    lastAudioSysTime{ 0.0 };     ///< 最近一次音频同步包时的系统时间
    double    smoothedAudioOffset{ 0.0 };  ///< 平滑后的系统时间与音频时间差
    bool      hasInitialAudioOffset{ false };  ///< 是否已初始化平滑偏移
    SyncClock syncClock;         ///< 用于平滑音频时间与逻辑时间的时钟
    double    syncTimer{ 0.0 };  ///< 音频强制同步计时器

    /// @brief 播放开始时的系统时钟 (steady_clock, 秒)
    double playStartSysTime{ 0.0 };
    /// @brief 播放开始时的视觉时间基准
    double playStartVisualTime{ 0.0 };

    std::vector<System::HitFXSystem::HitEvent>
           hitEvents;                 ///< 当前谱面所有的打击事件序列
    size_t nextHitIndex{ 0 };         ///< 下一个待触发的视觉打击事件索引
    size_t nextPredictHitIndex{ 0 };  ///< 下一个待触发的预读打击事件(音频)索引
    System::HitFXSystem hitFXSystem;  ///< 打击特效处理系统

    Event::ScopedSubscription<Event::AudioFinishedEvent>
        audioFinishedToken;  ///< 音频播放完成订阅令牌
    Event::ScopedSubscription<Event::AudioPositionEvent>
        audioPositionToken;  ///< 音频位置同步订阅令牌

    // --- 交互与工具状态 ---
    EditTool     currentTool{ EditTool::Move };  ///< 当前选中的编辑工具类型
    std::string  mouseCameraId;                  ///< 鼠标当前所在的视口 ID
    glm::vec2    lastMousePos{ 0.0f, 0.0f };     ///< 鼠标在对应视口内的最后坐标
    entt::entity hoveredEntity{ entt::null };    ///< 当前悬停的实体 ID
    int32_t      hoveredPart{ 0 };               ///< 悬停的音符部位 (HoverPart)
    int32_t      hoveredSubIndex{ -1 };          ///< 悬停的子索引 (针对折线)
    bool         isMouseInCanvas{ false };       ///< 鼠标是否在任何画布内
    bool         isDragging{ false };            ///< 是否正在执行拖拽操作
    double       previewHoverTime{ 0.0 };        ///< 预览区当前悬停的时间点
    double       previewEdgeScrollVelocity{ 0.0 };  ///< 预览区边缘滚动速度

    entt::entity draggedEntity{ entt::null };     ///< 正在拖拽的实体 ID
    HoverPart    draggedPart{ HoverPart::None };  ///< 正在拖拽的音符部位
    int          draggedSubIndex{ -1 };           ///< 正在拖拽的子索引
    std::optional<NoteComponent>
        dragInitialNote;  ///< 拖拽开始时的初始音符数据 (用于取消或增量计算)
    std::string dragCameraId;  ///< 发起拖拽的视口 ID

    bool isSelecting{ false };             ///< 是否正在进行框选操作
    bool hasMarqueeSelection{ false };     ///< 是否当前存在有效的框选结果
    bool marqueeIsAdditive{ false };       ///< 框选是否为加选模式 (Ctrl)
    std::vector<MarqueeBox> marqueeBoxes;  ///< 当前活跃的框选框列表

    // --- 笔刷工具状态 ---
    struct BrushState {
        bool   isActive{ false };
        double time{ 0.0 };  ///< 当前选中的位置(对 Hold/Flick 为起始点)
        double holdStartTime{ 0.0 };  ///< 记录按下 Shift 瞬间的时间点
        double duration{ 0.0 };       ///< Hold 持续时间
        int    track{ 0 };            ///< 当前轨道 (对 Flick 为起始轨道)
        int    startTrack{ 0 };       ///< Flick 起始轨道
        int    dtrack{ 0 };           ///< Flick 偏移轨道
        float  startMouseY{ 0.0f };   ///< 按下 Shift 瞬间的鼠标 Y 坐标 (像素)
        float  segmentStartMouseY{ 0.0f };  ///< 当前子段开始时的鼠标 Y 坐标
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };

        // Polyline 相关的实时构建链
        std::vector<NoteComponent::SubNote> polylineSegments;
    } brushState;

    // --- 橡皮擦工具状态 ---
    struct EraserState {
        bool                             isActive{ false };
        std::unordered_set<entt::entity> targetEntities;
    } eraserState;

    // --- 编辑操作栈 ---
    EditorActionStack          actionStack;  ///< 撤销/重做操作栈
    std::vector<ClipboardItem> clipboard;    ///< 编辑器剪贴板
};

}  // namespace MMM::Logic
