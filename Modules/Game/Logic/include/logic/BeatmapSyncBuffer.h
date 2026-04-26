#pragma once

#include "common/LogicCommands.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "ui/brush/BrushDrawCmd.h"
#include <concurrentqueue.h>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Logic
{

// 预定义纹理ID，用于跨线程纹理映射
enum class TextureID : uint32_t {
    None       = 0,
    Background = 1,
    Note       = 2,
    Node       = 3,
    HoldBodyVertical,
    HoldBodyHorizontal,
    HoldEnd,
    FlickArrowLeft,
    FlickArrowRight,
    Track,
    JudgeArea,
    Logo,

    NoteSelectionBorder = 100,

    EffectStart = 1000
};

enum class HoverPart : uint8_t {
    None = 0,
    Head,
    HoldBody,
    HoldEnd,
    FlickArrow,
    PolylineNode
};

/**
 * @brief 碰撞拾取包围盒
 */
struct Hitbox {
    entt::entity entity;
    HoverPart    part{ HoverPart::None };
    int          subIndex{
                 -1
    };  // 用于区分 Polyline 的第几个 Node 或 Body，或者哪个具体的部分
    float x;
    float y;
    float w;
    float h;
};

/**
 * @brief 时间线上的交互元素 (BPM/Scroll 调整点)
 */
struct TimelineInteractiveElement {
    double       time;
    float        y;
    uint32_t     effects;
    entt::entity bpmEntity{ entt::null };
    entt::entity scrollEntity{ entt::null };
    double       bpmValue{ 0.0 };
    double       scrollValue{ 0.0 };
};

/**
 * @brief 渲染快照数据，包含 UI 画布所需的所有几何与指令信息
 */
struct RenderSnapshot {
    std::vector<Graphic::Vertex::VKBasicVertex> vertices;
    std::vector<uint32_t>                       indices;
    std::vector<UI::BrushDrawCmd>               cmds;
    std::vector<UI::BrushDrawCmd>               glowCmds;
    std::vector<Hitbox>                         hitboxes;
    std::vector<TimelineInteractiveElement>     timelineElements;
    std::vector<System::ScrollSegment>
        scrollSegments;  // 全量 ScrollCache 拷贝，用于 UI 侧时间计算

    // 纹理 UV 映射表 (TextureID -> u,v,w,h)
    std::unordered_map<uint32_t, glm::vec4> uvMap;

    // 背景纹理绝对路径
    std::string backgroundPath;

    // 背景原始尺寸
    glm::vec2 bgSize{ 0.0f, 0.0f };

    // 播放状态
    bool   isPlaying{ false };
    double currentTime{ 0.0 };
    double totalTime{ 0.0 };

    /// @brief 逻辑线程写入该快照时的高精度系统时钟 (steady_clock, 秒)
    double snapshotSysTime{ 0.0 };
    /// @brief 当前播放速度倍率 (用于 UI 侧亚帧插值)
    double playbackSpeed{ 1.0 };

    // 框选盒子快照
    struct MarqueeBoxSnapshot {
        double      startTime{ 0.0 };
        double      endTime{ 0.0 };
        float       startTrack{ 0.0f };
        float       endTrack{ 0.0f };
        std::string cameraId;
    };

    // 交互状态
    EditTool                        currentTool{ EditTool::Move };
    bool                            isHoveringCanvas{ false };
    bool                            isSelecting{ false };
    std::vector<MarqueeBoxSnapshot> marqueeBoxes;
    std::string activeSelectionCameraId;  // 只有在 isSelecting 为 true 时有效

    double  hoveredTime{ 0.0 };
    double  snappedTime{ 0.0 };  // 磁吸后的精确拍线时间
    bool    isSnapped{ false };  // 是否磁吸到了拍线
    int     snappedNumerator{ 0 };
    int     snappedDenominator{ 1 };
    int     currentBeatDivisor{ 4 };
    int32_t hoveredTrack{ 0 };
    int     hoveredNoteNumerator{ 0 };
    int     hoveredNoteDenominator{ 1 };
    double  hoveredNoteTime{ 0.0 };  // 悬浮物件的精确时间戳
    int     hoveredBeatIndex{
            0
    };  // 当前悬浮时间点所在的拍序 (从首个BPMTiming开始)
    int hoveredNoteBeatIndex{ 0 };  // 悬浮物件所在的拍序

    bool   isPreviewHovered{ false };
    float  previewHoverY{ 0.0f };
    double previewHoverTime{ 0.0f };
    bool   isPreviewDragging{ false };

    int32_t trackCount{ 4 };  ///< 谱面轨道数量
    float   renderScaleY{ 1.0f };  ///< 垂直缩放倍率 (用于亚帧补偿计算)

    // 笔刷预览状态
    struct BrushSnapshot {
        bool            isActive{ false };              ///< 是否激活
        double          time{ 0.0 };                    ///< 位置/起始时间
        double          duration{ 0.0 };                ///< 持续时间 (Hold)
        int             track{ 0 };                     ///< 轨道
        int             dtrack{ 0 };                    ///< Flick 偏移轨道
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };  ///< 物件类型

        // Polyline 子物件预览
        std::vector<NoteComponent::SubNote> polylineSegments;
    } brush;

    // 橡皮擦预览状态
    std::unordered_set<entt::entity> erasingEntities;

    // 是否已加载谱面
    bool hasBeatmap{ false };

    /// @brief 静态布局绘制指令数量 (轨道底板 + 轨道边框 + 判定区)
    /// 这些指令对应的几何体不随时间变化，亚帧补偿不应偏移它们
    uint32_t staticCmdCount{ 0 };

    /// @brief 静态布局顶点数量 (与 staticCmdCount 对应的顶点分界)
    /// 从此索引开始到 staticVertexCount + dynamicVertexCount 的所有顶点属于动态元素
    uint32_t staticVertexCount{ 0 };

    /// @brief 动态元素的顶点数量
    /// 用于区分“动态层”之后是否还有“置顶静态层”
    uint32_t dynamicVertexCount{ 0 };

    /// @brief 清理当前快照数据（保留内存容量）
    void clear()
    {
        vertices.clear();
        indices.clear();
        cmds.clear();
        glowCmds.clear();
        hitboxes.clear();
        timelineElements.clear();
        scrollSegments.clear();
        uvMap.clear();
        backgroundPath.clear();
        bgSize           = glm::vec2(0.0f, 0.0f);
        isPlaying        = false;
        currentTime      = 0.0;
        totalTime        = 0.0;
        snapshotSysTime  = 0.0;
        playbackSpeed    = 1.0;
        currentTool      = EditTool::Move;
        isHoveringCanvas = false;
        isSelecting      = false;
        marqueeBoxes.clear();
        activeSelectionCameraId.clear();
        hoveredTime            = 0.0;
        snappedTime            = 0.0;
        isSnapped              = false;
        snappedNumerator       = 0;
        snappedDenominator     = 1;
        currentBeatDivisor     = 4;
        hoveredTrack           = 0;
        hoveredNoteNumerator   = 0;
        hoveredNoteDenominator = 1;
        hoveredBeatIndex       = 0;
        hoveredNoteBeatIndex   = 0;
        hoveredNoteTime        = 0.0;
        isPreviewHovered       = false;
        previewHoverY          = 0.0f;
        previewHoverTime       = 0.0;
        isPreviewDragging      = false;
        brush.isActive         = false;
        erasingEntities.clear();
        hasBeatmap        = false;
        staticCmdCount    = 0;
        staticVertexCount = 0;
        dynamicVertexCount = 0;
    }
};

