#pragma once

#include "common/AsciiFontData.h"
#include "common/ChartObjectKind.h"
#include "common/EditTool.h"
#include "common/NoteColor.h"
#include "common/UnicodeCodepoint.h"
#include "common/UnicodeFontData.h"
#include "common/render/AnnotationRenderData.h"
#include "common/render/CanvasRenderTypes.h"
#include "common/render/CanvasThicknessCompensation.h"
#include "common/render/HoverRenderData.h"
#include "common/render/NoteRenderData.h"
#include "common/render/ScrollRenderData.h"
#include "common/render/TimelineInteractiveElement.h"
#include "common/render/TimingInterpolationRenderData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Config
{
enum class CanvasComponentType : std::uint8_t;
}

namespace MMM::Common::Render
{
using Logic::ChartObjectKind;
using Logic::EditTool;
using Logic::NoteColorSlot;


/// @brief 单个可选画布组件实例的渲染与布局编辑边界。
struct CanvasComponentInstanceSnapshot {
    /// @brief 组件类型。
    Config::CanvasComponentType type{};

    /// @brief 重复组件实例序号；非重复组件为 0。
    std::int64_t instanceIndex{ 0 };

    /// @brief 实际文字内容左边界。
    float left{ 0.0f };

    /// @brief 实际文字内容上边界。
    float top{ 0.0f };

    /// @brief 实际文字内容右边界。
    float right{ 0.0f };

    /// @brief 实际文字内容下边界。
    float bottom{ 0.0f };

    /// @brief 当前实例允许布局的实际区域左边界。
    float regionLeft{ 0.0f };

    /// @brief 当前实例允许布局的实际区域上边界。
    float regionTop{ 0.0f };

    /// @brief 当前实例允许布局的实际区域右边界。
    float regionRight{ 0.0f };

    /// @brief 当前实例允许布局的实际区域下边界。
    float regionBottom{ 0.0f };
};

/// @brief 主画布中一条已经实际提交绘制的玩家区分拍线。
/// @note 教程等 UI 消费者必须复用这份几何，不能另行近似 BPM 或首拍相位。
struct PlayerBeatLineSnapshot {
    /// @brief 分拍线对应的精确谱面时间，单位秒。
    double time{ 0.0 };

