#pragma once

#include "common/ChartObjectKind.h"
#include "config/EditorConfig.h"
#include "logic/PreviewDensity.h"
#include "logic/audio/AudioTimelineDescriptor.h"
#include "logic/audio/PlaybackVisualClock.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/session/ClipboardTypes.h"
#include "logic/session/EditorAction.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM
{
class BeatMap;
class Project;
}  // namespace MMM

namespace MMM::Logic
{

class BeatmapSyncBuffer;

/// @brief 相机/视口信息
struct CameraInfo {
    std::string id;                        ///< 视口唯一标识符
    float       viewportWidth{ 800.0f };   ///< 视口逻辑宽度
    float       viewportHeight{ 600.0f };  ///< 视口逻辑高度
    /// @brief 内容相对基础轨道布局的横向逻辑像素偏移。
    float horizontalOffsetX{ 0.0F };
};

/// @brief 矩形框选区域
struct MarqueeBox {
    double      startTime{ 0.0 };    ///< 起始逻辑时间
    double      endTime{ 0.0 };      ///< 结束逻辑时间
    float       startTrack{ 0.0f };  ///< 起始轨道索引
    float       endTrack{ 0.0f };    ///< 结束轨道索引
    std::string cameraId;            ///< 所属视口 ID
};

/// @brief 提供给渲染系统的拖动中实体列表视图。
struct DragRenderPinnedEntities {
    /// @brief 当前拖动手势中需要绕过静态可见性索引补充渲染的实体列表。
    const std::vector<entt::entity>* entities{ nullptr };
};

/// @brief 共享的上下文状态，记录了当前会话的所有运行时数据，供各个 Controller
/// 和 Tool 访问。
struct SessionContext {
    // --- 核心状态 ---
    entt::registry noteRegistry;      ///< 音符实体的 ECS 注册表
    entt::registry sampleRegistry;    ///< 自动采样实体的独立 ECS 注册表
    entt::registry timelineRegistry;  ///< 时间轴事件(BPM等)的 ECS 注册表
    /// @brief 玩家物件已选实体索引，避免框选热路径扫描完整 Registry。
    std::unordered_set<entt::entity> selectedNoteEntities;
    /// @brief 自动采样已选实体索引，避免框选热路径扫描完整 Registry。
    std::unordered_set<entt::entity> selectedSampleEntities;

    double currentTime{ 0.0 };  ///< 当前逻辑播放时间 (秒)
    double animateTime{ 0.0 };  ///< 当前动画渲染时间，已包含视觉偏移。
    /// @brief 当前暂停态滚动动画的目标渲染时间，单位秒。
    double animateTimeTarget{ 0.0 };
    /// @brief 暂停态滚动动画是否仍需继续推进。
    bool animateTimeAnimationActive{ false };
    /// @brief 当前动画时间线缩放倍率，用于滚轮缩放过渡渲染。
    float animatedTimelineZoom{ 1.0f };
    /// @brief 当前动画时间线缩放的目标倍率。
    float animatedTimelineZoomTarget{ 1.0f };
    /// @brief 时间线缩放动画是否仍需继续推进。
    bool animatedTimelineZoomAnimationActive{ false };
    bool isPlaying{ false };  ///< 是否正在播放
    /// @brief 当前会话是否拥有全局音频时间线的控制权。
    bool isActiveSession{ false };
    /// @brief 是否作为同复合时间线后台跟随者读取全局 transport。
    bool    isAudioTimelineSyncFollower{ false };
    int32_t trackCount{ 12 };  ///< 当前轨道总数
    /// @brief 用户持久化的 BGM 轨道数量，不包含末尾运行时追加轨。
    int32_t bgmTrackCount{ 0 };

    std::shared_ptr<MMM::BeatMap> currentBeatmap;  ///< 当前载入的谱面对象
    /// @brief 访客会话独占的已校验协作资源项目；离线与房主会话为空。
    std::shared_ptr<MMM::Project> collaborationProject;
    /// @brief 房主谱面资源路径到本机内容缓存相对路径的映射。
    std::unordered_map<std::string, std::string> collaborationPathRemap;
    Config::EditorConfig lastConfig;  ///< 最近一次同步的编辑器配置
    std::unordered_map<std::string, CameraInfo>
              cameras;               ///< 当前所有活跃视口的信息
    glm::vec2 bgSize{ 0.0f, 0.0f };  ///< 背景图原始尺寸

    /// @brief 当前 Session 已解析过的画布同步缓冲区缓存。
    /// @warning 逻辑热路径共享指针：快照生成每 update 只读取 map
    /// 中已有 shared_ptr 引用，不复制所有权；首次遇到新 cameraId
    /// 时才从 RenderSyncRegistry 解析并保存所有权。
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        syncBuffers;

    /// @brief 当前 Session 各画布最近一次发布渲染快照的系统时间。
    /// @warning
    /// 逻辑热路径状态：播放时用于给辅助画布快照生成施加背压；只在逻辑线程读写。
    std::unordered_map<std::string, double> lastCameraSnapshotTimes;

