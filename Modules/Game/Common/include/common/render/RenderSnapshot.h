#pragma once

#include "common/AsciiFontData.h"
#include "common/ChartObjectKind.h"
#include "common/EditTool.h"
#include "common/NoteColor.h"
#include "common/UnicodeFontData.h"
#include "common/render/AnnotationRenderData.h"
#include "common/render/CanvasRenderTypes.h"
#include "common/render/NoteRenderData.h"
#include "common/render/ScrollRenderData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
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


/// @brief 跨逻辑与渲染线程共享的稳定纹理语义 ID。
/// @note 数值分区避免内置纹理、动态特效和字体图集互相冲突。
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

    /// @brief 独立长条头纹理；未提供的皮肤继续使用 Note。
    HoldHead,

    NoteSelectionBorder = 100,

    EffectStart = 1000,

    /// @brief ASCII 字形使用独立高位保留区，避免与皮肤动态特效 ID 冲突。
    AsciiGlyphStart = 0x00100000U,

    /// @brief 按需 Unicode 字形使用独立高位保留区。
    UnicodeGlyphStart = 0x00200000U
};

/// @brief 将 ASCII 字号档位与字符转换为字体图集纹理 ID。
/// @param tierIndex 字号档位索引。
/// @param character ASCII 字符。
/// @return 字符对应纹理 ID；范围外返回 `TextureID::None`。
[[nodiscard]] inline constexpr TextureID asciiGlyphTextureId(
    std::size_t tierIndex, char character)
{
    // 先转换为无符号字节，避免实现相关的负 char 破坏范围判断。
    const auto code = static_cast<unsigned char>(character);
    // 字号档位或字符超出固定 ASCII 图集时显式返回无纹理。
    if ( tierIndex >= Common::ASCII_FONT_RASTER_TIER_COUNT ||
         code < Common::ASCII_GLYPH_FIRST || code > Common::ASCII_GLYPH_LAST ) {
        return TextureID::None;
    }
    // 每个字号档位占用连续区间，字符码点提供区间内偏移。
    return static_cast<TextureID>(
        static_cast<std::uint32_t>(TextureID::AsciiGlyphStart) +
        tierIndex * Common::ASCII_GLYPH_COUNT + code -
        Common::ASCII_GLYPH_FIRST);
}

/// @brief 将 Unicode 码点转换为字体图集纹理 ID。
/// @param codepoint 合法且非 ASCII 的 Unicode 码点。
/// @return 字符对应纹理 ID；范围外返回 `TextureID::None`。
[[nodiscard]] inline constexpr TextureID unicodeGlyphTextureId(
    std::uint32_t codepoint)
{
    // ASCII 使用独立多档图集，非法 Unicode 标量也不得占用纹理编号。
    if ( codepoint <= Common::ASCII_GLYPH_LAST ||
         !Common::isValidUnicodeCodepoint(codepoint) ) {
        return TextureID::None;
    }
    // Unicode 高位保留区直接叠加码点，保证同一码点得到稳定 ID。
    return static_cast<TextureID>(
        static_cast<std::uint32_t>(TextureID::UnicodeGlyphStart) + codepoint);
}

/// @brief 拾取包围盒对应的可交互物件部位。
/// @note 部位用于选择拖动语义，具体物件领域另由 ChartObjectKind 表示。
enum class HoverPart : uint8_t {
    None = 0,
    Head,
    HoldBody,
    HoldEnd,
    FlickArrow,
    PolylineNode,
    SampleAnchor,
    SampleOffset
};

/// @brief UI 悬浮检视面板展示的细分物件结构类型。
/// @note 该枚举比 HoverPart 更细，用于选择不同字段和提示文案。
enum class HoverInspectKind : uint8_t {
    None = 0,
    Note,
    HoldHead,
    HoldBody,
    HoldEnd,
    FlickHead,
    FlickBody,
    FlickEnd,
    PolylineHead,
    PolylineNode,
    PolylineHoldBody,
    PolylineHoldEnd,
    PolylineFlickBody,
    PolylineFlickEnd,
    AudioSampleAnchor,
    AudioSampleTrigger
};

/// @brief 悬浮部位对应的拍位、时间和轨道信息。
struct HoverBeatPoint {
    /// @brief 是否显示该部位的拍位与时间信息
    bool show{ false };
    /// @brief 部位所在拍号
    int beatIndex{ 0 };
    /// @brief 部位所在分拍分子
    int numerator{ 0 };
    /// @brief 部位所在分拍分母
    int denominator{ 1 };
    /// @brief 部位精确时间戳，单位秒
    double time{ 0.0 };
    /// @brief 部位所在拍的起点时间，单位秒。
    double beatStartTime{ 0.0 };
    /// @brief 部位所在拍的终点时间，单位秒。
    double beatEndTime{ 0.0 };
    /// @brief 当前 BPM 下完整一拍的名义时长，单位秒。
    double beatDuration{ 0.0 };
    /// @brief 部位所在轨道
    int32_t track{ 0 };
};