    /// @brief 分拍线中心在主画布逻辑坐标中的纵坐标。
    float y{ 0.0F };
};

/// @brief 逻辑线程生成并交给 UI 画布消费的一帧完整渲染快照。
/// @note 快照按值拥有动态数据，跨线程只传递其稳定对象指针。
struct RenderSnapshot {
    /// @brief 所有普通、发光和覆盖批次共享的顶点缓冲数据。
    std::vector<Common::Render::CanvasVertex> vertices;
    /// @brief 与 vertices 对应的索引缓冲数据。
    std::vector<uint32_t> indices;
    /// @brief 普通混合通道的绘制批次。
    std::vector<Common::Render::CanvasDrawCmd> cmds;
    /// @brief 发光中间通道的绘制批次。
    std::vector<Common::Render::CanvasDrawCmd> glowCmds;
    /// @brief 最终覆盖层的绘制批次。
    std::vector<Common::Render::CanvasDrawCmd> overlayCmds;
    /// @brief 与本帧可见物件对应的拾取包围盒。
    std::vector<Hitbox> hitboxes;
    /// @brief 主画布定高图元与拾取共用的距离动态补偿；辅助视图保持关闭。
    Common::Render::CanvasThicknessCompensation thicknessCompensation{};
    /// @brief 按 Note、Hold、Flick、Polyline
    /// 顺序传递练习物件状态，实体句柄仅在本会话有效。
    struct WalkthroughPracticeNoteState {
        /// @brief 教学只复制前 32 段，避免每帧为四个练习物件分配动态内存。
        static constexpr std::size_t MAX_SUB_NOTES = 32;
        /// @brief 一段用于辨认编辑结果的值语义几何。
        struct SubNoteState {
            ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
            double          timestamp{ 0.0 };
            double          duration{ 0.0 };
            int             track{ 0 };
            int             dtrack{ 0 };
        };
        std::uint64_t token{ 0 };            ///< 对应创建步骤的标记。
        entt::entity  entity{ entt::null };  ///< 创建动作的实体。
        bool          alive{ false };        ///< 同一实体仍然存在。
        /// @brief 根折线最终保留的子段数，其他类型为零。
        std::uint32_t subNoteCount{ 0 };
        /// @brief 根物件类型和几何，用于验收实际落地的局部编辑。
        SubNoteState root{};
        /// @brief 已复制的子段数量；超出固定容量时教学不猜测隐藏段。
        std::uint32_t capturedSubNoteCount{ 0 };
        /// @brief 折线子段的稳定下标与值，不持有逻辑线程组件指针。
        std::array<SubNoteState, MAX_SUB_NOTES> subNotes{};
    };
    /// @brief 固定四个物件的删除状态，不依赖其是否处于可见时间范围。
    std::array<WalkthroughPracticeNoteState, 4> walkthroughPracticeNotes{};
    /// @brief 一次已经结束的编辑手势，配合正式几何防止拖动预览提前完成引导。
    struct WalkthroughEditEvent {
        enum class Kind : std::uint8_t { None, Drag, Brush };
        std::uint64_t revision{ 0 };
        entt::entity  sourceEntity{ entt::null };
        HoverPart     part{ HoverPart::None };
        int           subIndex{ -1 };
        Kind          kind{ Kind::None };
    } walkthroughEditEvent{};
    /// @brief 仅在物件变更批次递增，创作教程据此低频请求完整段落快照。
    std::uint64_t composeNoteRevision{ 0 };
    /// @brief 普通悬浮拾取与调试显示使用的横向包围盒缩放。
    float interactionHitboxScaleX{ 1.0F };
    /// @brief 普通悬浮拾取与调试显示使用的纵向包围盒缩放。
    float interactionHitboxScaleY{ 1.0F };
    /// @brief 本帧可交互的时间线效果标记。
    std::vector<TimelineInteractiveElement> timelineElements;
    /// @brief 全部插值段落的轻量描述，允许段首在视口之外。
    std::vector<TimingInterpolationElement> timingInterpolations;
    /// @brief 可选画布组件的逐实例渲染与布局边界。
    std::vector<CanvasComponentInstanceSnapshot> canvasComponentInstances;
    /// @brief UI 时间换算使用的全量 ScrollCache 分段副本。
    std::vector<ScrollSegment> scrollSegments;

    /// @brief 主画布玩家区本帧实际绘出的分拍线，不包含悬浮临时细分线。
    std::vector<PlayerBeatLineSnapshot> playerBeatLines;

    /// @brief 主画布普通玩家 Note 的实际渲染宽度。
    float playerNoteWidth{ 0.0F };

    /// @brief 主画布普通玩家 Note 的实际渲染高度。
    float playerNoteHeight{ 0.0F };

    /// @brief 预览窗口右侧全谱物件密度缓存；非 Preview 快照保持为空。
    PreviewDensitySnapshot previewDensity;

    /// @brief 重叠检测遮罩区域，使用当前快照的屏幕坐标。
    struct OverlapMask {
        /// @brief 遮罩左上角 X 坐标。
        float x{ 0.0f };

        /// @brief 遮罩左上角 Y 坐标。
        float y{ 0.0f };

        /// @brief 遮罩宽度。
        float w{ 0.0f };

        /// @brief 遮罩高度。
        float h{ 0.0f };

        /// @brief 该遮罩代表的重叠物件数量。
        int objectCount{ 2 };
    };

    /// @brief 当前快照中需要覆盖显示的重叠遮罩。
    std::vector<OverlapMask> overlapMasks;

    /// @brief 主画布当前可见时间范围内的批注标记。
    std::vector<AnnotationRenderMarker> annotationMarkers;

    /// @brief 当前会话全量批注标记缓存的版本号。
    std::uint64_t annotationRevision{ 0 };