/**
 * @brief 严格的一对一帧同步管道
 */
class BeatmapSyncBuffer
{
public:
    BeatmapSyncBuffer();
    ~BeatmapSyncBuffer() = default;

    // 禁用拷贝与移动
    BeatmapSyncBuffer(BeatmapSyncBuffer&&)                 = delete;
    BeatmapSyncBuffer(const BeatmapSyncBuffer&)            = delete;
    BeatmapSyncBuffer& operator=(BeatmapSyncBuffer&&)      = delete;
    BeatmapSyncBuffer& operator=(const BeatmapSyncBuffer&) = delete;

    /**
     * @brief [逻辑线程] 获取一个可供写入的工作快照缓冲区
     * @return 准备被写入的缓冲区指针
     */
    RenderSnapshot* getWorkingSnapshot();

    /**
     * @brief [逻辑线程] 提交写完的快照
     */
    void pushWorkingSnapshot();

    /**
     * @brief [UI 线程] 拉取队列中的下一个快照（FIFO）
     * @return
     * 最新的快照指针。如果队列为空，返回上一帧使用的缓存快照以防画面闪烁。
     */
    RenderSnapshot* pullLatestSnapshot();

    /**
     * @brief [逻辑线程] 重置缓冲区，清空所有待读快照
     */
    void reset();

private:
    moodycamel::ConcurrentQueue<RenderSnapshot*> m_freeQueue;
    moodycamel::ConcurrentQueue<RenderSnapshot*> m_readyQueue;

    std::vector<std::unique_ptr<RenderSnapshot>> m_storage;

    RenderSnapshot* m_working{ nullptr };
    RenderSnapshot* m_reading{ nullptr };
};

}  // namespace MMM::Logic