/// @brief 当前悬浮对象交给 UI 展示的结构化只读快照。
struct HoverInspectInfo {
    /// @brief 是否显示结构化悬浮检视信息
    bool show{ false };
    /// @brief 当前检视物件实体。
    entt::entity entity{ entt::null };
    /// @brief 当前检视物件所在的独立 ECS 注册表。
    ChartObjectKind objectKind{ ChartObjectKind::PlayerNote };
    /// @brief 当前悬浮部位类型
    HoverInspectKind kind{ HoverInspectKind::None };
    /// @brief Head 部位信息
    HoverBeatPoint head;
    /// @brief Body 部位信息
    HoverBeatPoint body;
    /// @brief End 部位信息
    HoverBeatPoint end;
    /// @brief 是否显示持续时间
    bool showDuration{ false };
    /// @brief 持续时间，单位秒
    double duration{ 0.0 };
    /// @brief 是否显示滑动轨道数
    bool showDtrack{ false };
    /// @brief Flick 滑动轨道数
    int32_t dtrack{ 0 };
    /// @brief 是否显示轨道位置
    bool showTrack{ false };
    /// @brief 当前部位轨道位置
    int32_t track{ 0 };

    /// @brief 是否显示自动采样资源信息。
    bool showAudioSample{ false };
    /// @brief 当前物件是否存在可独立试听的项目音频引用。
    bool showAudioPreview{ false };
    /// @brief 自动采样引用的项目音频资源 ID。
    std::string audioResourceId;
    /// @brief 自动采样物件音量。
    float volume{ 1.0F };
    /// @brief 当前试听绑定所属的 Polyline 子物件索引；负值表示物件本体。
    std::int32_t sampleBindingSubIndex{ -1 };
    /// @brief 自动采样相对锚点的有符号播放偏移，单位毫秒。
    std::int64_t offsetMs{ 0 };

    /// @brief 当前悬浮位置按重叠检测规则命中的物件数量。
    int overlapCount{ 1 };
};

/// @brief 悬浮检视或编辑手势触发的单轨单拍临时分拍预览。
struct HoverSubdivisionPreview {
    /// @brief 是否需要替换目标轨道当前拍内的分拍线。
    bool show{ false };
    /// @brief 临时预览所属的玩家轨道。
    int32_t track{ 0 };
    /// @brief 悬浮部件在目标拍内的最简分拍分子。
    int numerator{ 0 };
    /// @brief 临时预览使用的分拍分母。
    int denominator{ 1 };
    /// @brief 常用分拍线并集位掩码；为零时仅绘制 denominator 对应分拍线。
    std::uint32_t commonBeatDivisorMask{ 0U };
    /// @brief 触发临时预览的物件部件时间，单位秒。
    double focusTime{ 0.0 };
    /// @brief 目标拍起点时间，单位秒。
    double beatStartTime{ 0.0 };
    /// @brief 目标拍终点时间，单位秒。
    double beatEndTime{ 0.0 };
    /// @brief 生成临时分拍线所用的完整拍时长，单位秒。
    double beatDuration{ 0.0 };
};

/// @brief 用于 UI 碰撞拾取的轴对齐包围盒。
struct Hitbox {
    /// @brief 包围盒所属 ECS 实体。
    entt::entity entity;
    /// @brief 包围盒代表的物件部位。
    HoverPart part{ HoverPart::None };
    /// @brief Polyline 子物件或其他细分部位索引；负值表示物件本体。
    int subIndex{ -1 };
    /// @brief 包围盒左上角横坐标。
    float x;
    /// @brief 包围盒左上角纵坐标。
    float y;
    /// @brief 包围盒宽度。
    float w;
    /// @brief 包围盒高度。
    float h;
    /// @brief 实体所在的独立 ECS 注册表。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };
};

/// @brief 以包围盒中心为基准缩放交互拾取区域。
/// @param hitbox 原始渲染几何对应的包围盒。
/// @param scaleX 横向缩放；非正数或非有限值回退为 1。
/// @param scaleY 纵向缩放；非正数或非有限值回退为 1。
/// @return 保留实体与部件信息的缩放后包围盒。
/// @warning UI 与调试渲染热路径：每个候选框调用，只做常量级算术且不分配。
[[nodiscard]] inline Hitbox scaleInteractionHitbox(const Hitbox& hitbox,
                                                   float         scaleX,
                                                   float scaleY) noexcept
{
    // 非正或非有限缩放不具备有效几何意义，按单位缩放处理。
    const float safeScaleX =
        std::isfinite(scaleX) && scaleX > 0.0F ? scaleX : 1.0F;
    const float safeScaleY =
        std::isfinite(scaleY) && scaleY > 0.0F ? scaleY : 1.0F;
    if ( safeScaleX == 1.0F && safeScaleY == 1.0F ) {
        // 单位缩放直接返回原对象，避免执行无意义的中心重算。
        return hitbox;
    }
    // 先保存原包围盒中心，缩放后再围绕相同中心恢复左上角。
    const float centerX = hitbox.x + hitbox.w * 0.5F;
    const float centerY = hitbox.y + hitbox.h * 0.5F;

    Hitbox scaled = hitbox;
    scaled.w      = hitbox.w * safeScaleX;
    scaled.h      = hitbox.h * safeScaleY;
    scaled.x      = centerX - scaled.w * 0.5F;
    scaled.y      = centerY - scaled.h * 0.5F;
    // 极端输入若导致任一结果溢出，回退完整原包围盒而非返回部分坏值。
    if ( !std::isfinite(scaled.x) || !std::isfinite(scaled.y) ||
         !std::isfinite(scaled.w) || !std::isfinite(scaled.h) ) {
        return hitbox;
    }
    return scaled;
}