    /// @brief 从 TextureID 到图集矩形 u、v、w、h 的映射表。
    std::unordered_map<uint32_t, glm::vec4> uvMap;

    /// @brief 当前快照持有的图集 UV 修订号。
    std::uint64_t atlasUvRevision{ 0 };

    /// @brief 当前主画布 ASCII 字体的多档归一化字形度量。
    Common::AsciiFontAtlasMetrics asciiFontAtlasMetrics;

    /// @brief 当前主画布按项目资源名加载的 Unicode 字形度量。
    Common::UnicodeFontMetrics unicodeFontMetrics;

    /// @brief 单个快照最多回报的缺失 Unicode 字形数量。
    static constexpr std::size_t MAX_REQUESTED_UNICODE_GLYPHS = 256U;

    /// @brief 本帧可见标签请求补载的 Unicode 码点。
    std::array<std::uint32_t, MAX_REQUESTED_UNICODE_GLYPHS>
        requestedUnicodeGlyphs{};

    /// @brief `requestedUnicodeGlyphs` 中的有效元素数量。
    std::size_t requestedUnicodeGlyphCount{ 0U };

    /// @brief 记录当前字体图集中缺失的 Unicode 字形。
    /// @param codepoint 待补载码点。
    /// @warning 逻辑渲染热路径：只扫描固定上限栈内数组，不分配内存。
    void requestUnicodeGlyph(std::uint32_t codepoint)
    {
        // ASCII、非法标量和图集中已有字形均无需进入补载请求。
        if ( codepoint <= Common::ASCII_GLYPH_LAST ||
             !Common::isValidUnicodeCodepoint(codepoint) ||
             unicodeFontMetrics.glyph(codepoint) ) {
            return;
        }
        // 固定数组按当前有效范围去重，避免同一可见字符重复消耗容量。
        for ( std::size_t index = 0U; index < requestedUnicodeGlyphCount;
              ++index ) {
            if ( requestedUnicodeGlyphs[index] == codepoint ) return;
        }
        // 达到单帧上限后静默保留既有请求，下一帧仍可继续发现缺失字形。
        if ( requestedUnicodeGlyphCount < requestedUnicodeGlyphs.size() ) {
            requestedUnicodeGlyphs[requestedUnicodeGlyphCount++] = codepoint;
        }
    }

    /// @brief 逻辑线程可见音符查询临时列表，UI 线程不读取。
    std::vector<entt::entity> noteQueryScratch;

    /// @brief 逻辑线程可见音符查询去重临时集合，UI 线程不读取。
    std::unordered_set<entt::entity> noteQuerySeenScratch;

    /// @brief 逻辑线程可见自动采样查询临时列表，UI 线程不读取。
    std::vector<entt::entity> sampleQueryScratch;

    /// @brief 逻辑线程可见自动采样查询去重临时集合，UI 线程不读取。
    std::unordered_set<entt::entity> sampleQuerySeenScratch;

    /// @brief 背景资源绝对 UTF-8 路径。
    std::string backgroundPath;

    /// @brief 背景原始尺寸。
    glm::vec2 bgSize{ 0.0f, 0.0f };

    /// @brief 当前背景资源是否为视频。
    bool backgroundIsVideo{ false };

    /// @brief 视频事件在谱面时间轴上的开始时间，单位秒。
    double backgroundVideoStartTime{ 0.0 };

    /// @brief 生成快照时音频时间线是否正在播放。
    bool isPlaying{ false };
    /// @brief 本地是否正在预览连续 Seek；为 true 时联机视口暂缓发送。
    bool isSeekScrubbing{ false };
    /// @brief 包含视觉偏移的当前画布时间，单位秒。
    double currentTime{ 0.0 };
    /// @brief 当前主画布内容相对基础轨道布局的横向逻辑像素偏移。
    float canvasHorizontalOffsetX{ 0.0F };
    /// @brief 未包含视觉偏移的原始谱面播放时间，单位秒。
    double playbackTime{ 0.0 };
    /// @brief 当前谱面或音频时间线总时长，单位秒。
    double totalTime{ 0.0 };