    // --- 音频与播放状态 ---
    /// @brief 当前谱面的低频构建复合音频时间线描述符。
    AudioTimelineDescriptor audioTimelineDescriptor;
    /// @brief 将离散音频 block 快照转换为连续播放时间的会话本地时钟。
    PlaybackVisualClock playbackVisualClock;
    /// @brief 谱面或资源语义变化后是否需要重建完整描述符。
    bool isAudioTimelineDescriptorDirty{ true };
    /// @brief 当前会话下次成为活动项时是否必须重新提交音频图。
    bool isAudioTimelineActivationPending{ true };
    /// @brief 新指纹是否尚未发布到 SessionRegistry 的不可变快照。
    bool isAudioTimelineFingerprintPublishPending{ true };
    /// @brief 最近一次成功激活后缓存的复合时间线总时长，单位秒。
    /// @warning 逻辑/UI 热路径只读缓存；写入仅发生在低频音频图切换路径。
    double audioTimelineTotalTime{ 0.0 };
    /// @brief 最近一次激活时无法载入的自动采样数量。
    std::size_t missingAudioTimelineClipCount{ 0U };
    /// @brief 时间线自然结束后，下一次播放是否需要从零秒重新开始。
    bool restartPlaybackAfterFinishPending{ false };

    std::vector<System::HitFXSystem::HitEvent>
           hitEvents;                 ///< 当前谱面所有的打击事件序列
    size_t nextHitIndex{ 0 };         ///< 下一个待触发的视觉打击事件索引
    size_t nextPredictHitIndex{ 0 };  ///< 下一个待触发的预读打击事件(音频)索引
    size_t nextBoundSoundPrefetchIndex{
        0
    };  ///< 下一个待排队的物件绑定音效事件索引
    double nextBoundSoundPrefetchSystemTime{
        0.0
    };  ///< 下一次推进绑定音效后台加载的系统时间
    System::HitFXSystem hitFXSystem;  ///< 打击特效处理系统
    std::vector<const TimelineComponent*>
         bpmEvents;                  ///< 缓存并排序后的 BPM 事件
    bool isBpmEventsDirty{ true };   ///< BPM 缓存脏标记
    bool isHitEventsDirty{ false };  ///< 打击事件序列是否需要按音符变更重建
    bool isNoteOrderDirty{ true };   ///< 音符排序缓存是否需要完整重建
    bool isNotePruneDirty{ false };  ///< 音符排序缓存是否只需剔除失效实体
    bool isNoteStatsDirty{ true };   ///< 状态栏物件统计是否需要重算
    bool isTransformDirty{ true };   ///< 坐标转换缓存脏标记
    std::vector<entt::entity>
        sortedNoteEntities;  ///< 缓存并按时间排序后的音符实体列表
    std::vector<double>
        sortedNoteMaxEndPrefix;  ///< 排序音符列表的前缀最大结束时间缓存
    std::uint64_t noteVisibilityIndexRevision{ 0 };  ///< 音符可见性索引版本号
    /// @brief 按可见区间起点排序的自动采样实体缓存。
    std::vector<entt::entity> sortedSampleEntities;
    /// @brief 自动采样缓存的前缀最大区间终点。
    std::vector<double> sortedSampleMaxEndPrefix;
    /// @brief 自动采样可见性索引版本号。
    std::uint64_t sampleVisibilityIndexRevision{ 0 };
    /// @brief 自动采样排序缓存是否需要完整重建。
    bool isSampleOrderDirty{ true };
    /// @brief 自动采样排序缓存是否只需剔除失效实体。
    bool isSamplePruneDirty{ false };
    /// @brief 密度缓存使用的可计数物件时间，按时间升序排列。
    std::vector<double> previewDensityObjectTimes;
    /// @brief 仅在物件或全谱时长变化时重建的预览密度缓存。
    PreviewDensitySnapshot previewDensityCache;
    /// @brief 预览密度缓存是否需要依据物件时间重建。
    bool   isPreviewDensityDirty{ true };
    size_t noteCount{ 0 };  ///< 当前谱面的可计数物件数量缓存
    size_t maxCombo{ 0 };   ///< 当前谱面的最大连击数缓存
    double lastSnapshotTime{ 0.0 };

    // --- 交互与工具状态 ---
    EditTool     currentTool{ EditTool::Move };  ///< 当前选中的编辑工具类型
    std::string  mouseCameraId;                  ///< 鼠标当前所在的视口 ID
    glm::vec2    lastMousePos{ 0.0f, 0.0f };     ///< 鼠标在对应视口内的最后坐标
    entt::entity hoveredEntity{ entt::null };    ///< 当前悬停的实体 ID
    /// @brief 当前悬停实体所在的独立 ECS 注册表。
    ChartObjectKind hoveredObjectKind{ ChartObjectKind::PlayerNote };
    int32_t         hoveredPart{ 0 };          ///< 悬停的音符部位 (HoverPart)
    int32_t         hoveredSubIndex{ -1 };     ///< 悬停的子索引 (针对折线)
    bool            isMouseInCanvas{ false };  ///< 鼠标是否在任何画布内
    bool            isDragging{ false };       ///< 是否正在执行拖拽操作
    double          previewHoverTime{ 0.0 };   ///< 预览区当前悬停的时间点
    double          previewEdgeScrollVelocity{ 0.0 };  ///< 预览区边缘滚动速度