/// @brief 时间线上的 BPM、Scroll、Jump 与 HS 交互元素快照。
struct TimelineInteractiveElement {
    /// @brief 单个 Timing marker 的快照几何范围。
    struct MarkerGeometry {
        /// @brief 该 Timing 标记是否拥有可直接修饰的几何体。
        bool hasMarkerGeometry{ false };
        /// @brief 该 Timing 标记在快照顶点数组中的起点。
        uint32_t markerVertexOffset{ 0 };
        /// @brief 该 Timing 标记占用的顶点数量。
        uint32_t markerVertexCount{ 0 };
        /// @brief 该 Timing 标记在快照索引数组中的起点。
        uint32_t markerIndexOffset{ 0 };
        /// @brief 该 Timing 标记占用的索引数量。
        uint32_t markerIndexCount{ 0 };
    };

    /// @brief Timing 元素在谱面时间轴上的时间。
    double time;
    /// @brief 标记在当前画布快照中的纵坐标。
    float y;
    /// @brief 当前时间点实际包含的 Timing 效果位掩码。
    uint32_t effects;
    /// @brief BPM 效果对应实体；不存在时为 entt::null。
    entt::entity bpmEntity{ entt::null };
    /// @brief Scroll 效果对应实体；不存在时为 entt::null。
    entt::entity scrollEntity{ entt::null };
    entt::entity jumpEntity{ entt::null };  /// @brief Jump 效果实体
    entt::entity hsEntity{ entt::null };    /// @brief HS 效果实体
    /// @brief BPM 效果的原始数值。
    double bpmValue{ 0.0 };
    /// @brief Scroll 效果的原始数值。
    double scrollValue{ 0.0 };
    double jumpValue{ 0.0 };  /// @brief Jump 原始参数，单位毫秒
    double hsValue{ 1.0 };    /// @brief HS 原始参数
    /// @brief BPM 标记的快照几何范围。
    MarkerGeometry bpmMarker;
    /// @brief Scroll 标记的快照几何范围。
    MarkerGeometry scrollMarker;
    /// @brief Jump 标记的快照几何范围。
    MarkerGeometry jumpMarker;
    /// @brief HS 标记的快照几何范围。
    MarkerGeometry hsMarker;
    /// @brief 该 Timing 标记是否拥有可直接修饰的几何体。
    bool hasMarkerGeometry{ false };
    /// @brief 该 Timing 标记在快照顶点数组中的起点。
    uint32_t markerVertexOffset{ 0 };
    /// @brief 该 Timing 标记占用的顶点数量。
    uint32_t markerVertexCount{ 0 };
    /// @brief 该 Timing 标记在快照索引数组中的起点。
    uint32_t markerIndexOffset{ 0 };
    /// @brief 该 Timing 标记占用的索引数量。
    uint32_t markerIndexCount{ 0 };
};

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
    /// @brief 普通悬浮拾取与调试显示使用的横向包围盒缩放。
    float interactionHitboxScaleX{ 1.0F };
    /// @brief 普通悬浮拾取与调试显示使用的纵向包围盒缩放。
    float interactionHitboxScaleY{ 1.0F };
    /// @brief 本帧可交互的时间线效果标记。
    std::vector<TimelineInteractiveElement> timelineElements;
    /// @brief 可选画布组件的逐实例渲染与布局边界。
    std::vector<CanvasComponentInstanceSnapshot> canvasComponentInstances;
    /// @brief UI 时间换算使用的全量 ScrollCache 分段副本。
    std::vector<ScrollSegment> scrollSegments;

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
        interactionHitboxScaleX = 1.0F;
        interactionHitboxScaleY = 1.0F;
        overlapMasks.clear();
        annotationMarkers.clear();
        annotationRevision = 0;
        // 清除时间线、组件布局、滚动缓存和预览密度等派生数据。
        timelineElements.clear();
        canvasComponentInstances.clear();
        scrollSegments.clear();
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