    /// @brief 逻辑线程写入快照时的 steady_clock 时间，单位秒。
    double snapshotSysTime{ 0.0 };
    /// @brief 当前播放速度倍率 (用于 UI 侧亚帧插值)
    double playbackSpeed{ 1.0 };

    /// @brief 当前快照是否允许 UI 线程对动态顶点做线性播放补间。
    /// @warning UI 每帧路径读取；逻辑线程只在生成快照时写入，不得在 UI 侧修改。
    bool allowUiPlaybackInterpolation{ false };

    /// @brief UI 播放补间使用的 AbsY 每秒速度。
    /// @warning UI 每帧路径读取；只承载线性滚动段速度，SV/JUMP
    /// 边界附近必须置零。
    double uiInterpolationAbsYSpeed{ 0.0 };

    /// @brief UI 播放补间从 AbsY 到画布 Y 偏移的倍率。
    /// @warning UI 每帧路径读取；Timeline 需要乘当前 HS，Preview
    /// 仍由 CanvasSnapshotPrepare 按 renderScaleY 额外缩放。
    double uiInterpolationYOffsetScale{ 1.0 };

    /// @brief 无效 BPM 事件的会话级回退 BPM。
    double fallbackBpm{ 120.0 };

    /// @brief 当前判定线所在时间段生效的 BPM。
    double currentBpm{ 120.0 };

    /// @brief 当前判定线从首个 BPM Timing 起算的拍号；0 表示尚未进入首拍。
    int currentBeatIndex{ 0 };

    /// @brief 当前判定线所在时间段生效的 SV。
    double currentSv{ 1.0 };

    /// @brief 一台画布摄像机上的矩形框选时间与轨道范围。
    struct MarqueeBoxSnapshot {
        double      startTime{ 0.0 };
        double      endTime{ 0.0 };
        float       startTrack{ 0.0f };
        float       endTrack{ 0.0f };
        std::string cameraId;
    };

    /// @brief 生成当前交互快照时启用的编辑工具。
    EditTool currentTool{ EditTool::Move };
    /// @brief 当前快照是否允许生成拾取/悬浮等交互数据。
    bool                            acceptsInteraction{ false };
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
    int32_t hoveredNoteTrack{ 0 };   ///< 悬浮物件精确部件所在轨道
    int     hoveredBeatIndex{
            0
    };  // 当前悬浮时间点所在的拍序 (从首个BPMTiming开始)
    int hoveredNoteBeatIndex{ 0 };  // 悬浮物件所在的拍序
    /// @brief 当前悬浮物件的结构化检视信息
    HoverInspectInfo hoverInspect;
    /// @brief 当前悬浮 Note 的单轨单拍临时分拍预览。
    HoverSubdivisionPreview hoverSubdivisionPreview;

    bool   isPreviewHovered{ false };
    float  previewHoverY{ 0.0f };
    double previewHoverTime{ 0.0f };
    bool   isPreviewDragging{ false };

    /// @brief BMS 每侧普通键数，5/7 含独立转盘，9 表示无转盘 PMS；0 不标记。
    /// @note 逻辑线程从来源元数据发布，渲染线程不读取可变谱面或文件路径。
    int32_t bmsKeysPerSide{ 0 };
    int32_t trackCount{ 4 };  ///< 谱面玩家轨道数量。
    /// @brief 持久化草稿轨道数量，不包含最左侧运行时追加轨。
    int32_t draftTrackCount{ 4 };
    /// @brief 持久化 BGM 轨道数量，不包含运行时追加轨。
    int32_t bgmTrackCount{ 0 };
    /// @brief 当前快照是否显示并允许交互 BGM 轨道区。
    bool bmsEditingEnabled{ true };
    /// @brief 当前快照是否按全局专业模式显示并允许交互草稿轨道区。
    bool   draftLanesEnabled{ false };
    float  renderScaleY{ 1.0f };     ///< 垂直缩放倍率 (用于亚帧补偿计算)
    double visibleTimeStart{ 0.0 };  ///< 当前视口可见的时间范围起点
    double visibleTimeEnd{ 0.0 };    ///< 当前视口可见的时间范围终点
    size_t noteCount{ 0 };           ///< 当前谱面的可计数物件数量
    size_t maxCombo{ 0 };            ///< 当前谱面的最大连击数