    entt::entity draggedEntity{ entt::null };  ///< 正在拖拽的实体 ID
    /// @brief 当前拖拽实体所在的独立 ECS 注册表。
    ChartObjectKind draggedObjectKind{ ChartObjectKind::PlayerNote };
    HoverPart       draggedPart{ HoverPart::None };  ///< 正在拖拽的音符部位
    int             draggedSubIndex{ -1 };           ///< 正在拖拽的子索引
    std::optional<NoteComponent>
        dragInitialNote;  ///< 拖拽开始时的初始音符数据 (用于取消或增量计算)
    /// @brief 拖拽开始时的初始自动采样数据。
    std::optional<SampleComponent> dragInitialSample;
    std::string                    dragCameraId;  ///< 发起拖拽的视口 ID
    /// @brief 当前拖动手势中需要补充渲染的实体列表。
    std::vector<entt::entity> dragRenderPinnedEntities;
    /// @brief 当前拖动手势中需要补充渲染的自动采样实体列表。
    std::vector<entt::entity> dragSampleRenderPinnedEntities;

    bool isSelecting{ false };          ///< 是否正在进行框选操作
    bool hasMarqueeSelection{ false };  ///< 是否当前存在有效的框选结果
    bool marqueeIsAdditive{ false };    ///< 框选是否为加选模式 (Ctrl)
    /// @brief 框选区域变化后是否需要重算实体选中状态
    bool                    isMarqueeSelectionDirty{ false };
    std::vector<MarqueeBox> marqueeBoxes;  ///< 当前活跃的框选框列表

    // --- 笔刷工具状态 ---
    struct BrushState {
        bool   isActive{ false };
        double time{ 0.0 };  ///< 当前选中的位置(对 Hold/Flick 为起始点)
        double holdStartTime{ -1.0 };  ///< 记录按下 Shift 瞬间的时间点
        double duration{ 0.0 };        ///< Hold 持续时间
        int    track{ 0 };             ///< 当前轨道 (对 Flick 为起始轨道)
        int    startTrack{ 0 };        ///< Flick 起始轨道
        int    dtrack{ 0 };            ///< Flick 偏移轨道
        float  startMouseY{ 0.0f };    ///< 按下 Shift 瞬间的鼠标 Y 坐标 (像素)
        float  segmentStartMouseY{ 0.0f };  ///< 当前子段开始时的鼠标 Y 坐标
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };

        /// @brief 当前画笔应用到新建物件的自定义颜色。
        NoteColorOverrides customColors;

        /// @brief 当前项目音频工具选中的资源 ID；为空时新建玩家物件不绑定音效。
        std::string selectedAudioResourceId;

        /// @brief 当前项目音频工具选中的资源类型。
        ::MMM::AudioTrackType selectedAudioTrackType{
            ::MMM::AudioTrackType::Effect
        };

        /// @brief 当前项目音频工具为新建物件设置的音量倍率。
        float selectedAudioVolume{ 1.0F };

        /// @brief 当前手势是否正在 BGM 区创建自动采样。
        bool createsAudioSample{ false };

        /// @brief 当前自动采样创建手势锁定的资源 ID。
        std::string activeAudioResourceId;

        /// @brief 当前玩家物件创建或恢复手势锁定的采样绑定。
        std::optional<::MMM::AudioSampleBinding> activeSampleBinding;

        /// @brief 当前画笔是否通过先删除已有物件进入恢复或转换编辑。
        bool replacesExistingObject{ false };

        // Polyline 相关的实时构建链
        std::vector<NoteComponent::SubNote> polylineSegments;
    } brushState;

    // --- 橡皮擦工具状态 ---
    struct EraserState {
        bool isActive{ false };
        bool isShiftDown{ false };  ///< Shift 按下时整体删除 Polyline
        /// @brief 当前擦除目标所在的独立 ECS 注册表。
        ChartObjectKind targetObjectKind{ ChartObjectKind::PlayerNote };
        std::unordered_set<entt::entity> targetEntities;
    } eraserState;

    // --- 同步脏标记 ---
    bool m_needsNotesSync{ false };    ///< 音响实体有变更，需同步到 BeatMap
    bool m_needsSamplesSync{ false };  ///< 自动采样实体有变更，需同步到 BeatMap
    bool m_needsTimingsSync{ false };  ///< 时间线实体有变更，需同步到 BeatMap

    // --- 编辑操作栈 ---
    EditorActionStack          actionStack;  ///< 撤销/重做操作栈
    std::vector<ClipboardItem> clipboard;    ///< 编辑器本地音符剪贴板回退。
    /// @brief 编辑器本地自动采样剪贴板回退。
    std::vector<SampleClipboardItem> sampleClipboard;
    std::string lastActionMessage;  ///< 最近一次操作的详细描述
};

}  // namespace MMM::Logic