    /// @brief 当前绘制手势交给画布展示的预览状态。
    struct BrushSnapshot {
        bool isActive{ false };  ///< 是否激活
        /// @brief 当前手势是否创建 BGM 区自动采样。
        bool            createsAudioSample{ false };
        double          time{ 0.0 };                    ///< 位置/起始时间
        double          duration{ 0.0 };                ///< 持续时间 (Hold)
        int             track{ 0 };                     ///< 轨道
        int             dtrack{ 0 };                    ///< Flick 偏移轨道
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };  ///< 物件类型

        /// @brief 笔刷预览使用的自定义颜色。
        Common::Render::NoteColorOverrides customColors;

        /// @brief 自动采样预览引用的项目音频资源 ID。
        std::string audioResourceId;

        /// @brief Polyline 手势尚未提交的子物件预览。
        std::vector<Common::Render::PolylineSubNote> polylineSegments;
    } brush;

    /// @brief 当前橡皮擦手势累计命中的实体集合。
    std::unordered_set<entt::entity> erasingEntities;
    /// @brief 橡皮擦目标所在的独立 ECS 注册表。
    ChartObjectKind erasingObjectKind{ ChartObjectKind::PlayerNote };
    int             erasingSubIndex{ -1 };

    /// @brief 当前快照是否对应已加载谱面。
    bool hasBeatmap{ false };
    /// @brief 当前快照对应的谱面实例标识，仅用于进程内比较，禁止解引用。
    std::uintptr_t beatmapInstanceId{ 0 };
    /// @brief 当前快照对应谱面的项目内或绝对路径键。
    std::string beatmapPathKey;
    std::string beatmapName;
    bool        isDirty{ false };
    std::string lastActionMessage;

    /// @brief 静态布局绘制指令数量 (轨道底板 + 轨道边框 + 判定区)
    /// 这些指令对应的几何体不随时间变化，亚帧补偿不应偏移它们
    uint32_t staticCmdCount{ 0 };

    /// @brief 静态布局顶点数量 (与 staticCmdCount 对应的顶点分界)
    /// 从此索引开始到 staticVertexCount + dynamicVertexCount
    /// 的所有顶点属于动态元素
    uint32_t staticVertexCount{ 0 };

    /// @brief 动态元素的顶点数量
    /// 用于区分“动态层”之后是否还有“置顶静态层”
    uint32_t dynamicVertexCount{ 0 };

    /// @brief 计算当前 UI 时刻相对快照的有效播放补间时长。
    /// @param nowSteadySeconds 当前 steady_clock 秒数。
    /// @return 仅播放中且处于 100ms 新鲜窗口时返回正时长，否则返回零。
    /// @warning UI 每帧路径：只做常量级数值校验。
    [[nodiscard]] double playbackInterpolationElapsed(
        double nowSteadySeconds) const noexcept
    {
        // 非播放、无快照时钟或无效速度都不能进行 UI 侧外推。
        if ( !isPlaying || snapshotSysTime <= 0.0 ||
             !std::isfinite(nowSteadySeconds) ||
             !std::isfinite(playbackSpeed) || playbackSpeed <= 0.0 ) {
            return 0.0;
        }
        // 只接受快照之后 100ms 内的正时间差，防止停顿后继续外推旧状态。
        double elapsed = nowSteadySeconds - snapshotSysTime;
        if ( elapsed <= 0.0 || elapsed >= 0.1 ) {
            return 0.0;
        }
        // 已知播放终点时把外推限制在剩余媒体时间内，避免越过尾端。
        if ( std::isfinite(playbackTime) && std::isfinite(totalTime) ) {
            const double remainingTime =
                (totalTime - playbackTime) / playbackSpeed;
            if ( remainingTime <= 0.0 ) {
                // 已到达或越过终点时不再产生补间时间。
                return 0.0;
            }
            elapsed = std::min(elapsed, remainingTime);
        }
        return elapsed;
    }

    /// @brief 将动画时间解析到当前 UI 壁钟。
    /// @param nowSteadySeconds 当前 steady_clock 秒数。
    /// @return 与画布补间同源的动画时间。
    /// @warning UI 每帧路径：只做常量级时间计算。
    [[nodiscard]] double resolveCurrentTimeAt(
        double nowSteadySeconds) const noexcept
    {
        // 画布时间沿用与顶点补间相同的新鲜窗口和播放速度。
        const double resolved =
            currentTime +
            playbackInterpolationElapsed(nowSteadySeconds) * playbackSpeed;
        // 极端数值溢出时回退快照原值，避免 NaN 进入渲染计算。
        return std::isfinite(resolved) ? resolved : currentTime;
    }

    /// @brief 将未含视觉偏移的播放时间解析到当前 UI 壁钟。
    /// @param nowSteadySeconds 当前 steady_clock 秒数。
    /// @return 与画布补间同源的谱面播放时间。
    /// @warning UI 每帧路径：只做常量级时间计算。
    [[nodiscard]] double resolvePlaybackTimeAt(
        double nowSteadySeconds) const noexcept
    {
        // 原始播放时间不包含视觉偏移，但共享同一补间经过时长。
        const double resolved =
            playbackTime +
            playbackInterpolationElapsed(nowSteadySeconds) * playbackSpeed;
        return std::isfinite(resolved) ? resolved : playbackTime;
    }

    /**
     * @brief [UI 线程专用] 亚帧插值：获取从 currentTime 到 currentTime + dt
     * 的累积绝对位移
     *
     * 普通线性滚动段可以直接使用快照记录的 AbsY 速度补间；高 SV、JUMP
     * 或跨段边界 会在快照生成时关闭 allowUiPlaybackInterpolation，避免 UI
     * 线程显示不稳定中间态。
     *
     * @param dt 滞后时间 (秒，UI绘制时刻 - 快照生成时刻)。
     * @return 累积位移 (AbsY 空间)
     * @warning UI 每帧路径：禁止在这里遍历 ScrollSegment 或做高 SV 分段积分。
     */
    double getInterpolatedOffset(double dt) const
    {
        // 只有逻辑层明确允许的线性滚动段才执行亚帧位移外推。
        if ( !allowUiPlaybackInterpolation || !isPlaying || dt <= 0.0 ||
             dt >= 0.1 || !std::isfinite(dt) ||
             !std::isfinite(uiInterpolationAbsYSpeed) ) {
            return 0.0;
        }
        // AbsY 速度先转换到画布 Y 空间，再乘以经过秒数得到累积偏移。
        return uiInterpolationAbsYSpeed * uiInterpolationYOffsetScale * dt;
    }

    /// @brief 清理当前快照业务数据并保留动态容器容量。
    /// @warning 逻辑热路径复用入口；只能由当前拥有该快照的线程调用。
    void clear()
    {
        // 首先清空 GPU 几何、绘制批次和交互几何，保留各 vector 容量。
        vertices.clear();
        indices.clear();
        cmds.clear();
        glowCmds.clear();
        overlayCmds.clear();
        hitboxes.clear();
        // 缓冲跨画布复用时不得将主画布的三维补偿带入时间线或缩略预览。
        thicknessCompensation    = {};
        walkthroughPracticeNotes = {};
        // 复用快照缓冲时不能让上张谱面的释放事务流入下一张谱面。
        walkthroughEditEvent    = {};
        composeNoteRevision     = 0;
        interactionHitboxScaleX = 1.0F;
        interactionHitboxScaleY = 1.0F;
        overlapMasks.clear();
        annotationMarkers.clear();
        annotationRevision = 0;
        // 清除时间线、组件布局、滚动缓存和预览密度等派生数据。
        timelineElements.clear();
        timingInterpolations.clear();
        canvasComponentInstances.clear();
        scrollSegments.clear();
        playerBeatLines.clear();
        playerNoteWidth  = 0.0F;
        playerNoteHeight = 0.0F;
        previewDensity.clear();
        requestedUnicodeGlyphCount = 0U;
        noteQueryScratch.clear();
        noteQuerySeenScratch.clear();
        sampleQueryScratch.clear();
        sampleQuerySeenScratch.clear();
        // 资源字段恢复为空，视频和背景状态不能泄漏到下一个项目快照。
        backgroundPath.clear();
        bgSize                       = glm::vec2(0.0f, 0.0f);
        backgroundIsVideo            = false;
        backgroundVideoStartTime     = 0.0;
        isPlaying                    = false;
        isSeekScrubbing              = false;
        currentTime                  = 0.0;
        canvasHorizontalOffsetX      = 0.0F;
        playbackTime                 = 0.0;
        totalTime                    = 0.0;
        snapshotSysTime              = 0.0;
        playbackSpeed                = 1.0;
        allowUiPlaybackInterpolation = false;
        uiInterpolationAbsYSpeed     = 0.0;
        uiInterpolationYOffsetScale  = 1.0;
        // 音乐和滚动上下文恢复安全默认值，供尚未写入的新快照读取。
        fallbackBpm      = 120.0;
        currentBpm       = 120.0;
        currentBeatIndex = 0;
        currentSv        = 1.0;
        currentTool      = EditTool::Move;
        // 清除全部交互与磁吸状态，避免复用时延续上一帧手势反馈。
        acceptsInteraction = false;
        isHoveringCanvas   = false;
        isSelecting        = false;
        marqueeBoxes.clear();
        activeSelectionCameraId.clear();
        hoveredTime             = 0.0;
        snappedTime             = 0.0;
        isSnapped               = false;
        snappedNumerator        = 0;
        snappedDenominator      = 1;
        currentBeatDivisor      = 4;
        hoveredTrack            = 0;
        hoveredNoteNumerator    = 0;
        hoveredNoteDenominator  = 1;
        hoveredBeatIndex        = 0;
        hoveredNoteBeatIndex    = 0;
        hoveredNoteTime         = 0.0;
        hoveredNoteTrack        = 0;
        hoverInspect            = HoverInspectInfo{};
        hoverSubdivisionPreview = HoverSubdivisionPreview{};
        isPreviewHovered        = false;
        previewHoverY           = 0.0f;
        previewHoverTime        = 0.0;
        isPreviewDragging       = false;
        // 笔刷内部容器保留容量，但清空当前预览身份和颜色覆盖。
        brush.isActive           = false;
        brush.createsAudioSample = false;
        brush.customColors       = {};
        brush.audioResourceId.clear();
        brush.polylineSegments.clear();
        erasingEntities.clear();
        // 橡皮擦恢复玩家音符领域和无子物件的初始状态。
        erasingObjectKind = ChartObjectKind::PlayerNote;
        erasingSubIndex   = -1;
        // 清除来源键位，切换到普通谱面时不能沿用上次的转盘标记。
        bmsKeysPerSide    = 0;
        hasBeatmap        = false;
        beatmapInstanceId = 0;
        beatmapPathKey.clear();
        beatmapName.clear();
        isDirty = false;
        lastActionMessage.clear();
        // 几何分层计数必须与已清空的顶点和指令数组同步归零。
        staticCmdCount     = 0;
        staticVertexCount  = 0;
        dynamicVertexCount = 0;
        visibleTimeStart   = 0.0;
        visibleTimeEnd     = 0.0;
        noteCount          = 0;
        maxCombo           = 0;
        // 轨道可见性恢复默认编辑模式；玩家轨数本身由下一次生成覆盖。
        draftTrackCount   = trackCount;
        bgmTrackCount     = 0;
        bmsEditingEnabled = true;
        draftLanesEnabled = false;
    }
};

}  // namespace MMM::Common::Render
