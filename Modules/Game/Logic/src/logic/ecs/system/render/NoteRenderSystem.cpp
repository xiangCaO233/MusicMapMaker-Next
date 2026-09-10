#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/CanvasComponentRenderSystem.h"
#include "logic/ecs/system/SampleRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

namespace
{

/// @brief UI 播放补间窗口，单位为 steady-clock 秒。
/// @warning 快照热路径常量；需与 CanvasSnapshotPrepare 保持同步。
constexpr double UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS = 0.1;

/// @brief Timeline UI 补间安全窗口，单位为 steady-clock 秒。
/// @warning 快照热路径常量：Timeline 使用屏幕 Y 补偿，窗口取接近
/// 辅助视图快照间隔的保守值，避免密集 Timing 过度关闭补间。
constexpr double TIMELINE_UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS =
    1.0 / 120.0;

/// @brief Timeline 专业模式轨道数量。
constexpr int PROFESSIONAL_TIMELINE_LANE_COUNT = 4;

/// @brief 草稿轨道纹理叠加色键，避免快照热路径构造临时字符串。
const std::string DRAFT_TRACK_TEXTURE_TINT_COLOR_KEY =
    "draft_tracks.texture_tint";
/// @brief 草稿轨道覆盖色键，避免快照热路径构造临时字符串。
const std::string DRAFT_TRACK_OVERLAY_COLOR_KEY = "draft_tracks.overlay";
/// @brief 草稿轨道边框色键，避免快照热路径构造临时字符串。
const std::string DRAFT_TRACK_BORDER_COLOR_KEY = "draft_tracks.border";
/// @brief 草稿轨道判定区叠加色键，避免快照热路径构造临时字符串。
const std::string DRAFT_TRACK_JUDGMENT_TINT_COLOR_KEY =
    "draft_tracks.judgment_tint";

/// @brief 判断皮肤颜色是否为缺省的洋红哨兵。
/// @param color 皮肤颜色。
/// @return 颜色为缺省哨兵时返回 true。
/// @note 使用精确 RGBA 哨兵，不按近似洋红颜色猜测资源缺失。
/// @warning 每次轨道配色查询可调用，仅做固定数量浮点比较。
bool isMissingSkinColor(const Config::Color& color)
{
    // 透明度也参与缺省判断。
    // 用户指定的半透明洋红不应触发回退。
    return color.r == 1.0F && color.g == 0.0F && color.b == 1.0F &&
           color.a == 1.0F;
}

/// @brief 获取可回退的主画布轨道皮肤颜色。
/// @param key 皮肤颜色键。
/// @param fallback 缺失时使用的颜色。
/// @return 转换后的 GLM 颜色。
/// @warning
/// 快照热路径：只接收静态键并执行内存内皮肤颜色查询，不得访问文件系统。
glm::vec4 laneColor(const std::string& key, glm::vec4 fallback)
{
    // 只读取已加载的颜色表，不保留对表条目的引用。
    // 缺省哨兵在这里转换为区域自己的默认色。
    const auto color = Config::SkinManager::instance().getColor(key);
    if ( isMissingSkinColor(color) ) return fallback;
    return { color.r, color.g, color.b, color.a };
}

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
/// @param effect 待分类的 Timing 类型。
/// @return BPM、SCROLL、JUMP、HS 分别对应零到三；未知值回退零。
/// @note 此顺序必须与专业轨道底色及标记遍历顺序一致。
/// @warning Timeline 快照热路径，禁止动态建立类型映射表。
int professionalTimelineLane(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 获取 Timeline Timing 类型的快照效果掩码。
/// @param effect 单个 Timing 类型。
/// @return 对应的位掩码；未知类型返回零。
/// @note 一个 ScrollSegment 可以携带多个效果，不能用枚举相等代替按位判断。
/// @warning 标记生成热路径，只返回固定掩码，不分配内存。
uint32_t timelineEffectMask(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return SCROLL_EFFECT_BPM;
    case ::MMM::TimingEffect::SCROLL: return SCROLL_EFFECT_SCROLL;
    case ::MMM::TimingEffect::JUMP: return SCROLL_EFFECT_JUMP;
    case ::MMM::TimingEffect::HS: return SCROLL_EFFECT_HS;
    }
    return 0;
}

/// @brief 获取 Timeline 元素中指定 Timing 类型的 marker 几何槽。
/// @param element 接收几何区间的交互元素。
/// @param effect 当前绘制的效果类型。
/// @return 元素内对应槽的可写引用，未知类型沿用 scroll 槽。
/// @pre element 在引用使用期间不得因容器扩容失效。
/// @note 槽记录顶点与索引区间，不拥有额外几何缓冲。
/// @warning 每个可见 Timing 标记调用，不复制交互元素或共享所有权。
TimelineInteractiveElement::MarkerGeometry& markerGeometryForEffect(
    TimelineInteractiveElement& element, ::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return element.bpmMarker;
    case ::MMM::TimingEffect::SCROLL: return element.scrollMarker;
    case ::MMM::TimingEffect::JUMP: return element.jumpMarker;
    case ::MMM::TimingEffect::HS: return element.hsMarker;
    }
    return element.scrollMarker;
}

/// @brief 绘制时间线/预览用的小型判定框。
/// @warning 热路径：每个 Timeline/Preview 快照生成时执行；只推送固定数量几何。
/// @param batcher 接收矩形与边框的批处理器。
/// @param leftX 判定框左边界，单位为像素。
/// @param centerY 框中心的屏幕 Y。
/// @param width 框宽，过窄时不提交几何。
/// @param height 框高，过薄时不提交几何。
/// @note 使用无纹理几何；当前裁剪状态由调用方选择。
/// @note 本函数不恢复调用前的纹理状态，也不主动提交尾批。
void drawJudgmentGuideBox(Batcher& batcher, float leftX, float centerY,
                          float width, float height)
{
    // 过小尺寸既不可辨认，也容易形成退化边框。
    // 跳过时不改变批处理器的纹理和裁剪状态。
    if ( width <= 0.5f || height <= 0.5f ) return;

    auto& skin        = Config::SkinManager::instance();
    auto  fillColor   = skin.getColor("preview.judgment_guide.fill");
    auto  borderColor = skin.getColor("preview.judgment_guide.border");
    constexpr float strokeWidth = 2.0f;

    // pushQuad 接收底边坐标，描边入口则接收两端边界。
    // 两套接口共享同一中心与半高，避免填充和描边偏移。
    const float topY    = centerY - height * 0.5f;
    const float bottomY = centerY + height * 0.5f;
    batcher.setTexture(TextureID::None);
    batcher.pushQuad(leftX,
                     bottomY,
                     width,
                     height,
                     { fillColor.r, fillColor.g, fillColor.b, fillColor.a });
    batcher.pushStrokeRect(
        leftX,
        topY,
        leftX + width,
        bottomY,
        strokeWidth,
        { borderColor.r, borderColor.g, borderColor.b, borderColor.a });
}

}  // namespace

/// @brief 生成指定画布的批量渲染快照。
/// @warning 热路径：每帧/每 update 执行；禁止引入文件系统访问、
/// registry 全量无缓存扫描或阻塞同步；Timeline 与活跃主画布 Move
/// 工具会复制 ScrollSegment 供 UI 精确时间映射。
/// @param registry 当前会话的音符 registry，并借用滚动缓存到上下文。
/// @param sampleRegistry 自动采样 registry，生命周期覆盖本次快照生成。
/// @param sortedSampleEntities 已按时间排序的采样索引。
/// @param sortedSampleMaxEndPrefix 与索引对应的最大结束时间前缀。
/// @param timelineRegistry 提供 ScrollCache 的时间线 registry。
/// @param bpmEvents 会话维护的有序 BPM 组件观察指针。
/// @param snapshot 调用方提供的可写快照，进入前已准备本帧状态。
/// @param cameraId 画布标识，区分主画布、Preview 与 Timeline。
/// @param currentTime 播放或编辑位置，单位为秒。
/// @param viewportWidth 当前画布像素宽度。
/// @param viewportHeight 当前画布像素高度。
/// @param judgmentLineY 当前画布判定线屏幕坐标。
/// @param trackCount 玩家轨数，轨道投影要求有效正值。
/// @param bgmTrackCount 自动采样区域的逻辑轨数。
/// @param draftTrackCount 独立草稿区域的逻辑轨数。
/// @param config 本次快照使用的会话配置。
/// @param mainViewportHeight 主画布高度，用于换算预览可见范围。
/// @param hitFXSystem 可选特效状态；为空时不生成特效且逐轨 KPS 为空。
/// @pre snapshot 非空，调用期间不能被另一线程同时写入。
/// @pre 索引与 registry 的内容对应，不能在生成中途重排或删除实体。
/// @note 只生产 CPU 几何和交互描述；GPU 资源准备属于消费端。
/// @note 静态和动态边界按顶点位置记录，命令覆盖顺序可独立调整。
/// @note 缺少 ScrollCache 时提前返回，由调用方决定快照是否发布。
void NoteRenderSystem::generateSnapshot(
    entt::registry& registry, entt::registry& sampleRegistry,
    const std::vector<entt::entity>&             sortedSampleEntities,
    const std::vector<double>&                   sortedSampleMaxEndPrefix,
    const entt::registry&                        timelineRegistry,
    const std::vector<const TimelineComponent*>& bpmEvents,
    RenderSnapshot* snapshot, const std::string& cameraId, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, int32_t bgmTrackCount, int32_t draftTrackCount,
    const Config::EditorConfig& config, float mainViewportHeight,
    HitFXSystem* hitFXSystem)
{
    // 相机 ID 决定视图职责，不根据视口尺寸猜测主画布。
    // 多个主画布会话都应走相同的主画布分支。
    const bool isMainCanvas = SessionUtils::isMainCanvasCameraId(cameraId);
    // 缩放由持久化配置输入，先排除非有限值。
    // 快照保存规范结果，使 UI 拾取和调试框使用相同尺度。
    /// @brief 规范化将随快照发布的交互缩放。
    /// @param scale 原始配置值。
    /// @return 有限且位于配置允许区间内的缩放。
    /// @warning 每帧调用，仅做有限值检查和限幅。
    const auto normalizeInteractionHitboxScale = [](float scale) {
        if ( !std::isfinite(scale) ) {
            return Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE;
        }
        return std::clamp(scale,
                          Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                          Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE);
    };
    snapshot->interactionHitboxScaleX =
        normalizeInteractionHitboxScale(config.visual.interactionHitboxScaleX);
    snapshot->interactionHitboxScaleY =
        normalizeInteractionHitboxScale(config.visual.interactionHitboxScaleY);

    // 核心同步：如果预览区正在拖拽，主画布渲染的时间应该是预览区当前的悬停时间
    // 预览拖动期间先改变本地视觉时间，不等待播放时钟追上。
    // 只覆盖主画布的绘制时间，Preview 本身仍保留交互状态。
    double renderTime = currentTime;
    if ( isMainCanvas && snapshot->isPreviewDragging ) {
        renderTime = snapshot->previewHoverTime;
    }

    // 滚动映射由时间线侧预先维护。
    // 没有缓存就不能可靠地把时间转换成像素，不在此临时构建。
    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 指针存入 context 供 renderPolyline 等后续使用
    // 上下文保存的是观察指针，所有权仍在 timelineRegistry。
    // 已有槽仅更新地址，避免每次快照替换上下文对象。
    // 调用方必须保证绘制期间缓存不被销毁。
    if ( auto** cacheSlot = registry.ctx().find<const ScrollCache*>() ) {
        *cacheSlot = cache;
    } else {
        registry.ctx().emplace<const ScrollCache*>(cache);
    }

    // Timeline 按屏幕 Y 补偿，使用更短的线性安全窗口。
    // 其他支持补间的画布沿用主画布窗口。
    // 窗口是预测有效范围，不是线程等待时长。
    const double interpolationWindow =
        cameraId == "Timeline"
            ? TIMELINE_UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS
            : UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS;
    // 播放倍率可能为负，安全区间长度始终取非负值。
    // 真正的运动方向由后面带符号的速度表达。
    const double interpolationDuration =
        std::abs(snapshot->playbackSpeed) * interpolationWindow;
    const bool supportsUiPlaybackInterpolation =
        isMainCanvas || cameraId == "Preview" || cameraId == "Timeline";
    // 暂停和预览拖动都不允许沿旧播放速度外推。
    // 时间与窗口有限只是必要条件，还要确认缓存区间确实线性。
    // 跨越速度或跳跃边界时由下一份逻辑快照给出精确结果。
    const bool canUsePlaybackInterpolation =
        snapshot->isPlaying && !snapshot->isPreviewDragging &&
        supportsUiPlaybackInterpolation && std::isfinite(renderTime) &&
        std::isfinite(interpolationDuration) &&
        cache->canInterpolateLinearly(renderTime, interpolationDuration);
    if ( canUsePlaybackInterpolation ) {
        // 滚动速度乘播放倍率，得到绝对滚动坐标每秒变化量。
        // 不在 UI 侧重新查询时间线或访问逻辑 registry。
        const double interpolationSpeed =
            cache->getSpeedAt(renderTime) * snapshot->playbackSpeed;
        double interpolationYOffsetScale = 1.0;
        if ( cameraId == "Timeline" ) {
            // Timeline 另带 HS 屏幕比例，和绝对滚动速度分别发布。
            // 坏的 HS 先回退单位值，避免把非有限量传给绘制线程。
            interpolationYOffsetScale = cache->getHsAt(renderTime);
            if ( !std::isfinite(interpolationYOffsetScale) ) {
                interpolationYOffsetScale = 1.0;
            }
        }
        // 最终再次验证计算结果，防止两个有限输入相乘溢出。
        // 禁用补间时速度置零、偏移比例置一，不遗留上帧参数。
        snapshot->allowUiPlaybackInterpolation =
            std::isfinite(interpolationSpeed) &&
            std::isfinite(interpolationYOffsetScale);
        snapshot->uiInterpolationAbsYSpeed =
            snapshot->allowUiPlaybackInterpolation ? interpolationSpeed : 0.0;
        snapshot->uiInterpolationYOffsetScale =
            snapshot->allowUiPlaybackInterpolation ? interpolationYOffsetScale
                                                   : 1.0;
    } else {
        // 快照缓冲会复用，失败分支必须显式清除上一帧的允许标志。
        // 不能仅跳过赋值，否则暂停后仍可能继续视觉滚动。
        snapshot->allowUiPlaybackInterpolation = false;
        snapshot->uiInterpolationAbsYSpeed     = 0.0;
        snapshot->uiInterpolationYOffsetScale  = 1.0;
    }

    // Timeline 右键创建事件与主画布 Move 工具空白拖动需要完整映射；
    // 普通播放快照只携带线性补间速度。
    if ( cameraId == "Timeline" ||
         (isMainCanvas && snapshot->acceptsInteraction &&
          snapshot->currentTool == EditTool::Move) ) {
        // 交互视图需要逆映射任意位置，线性速度不足以处理整张时间线。
        // 只在指定视图或 Move 工具交互时复制，普通播放避免携带完整段表。
        // 复制的是动画后的映射，与当前显示缩放保持一致。
        cache->copyAnimatedSegmentsTo(snapshot->scrollSegments);
    }

    // 批处理器借用快照中的几何容器。
    // 布局输出统一在栈上初始化，空谱面分支可保留零范围。
    Batcher batcher(snapshot);
    float   leftX = 0, rightX = 0, topY = 0, bottomY = 0, trackAreaW = 0,
            singleTrackW = 0;
    float   renderScaleY = 1.0f;

    // 第一阶段：静态布局与打击特效预生成。
    // 打击特效顶点不随谱面滚动，因此在静态顶点边界前生成，
    // 绘制命令再按皮肤布局模式插入对应覆盖层。
    // 先记住命令起点，之后只提取本次生成的特效命令。
    // 顶点仍留在原缓冲位置，重新插入命令不会改变索引所指几何。
    uint32_t fxCmdStart = static_cast<uint32_t>(snapshot->cmds.size());
    if ( hitFXSystem && trackCount > 0 &&
         (isMainCanvas || cameraId == "Preview") ) {
        // 提前计算轨道参数
        // 特效要在静态边界之前生成，因此先独立计算其轨道投影。
        // 这里的临时投影不替代后面供音符使用的布局输出。
        float tempLX = 0, tempRX = 0, tempTY = 0, tempBY = viewportHeight;
        if ( isMainCanvas ) {
            // 主画布轨道允许水平移动，特效必须使用同一平移量。
            // 上下边界按归一化布局转换为当前视口像素。
            const auto projection = calculatePlayerTrackProjection(
                viewportWidth,
                trackCount,
                config.visual.trackLayout.left,
                config.visual.trackLayout.right,
                snapshot->canvasHorizontalOffsetX);
            tempLX = projection.leftX;
            tempRX = projection.rightX;
            tempTY = viewportHeight * config.visual.trackLayout.top;
            tempBY = viewportHeight * config.visual.trackLayout.bottom;
        } else {
            tempLX = config.visual.previewConfig.margin.left;
            tempRX = viewportWidth - config.visual.previewConfig.margin.right;
            tempTY = config.visual.previewConfig.margin.top;
            tempBY = viewportHeight - config.visual.previewConfig.margin.bottom;
        }
        // 外层已要求正轨数，避免在此分母为零。
        // Preview 与主画布边距来源不同，但都按各自可绘制宽度均分。
        float tempSTW = (tempRX - tempLX) / static_cast<float>(trackCount);

        // 特效使用 renderTime，使预览拖动的目标位置与主画布即时一致。
        // 这一调用只负责玩家区域，草稿区域另用独立投影。
        hitFXSystem->generateSnapshot(batcher,
                                      renderTime,
                                      config,
                                      trackCount,
                                      judgmentLineY,
                                      tempLX,
                                      tempTY,
                                      tempBY,
                                      tempSTW);
        // 专业模式下草稿可能有独立轨数与宽度。
        // 不按玩家轨宽向左简单延伸，否则自定义布局后特效会错位。
        if ( isMainCanvas && config.settings.professionalMode ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing,
                                              true,
                                              draftTrackCount,
                                              true);
            // 实际草稿轨数由投影结果决定。
            // 特效区域身份通过最后的草稿标志传入，避免套用玩家轨号。
            const auto visibleDraftTrackCount =
                static_cast<std::int32_t>(laneProjection.draftLaneCount);
            // 草稿打击特效与草稿背景共用独立的 X 和单轨宽度。
            hitFXSystem->generateSnapshot(batcher,
                                          renderTime,
                                          config,
                                          visibleDraftTrackCount,
                                          judgmentLineY,
                                          laneProjection.draftLeftX,
                                          tempTY,
                                          tempBY,
                                          laneProjection.draftLaneWidth,
                                          true);
        }
    }
    uint32_t fxCmdEnd = static_cast<uint32_t>(snapshot->cmds.size());

    // 提取并暂存打击特效命令
    // 延迟的是绘制顺序，不延迟本地状态计算。
    // 命令副本保存已有顶点索引，无须复制几何。
    // 空范围保持空列表，后续两个插入点均可跳过。
    std::vector<Common::Render::CanvasDrawCmd> deferredHitCmds;
    if ( fxCmdEnd > fxCmdStart ) {
        // 先保存后移除原位置的命令，避免特效在布局前后重复绘制。
        // 移除命令不能同步移除顶点，否则保存的索引将失效。
        deferredHitCmds.assign(snapshot->cmds.begin() + fxCmdStart,
                               snapshot->cmds.end());
        snapshot->cmds.erase(snapshot->cmds.begin() + fxCmdStart,
                             snapshot->cmds.end());
    }

    // 正常生成基础布局
    // Timeline 的背景和标记使用全视口裁剪。
    // 它在专用函数内设置静态与动态边界，外层不重复覆盖。
    if ( cameraId == "Timeline" ) {
        batcher.setScissor(0, 0, viewportWidth, viewportHeight);
        NoteRenderSystem::generateTimelineSnapshot(snapshot,
                                                   bpmEvents,
                                                   batcher,
                                                   renderTime,
                                                   viewportWidth,
                                                   viewportHeight,
                                                   judgmentLineY,
                                                   config,
                                                   cache);
        // Preview 使用像素边距，不按主画布的归一化布局切分。
        // 生成布局后输出纵向压缩比，后面的拍线与音符共用该比例。
    } else if ( cameraId == "Preview" ) {
        float lx = config.visual.previewConfig.margin.left;
        float rx = viewportWidth - config.visual.previewConfig.margin.right;
        float ty = config.visual.previewConfig.margin.top;
        float by = viewportHeight - config.visual.previewConfig.margin.bottom;
        batcher.setScissor(lx, ty, rx - lx, by - ty);

        NoteRenderSystem::generatePreviewSnapshot(snapshot,
                                                  batcher,
                                                  renderTime,
                                                  viewportWidth,
                                                  viewportHeight,
                                                  judgmentLineY,
                                                  trackCount,
                                                  config,
                                                  mainViewportHeight,
                                                  leftX,
                                                  rightX,
                                                  topY,
                                                  bottomY,
                                                  trackAreaW,
                                                  singleTrackW,
                                                  renderScaleY);
    } else {
        batcher.setScissor(0, 0, viewportWidth, viewportHeight);
        // 主画布以未压缩滚动距离绘制。
        // 预览布局返回的比例不能泄漏到另一种视图。
        renderScaleY = 1.0f;

        NoteRenderSystem::generateMainCanvasSnapshot(registry,
                                                     timelineRegistry,
                                                     snapshot,
                                                     batcher,
                                                     renderTime,
                                                     viewportWidth,
                                                     viewportHeight,
                                                     judgmentLineY,
                                                     trackCount,
                                                     config,
                                                     bgmTrackCount,
                                                     draftTrackCount,
                                                     cache,
                                                     leftX,
                                                     rightX,
                                                     topY,
                                                     bottomY,
                                                     trackAreaW,
                                                     singleTrackW,
                                                     renderScaleY);

        // 只有实际悬浮或预览拖动时才更新拍号提示。
        // 拖动目标时间优先于旧悬浮时间，使提示与正在显示的位置一致。
        if ( snapshot->isHoveringCanvas || snapshot->isPreviewDragging ) {
            double hoveredTime         = snapshot->isPreviewDragging
                                             ? snapshot->previewHoverTime
                                             : snapshot->hoveredTime;
            snapshot->hoveredBeatIndex = SessionUtils::calculateBeatIndex(
                hoveredTime, bpmEvents, snapshot->fallbackBpm);
        }
    }

    // 整轨光效属于轨道覆盖层：在静态布局之后、拍线和物件之前绘制，
    // 避免半透明渐变覆盖物件本身；固定尺寸特效继续在第三阶段置顶。
    if ( Config::SkinManager::instance().getHitEffectLayoutMode() ==
             Config::HitEffectLayoutMode::TrackFill &&
         !deferredHitCmds.empty() ) {
        // 先结束未提交的布局批次，再插入已有特效命令。
        // 否则同一批的后续几何可能在命令层越过覆盖层。
        // 插入后清空暂存列表，避免第三阶段再次置顶。
        batcher.flush();
        snapshot->cmds.insert(snapshot->cmds.end(),
                              deferredHitCmds.begin(),
                              deferredHitCmds.end());
        deferredHitCmds.clear();
    }

    // 记录静态边界 (此时 snapshot->vertices 包含了特效和布局的顶点)
    if ( cameraId != "Timeline" ) {
        snapshot->staticVertexCount =
            static_cast<uint32_t>(snapshot->vertices.size());
        snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());
    }

    // 第二阶段：生成随时间滚动的拍线与物件。
    // 这些内容会受到 UI 线程 yOffset 补偿的影响，从而消除亚帧抖动
    // 几何输出与布局元信息同时进入同一快照。
    // 消费端不应跨会话读取另一份配置推导这些比例。
    snapshot->trackCount   = trackCount;
    snapshot->renderScaleY = renderScaleY;

    if ( cameraId != "Timeline" ) {
        batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
        // 先绘制拍线，使其在物件下方
        // 主画布显示模式控制拍线基础可见性。
        // Timing 线独立于拍线，默认仅由预览分支启用。
        // 近光标渐隐依赖精确编辑指针，不能直接用于缩略预览。
        const bool beatLinesHidden       = config.visual.beatLineDisplayMode ==
                                           Config::BeatLineDisplayMode::Hidden;
        bool       shouldDrawBeatLines   = !beatLinesHidden;
        bool       shouldDrawTimingLines = false;
        bool       revealBeatLinesNearCursor =
            config.visual.beatLineDisplayMode ==
            Config::BeatLineDisplayMode::NearCursor;

        if ( cameraId == "Preview" ) {
            // 预览区保留自身开关；自动渐隐只应用到具备精确编辑光标的主画布。
            // 全局隐藏优先于 Preview 自己的开关。
            // 预览的 Timing 线开关仍独立生效，不与拍线显示绑定。
            shouldDrawBeatLines =
                !beatLinesHidden && config.visual.previewConfig.drawBeatLines;
            revealBeatLinesNearCursor = false;
            shouldDrawTimingLines = config.visual.previewConfig.drawTimingLines;
        }

        // 先绘制玩家区拍线，随后再绘制低透明度辅助区域。
        // 主画布标志控制主区域专属信息，辅助区不重复绘制它。
        if ( shouldDrawBeatLines ) {
            NoteRenderSystem::drawBeatLines(batcher,
                                            viewportHeight,
                                            judgmentLineY,
                                            config,
                                            bpmEvents,
                                            renderTime,
                                            cache,
                                            leftX,
                                            topY,
                                            bottomY,
                                            trackAreaW,
                                            renderScaleY,
                                            revealBeatLinesNearCursor,
                                            1.0F,
                                            isMainCanvas);
        }

        if ( isMainCanvas && shouldDrawBeatLines ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing,
                                              config.settings.professionalMode,
                                              draftTrackCount,
                                              true);
            // 草稿几何可能部分移出视口，裁剪仅取其可见交集。
            // 投影原点仍保留真实位置，不能用裁剪边缘改写轨道坐标。
            const float visibleDraftLeft =
                std::max(0.0F, laneProjection.draftLeftX);
            const float visibleDraftRight =
                std::min(viewportWidth, laneProjection.draftRightX);
            // 空交集不提交裁剪和拍线。
            // 负宽度不能作为“不可见”的隐式标记传给批处理器。
            if ( visibleDraftRight > visibleDraftLeft ) {
                batcher.setScissor(visibleDraftLeft,
                                   topY,
                                   visibleDraftRight - visibleDraftLeft,
                                   bottomY - topY);
                NoteRenderSystem::drawBeatLines(
                    batcher,
                    viewportHeight,
                    judgmentLineY,
                    config,
                    bpmEvents,
                    renderTime,
                    cache,
                    visibleDraftLeft,
                    topY,
                    bottomY,
                    visibleDraftRight - visibleDraftLeft,
                    renderScaleY,
                    revealBeatLinesNearCursor,
                    0.42F,
                    false);
            }
            // 此助手只负责一个连续可见区域。
            // 批注与 BGM 的合并策略由外层决定，避免跨空白区域连接拍线。
            /// @brief 在辅助区域的可见交集内追加低透明度拍线。
            /// @param regionLeft 区域真实左边界。
            /// @param regionRight 区域真实右边界。
            /// @warning 主画布快照热路径，会改变批处理裁剪状态。
            const auto drawAuxiliaryBeatLineRegion = [&](float regionLeft,
                                                         float regionRight) {
                // 先和全视口相交，辅助区域完全移出时立即结束。
                // 纵向范围保持和玩家区一致，确保同时间拍线对齐。
                const float visibleLeft  = std::max(0.0F, regionLeft);
                const float visibleRight = std::min(viewportWidth, regionRight);
                if ( visibleRight <= visibleLeft ) return;
                batcher.setScissor(visibleLeft,
                                   topY,
                                   visibleRight - visibleLeft,
                                   bottomY - topY);
                NoteRenderSystem::drawBeatLines(batcher,
                                                viewportHeight,
                                                judgmentLineY,
                                                config,
                                                bpmEvents,
                                                renderTime,
                                                cache,
                                                visibleLeft,
                                                topY,
                                                bottomY,
                                                visibleRight - visibleLeft,
                                                renderScaleY,
                                                revealBeatLinesNearCursor,
                                                0.28F,
                                                false);
            };
            // 批注与 BGM 相交时绘制一次并集，分离时分别绘制，避免拍线穿越空隙。
            // 使用严格相交判断，边缘刚好相接也可分区绘制。
            // 重叠时取并集，避免半透明拍线叠画后局部变亮。
            const bool auxiliaryRegionsOverlap =
                laneProjection.annotationLeftX < laneProjection.bgmRightX &&
                laneProjection.bgmLeftX < laneProjection.annotationRightX;
            if ( auxiliaryRegionsOverlap ) {
                drawAuxiliaryBeatLineRegion(
                    std::min(laneProjection.annotationLeftX,
                             laneProjection.bgmLeftX),
                    std::max(laneProjection.annotationRightX,
                             laneProjection.bgmRightX));
            } else {
                drawAuxiliaryBeatLineRegion(laneProjection.annotationLeftX,
                                            laneProjection.annotationRightX);
                drawAuxiliaryBeatLineRegion(laneProjection.bgmLeftX,
                                            laneProjection.bgmRightX);
            }
            // 辅助区调用会修改批处理器状态，结束后恢复玩家区裁剪。
            // 后续 Timing 线不能继承最后一个辅助区域的狭窄裁剪。
            batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
        }

        // Timing 线在拍线之后、物件之前。
        // 它沿用当前缓存时间映射，不扫描原始 Timing 实体。
        if ( shouldDrawTimingLines ) {
            NoteRenderSystem::drawTimingLines(batcher,
                                              viewportHeight,
                                              judgmentLineY,
                                              config,
                                              renderTime,
                                              cache,
                                              leftX,
                                              topY,
                                              bottomY,
                                              trackAreaW,
                                              renderScaleY);
        }

        // 将几何右边界与视口裁剪右边界分开保存。
        // 前者保留布局真实范围，后者必须被限制在可见画布内。
        float noteRenderClipLeftX  = leftX;
        float noteRenderClipRightX = rightX;
        float noteRenderRightX     = rightX;
        // 投影值在栈上覆盖整个 renderNotes 调用。
        // 传出的指针只用于本次借用，不保存到跨线程快照中。
        CanvasLaneProjection        noteLaneProjection;
        const CanvasLaneProjection* noteLaneProjectionPtr = nullptr;
        if ( isMainCanvas ) {
            noteLaneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing,
                                              config.settings.professionalMode,
                                              draftTrackCount,
                                              true);
            // 有投影时，音符生成可使用独立辅助区域布局。
            // Preview 继续传空指针，沿用简化的统一轨道布局。
            noteLaneProjectionPtr    = &noteLaneProjection;
            const auto contentBounds = noteLaneProjection.contentBounds();
            // 草稿与 BGM 区域
            // 可被移动到任意一侧，音符与遮罩裁剪统一覆盖真实外包范围。
            noteRenderClipLeftX =
                std::clamp(contentBounds.leftX, 0.0F, viewportWidth);
            noteRenderClipRightX =
                std::clamp(contentBounds.rightX, 0.0F, viewportWidth);
            // 真实右边界不钳位，避免把局部滚出视口的物件压缩到边缘。
            // GPU 裁剪放宽到画布，再由音符生成使用真实区域边界。
            noteRenderRightX = contentBounds.rightX;
            batcher.setScissor(0.0F, topY, viewportWidth, bottomY - topY);
        } else {
            batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
        }
        // 拍线已经生成，此时追加物件保证正常覆盖关系。
        // 绘制位置、遮罩范围和交互数据必须来自同一次投影。
        NoteRenderSystem::renderNotes(registry,
                                      snapshot,
                                      cameraId,
                                      renderTime,
                                      judgmentLineY,
                                      trackCount,
                                      config,
                                      batcher,
                                      leftX,
                                      noteRenderClipLeftX,
                                      noteRenderClipRightX,
                                      noteRenderRightX,
                                      topY,
                                      bottomY,
                                      singleTrackW,
                                      renderScaleY,
                                      noteLaneProjectionPtr);
        if ( isMainCanvas ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing,
                                              config.settings.professionalMode,
                                              draftTrackCount,
                                              true);
            // 自动采样只在主画布展示，不混入缩略 Preview。
            // 已排序实体与最大结束时间前缀供可见区筛选使用。
            // 不能在这个每帧调用点重新建立全部索引。
            SampleRenderSystem::renderSamples(sampleRegistry,
                                              sortedSampleEntities,
                                              sortedSampleMaxEndPrefix,
                                              snapshot,
                                              batcher,
                                              laneProjection,
                                              cache,
                                              config,
                                              renderTime,
                                              judgmentLineY,
                                              viewportWidth,
                                              topY,
                                              bottomY,
                                              renderScaleY);
        }
        if ( cameraId == "Preview" ) {
            float lx = config.visual.previewConfig.margin.left;
            float rx = viewportWidth - config.visual.previewConfig.margin.right;
            float ty = config.visual.previewConfig.margin.top;
            float by =
                viewportHeight - config.visual.previewConfig.margin.bottom;
            batcher.setScissor(lx, ty, rx - lx, by - ty);
        } else if ( isMainCanvas ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing,
                                              config.settings.professionalMode,
                                              draftTrackCount,
                                              true);
            const auto  contentBounds = laneProjection.contentBounds();
            const float clipLeft      = std::max(0.0F, contentBounds.leftX);
            const float clipRight =
                std::min(viewportWidth, contentBounds.rightX);
            // 水平裁剪覆盖全部真实内容区域。
            // 纵向额外留出余量，使跨越上下边界的调试线框仍可显示。
            // 宽度钳为非负，完全滚出时形成空区域。
            batcher.setScissor(clipLeft,
                               -viewportHeight * 0.5F,
                               std::max(0.0F, clipRight - clipLeft),
                               viewportHeight * 2.0F);
        } else {
            batcher.setScissor(leftX,
                               -viewportHeight * 0.5F,
                               trackAreaW,
                               viewportHeight * 2.0F);
        }
        // 调试框在音符和采样的交互数据生成后绘制。
        // 只读快照命中框，不能为调试再次计算另一套拾取结果。
        if ( isMainCanvas && config.visual.debugDrawHitboxes ) {
            NoteRenderSystem::debugRenderHitboxes(batcher, snapshot);
        }
    }

    // 框选端点仍是时间与轨道坐标。
    // 到这里才按当前滚动缓存投影，播放或缩放时不会固定在旧像素位置。
    for ( const auto& box : snapshot->marqueeBoxes ) {
        NoteRenderSystem::renderMarqueeBox(batcher,
                                           box,
                                           judgmentLineY,
                                           leftX,
                                           singleTrackW,
                                           renderScaleY,
                                           cache,
                                           renderTime,
                                           viewportWidth,
                                           viewportHeight);
    }

    // 几何先后位置决定 UI 只对哪一段顶点做滚动补偿。
    // 框选也随时间投影，包含在动态范围里。
    // 记录动态顶点数量
    if ( cameraId != "Timeline" ) {
        snapshot->dynamicVertexCount =
            static_cast<uint32_t>(snapshot->vertices.size()) -
            snapshot->staticVertexCount;
    }

    // 第三阶段：追加置顶覆盖层。
    // 将之前生成的打击特效命令插入到最后，使其绘制在物件上方
    // 剩余列表只包含需要置顶的固定尺寸特效。
    // 其顶点早已在静态区，命令后移不会使其随播放补间滚动。
    if ( !deferredHitCmds.empty() ) {
        snapshot->cmds.insert(snapshot->cmds.end(),
                              deferredHitCmds.begin(),
                              deferredHitCmds.end());
    }

    // 预览区包围盒位于动态顶点范围之后，保持屏幕位置并覆盖物件。
    if ( cameraId == "Preview" ) {
        auto& skin       = Config::SkinManager::instance();
        auto  boxCol     = skin.getColor("preview.boundingbox");
        bool  isDragging = snapshot->isPreviewDragging;

        // 包围盒表示主画布轨道的有效高度，不是整个窗口高度。
        // 通过预览纵向比例映射，边距与主画布布局变化会自然影响框高。
        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float boxDrawH = mainEffectiveH * renderScaleY;

        // 1. [展示中] 始终绘制当前主视窗位置的包围盒 (除非正在拖拽)
        // 拖动期间仅显示目标框，避免同时显示旧位置与新位置造成歧义。
        // 非拖动态仍持续标示当前主画布可见范围。
        if ( !isDragging ) {
            // 判定线通常不在有效区域正中，框底需加上对应底部距离。
            // 不能直接用框高的一半代替这个非对称偏移。
            float boxBottom =
                judgmentLineY + (config.visual.trackLayout.bottom -
                                 config.visual.judgeline_pos) *
                                    mainViewportHeight * renderScaleY;

            batcher.setTexture(TextureID::None);
            batcher.pushQuad(leftX,
                             boxBottom,
                             trackAreaW,
                             boxDrawH,
                             { boxCol.r, boxCol.g, boxCol.b, boxCol.a });
            batcher.pushStrokeRect(leftX,
                                   boxBottom - boxDrawH,
                                   rightX,
                                   boxBottom,
                                   2.0f,
                                   { boxCol.r, boxCol.g, boxCol.b, 1.0f });
        }

        // 2. [悬浮中/拖拽中] 绘制参考包围盒
        // 悬浮和拖动态共用目标范围投影。
        // 坐标来自快照交互状态，不在逻辑绘制中读取 UI 鼠标 API。
        if ( snapshot->isPreviewHovered || isDragging ) {
            float hoverBoxBottom =
                snapshot->previewHoverY + (config.visual.trackLayout.bottom -
                                           config.visual.judgeline_pos) *
                                              mainViewportHeight * renderScaleY;

            // 旧皮肤可能没有悬浮框颜色，沿用淡黄色回退。
            // 这里保留既有 RGB 哨兵规则，不改变皮肤兼容行为。
            auto hoverBoxCol = skin.getColor("preview.hoverbox");
            if ( hoverBoxCol.r == 1.0f && hoverBoxCol.g == 0.0f &&
                 hoverBoxCol.b == 1.0f ) {
                hoverBoxCol = { 1.0f, 1.0f, 0.6f, 0.3f };
            }
            // 拖动态增强填充可见度，上限保持一。
            // 仅改变局部颜色值，不回写共享皮肤配置。
            if ( isDragging ) {
                hoverBoxCol.a = std::min(1.0f, hoverBoxCol.a * 2.0f);
            }

            batcher.setTexture(TextureID::None);
            batcher.pushQuad(
                leftX,
                hoverBoxBottom,
                trackAreaW,
                boxDrawH,
                { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, hoverBoxCol.a });
            batcher.pushStrokeRect(
                leftX,
                hoverBoxBottom - boxDrawH,
                rightX,
                hoverBoxBottom,
                2.0f,
                { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, 0.8f });

            // 在鼠标位置绘制临时的判定线预览
            batcher.pushQuad(
                leftX,
                snapshot->previewHoverY + 2.0f * 0.5f,
                trackAreaW,
                2.0f,
                { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, 0.6f });
        }

        // 3. 绘制预览区判定框 (最上层静态)
        // 先提交悬浮覆盖层，再追加判定框。
        // 判定框位置不随目标悬浮时间移动，用于区分当前判定基准。
        batcher.flush();
        drawJudgmentGuideBox(batcher, leftX, judgmentLineY, trackAreaW, 18.0f);
    }

    batcher.flush();

    if ( isMainCanvas ) {
        // 组件上下文只借用本帧缓存和 BPM 列表。
        // 特效系统不存在时提供空 KPS 视图，避免临时构造虚假逐轨计数。
        // 组件在普通几何提交后绘制，拥有独立的覆盖层顺序。
        const CanvasComponentRenderContext componentContext{
            .currentTime    = renderTime,
            .viewportWidth  = viewportWidth,
            .viewportHeight = viewportHeight,
            .judgmentLineY  = judgmentLineY,
            .visibleTop     = topY,
            .visibleBottom  = bottomY,
            .renderScaleY   = renderScaleY,
            .beatDivisor    = config.settings.beatDivisor,
            .trackCount     = trackCount,
            .trackLeft      = config.visual.trackLayout.left,
            .trackRight     = config.visual.trackLayout.right,
            .trackKps       = hitFXSystem ? hitFXSystem->trackKps()
                                          : std::span<const std::uint32_t>{},
            .bpmEvents      = bpmEvents,
            .scrollCache    = cache,
        };
        CanvasComponentRenderSystem::render(
            snapshot, componentContext, config.visual.canvasComponents);
    }
}

/// @brief 将框选的轨道与时间范围投影成屏幕矩形。
/// @param batcher 接收无纹理圆角几何的批处理器。
/// @param box 两端点保持原始拖动方向的框选快照。
/// @param judgmentLineY 屏幕判定线位置。
/// @param leftX 玩家轨道原点。
/// @param singleTrackW 单轨像素宽度。
/// @param renderScaleY 当前画布的纵向压缩比例。
/// @param cache 借用滚动缓存，用于时间到绝对滚动位置的转换。
/// @param renderTime 当前视觉时间，单位为秒。
/// @param viewportWidth 全画布裁剪宽度。
/// @param viewportHeight 全画布裁剪高度。
/// @pre cache 非空且生成期间保持有效。
/// @note 该入口按统一单轨宽度换算横坐标，不接收独立区域投影。
/// @note 小于一像素的矩形不绘制，不修改框选的逻辑选中结果。
/// @warning 每个活动框选随快照调用；禁止访问文件、等待输入或修改 registry。
void NoteRenderSystem::renderMarqueeBox(
    Batcher& batcher, const RenderSnapshot::MarqueeBoxSnapshot& box,
    float judgmentLineY, float leftX, float singleTrackW, float renderScaleY,
    const ScrollCache* cache, double renderTime, float viewportWidth,
    float viewportHeight)
{
    // 横向端点允许逆向拖动，先保留端点方向。
    // 排序留到两个坐标都完成换算之后，避免丢失时间端点含义。
    float x1 = leftX + box.startTrack * singleTrackW;
    float x2 = leftX + box.endTrack * singleTrackW;

    // 先在双精度绝对滚动坐标中计算差值。
    // 只在投影到屏幕时转成 float，减少长谱面大时间值造成的精度损失。
    double currentAbsY = cache->getAbsY(renderTime);
    double startAbsY   = cache->getAbsY(box.startTime);
    double endAbsY     = cache->getAbsY(box.endTime);

    float y1 = judgmentLineY -
               static_cast<float>(startAbsY - currentAbsY) * renderScaleY;
    float y2 = judgmentLineY -
               static_cast<float>(endAbsY - currentAbsY) * renderScaleY;

    // 屏幕上下方向可能和时间递增相反。
    // 分别归一化水平与垂直两端，四种拖动方向都得到正尺寸矩形。
    float left   = std::min(x1, x2);
    float right  = std::max(x1, x2);
    float top    = std::min(y1, y2);
    float bottom = std::max(y1, y2);
    float w      = right - left;
    float h      = bottom - top;

    // 退化框不绘制，但其逻辑选择结果由交互控制器处理。
    // 渲染入口不借此取消手势或更改选中实体。
    if ( w < 1.0f || h < 1.0f ) return;

    // 仅查询内存中的选框样式。
    // 描边宽度和圆角由用户全局编辑设置决定，不随音符皮肤纹理缩放。
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    float borderW  = settings.marqueeThickness;
    float cornerR  = settings.marqueeRounding;

    // 重置 scissor 到全屏，确保框选矩形不被轨道裁剪掉
    batcher.setScissor(0, 0, viewportWidth, viewportHeight);
    batcher.setTexture(TextureID::None);

    // 绘制半透明填充
    glm::vec4 fillCol   = { 0.2f, 0.6f, 1.0f, 0.15f };
    glm::vec4 strokeCol = { 0.3f, 0.7f, 1.0f, 0.85f };

    // 填充先于描边，边框不会被半透明内部覆盖。
    // 底边坐标与高匹配批处理接口，和之前归一化的上下边界保持一致。
    batcher.pushRoundedQuad(left, bottom, w, h, cornerR, fillCol);
    batcher.pushRoundedStrokeRect(
        left, bottom, w, h, cornerR, borderW, strokeCol);
}

/// @brief 根据会话配置生成时间线快照，与其他画布共享专业模式状态。
/// @warning 逻辑渲染热路径：只读取传入的配置快照和已缓存的时间线数据。
/// @param snapshot 接收几何及 Timing 交互元素的快照。
/// @param bpmEvents 已排序的 BPM 观察指针，供分拍线生成使用。
/// @param batcher 沿用调用方设置的全时间线裁剪。
/// @param currentTime 视觉时间，单位为秒。
/// @param viewportWidth Timeline 像素宽度。
/// @param viewportHeight Timeline 像素高度。
/// @param judgmentLineY 时间线判定框中心位置。
/// @param config 与所属会话一致的编辑器配置。
/// @param cache 已构建的滚动段缓存，包含缩放动画与 HS 信息。
/// @pre snapshot 与 cache 非空，BPM 指针在本次调用期间有效。
/// @note 普通模式将同段多个效果合在一个标记，专业模式分别放入四条轨道。
/// @note 像素行占用只抑制标记几何，交互事件仍会写入快照。
/// @note 判定框在动态顶点边界之后生成，避免随播放补间滚动。
void NoteRenderSystem::generateTimelineSnapshot(
    RenderSnapshot*                              snapshot,
    const std::vector<const TimelineComponent*>& bpmEvents, Batcher& batcher,
    double currentTime, float viewportWidth, float viewportHeight,
    float judgmentLineY, const Config::EditorConfig& config,
    const ScrollCache* cache)
{
    // 空会话没有可解释的 Timing 数据，保持已有快照内容并返回。
    // 背景与交互元素都只在谱面存在时追加。
    if ( !snapshot->hasBeatmap ) return;

    batcher.setTexture(TextureID::None);

    // 绘制背景 (确保全覆盖，消除透明混合带来的边缘可疑像素)
    batcher.pushQuad(
        0, viewportHeight, viewportWidth, viewportHeight, { 0, 0, 0, 0.01f });

    auto&      skin             = Config::SkinManager::instance();
    auto       tickCol          = skin.getColor("timeline.tick");
    const bool professionalMode = config.settings.professionalMode;

    // 使用视觉锚点绝对位置，保持缩放动画和当前时间对齐。
    // 后续 HS 变换针对每个事件自己的段，不在此统一折入锚点。
    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);

    // 普通模式左右保留提示空间。
    // 极窄窗口至少给出一像素标记宽度，避免负几何尺寸。
    float paddingX = 30.0f;
    float lineW    = std::max(1.0f, viewportWidth - paddingX * 2.0f);

    if ( professionalMode ) {
        // 颜色顺序与 professionalTimelineLane 的类型映射保持一致。
        // 底色较淡，避免盖过后续标记与拍线。
        // 这里是固定四类效果，不能用当前事件数作为轨道数量。
        constexpr glm::vec4 laneColors[PROFESSIONAL_TIMELINE_LANE_COUNT] = {
            { 1.0f, 0.28f, 0.28f, 0.20f },
            { 0.28f, 1.0f, 0.38f, 0.18f },
            { 0.34f, 0.55f, 1.0f, 0.18f },
            { 1.0f, 0.87f, 0.28f, 0.18f },
        };
        const float laneWidth =
            viewportWidth /
            static_cast<float>(PROFESSIONAL_TIMELINE_LANE_COUNT);
        batcher.setTexture(TextureID::None);
        for ( int lane = 0; lane < PROFESSIONAL_TIMELINE_LANE_COUNT; ++lane ) {
            // 按同一单轨宽度确定原点，避免逐轨累加浮点误差。
            // 最后一轨使用视口剩余宽度，让右边界精确落在画布末端。
            const float laneX = laneWidth * static_cast<float>(lane);
            batcher.pushQuad(laneX,
                             viewportHeight,
                             lane == PROFESSIONAL_TIMELINE_LANE_COUNT - 1
                                 ? viewportWidth - laneX
                                 : laneWidth,
                             viewportHeight,
                             laneColors[lane]);
            // 只画内部边界，不在最左端重复叠加外边线。
            // 分隔线属于静态背景，不跟随当前播放位置平移。
            if ( lane > 0 ) {
                batcher.pushQuad(laneX,
                                 viewportHeight,
                                 1.0f,
                                 viewportHeight,
                                 { 1.0f, 1.0f, 1.0f, 0.16f });
            }
        }
    }

    // 轨道底色在动态范围之前写入。
    // 分拍线和 Timing 标记从接下来的顶点位置开始参与滚动补偿。
    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    // 皮肤键使用约分后的拍分母，避免同一节奏层级出现不同颜色。
    // 颜色缺省时宽度也转向默认键，保持旧皮肤的回退组合。
    /// @brief 查询约分后拍分母对应的颜色和线宽。
    /// @param denominator 拍内位置约分后的分母。
    /// @return 应用会话透明度后的颜色与皮肤线宽。
    /// @warning Timeline 热路径，保持内存内查询，不允许触发皮肤重载。
    auto getBeatLineConfig = [&](int denominator) {
        // 保护除数形成的配置键，非法分母按整拍层级处理。
        // 本助手不改变外层生成分拍线的实际步长。
        if ( denominator <= 0 ) denominator = 1;
        std::string   key = "beat_lines.beat_" + std::to_string(denominator);
        Config::Color c   = skin.getColor(key);
        // 只有完整不透明洋红哨兵表示颜色缺失。
        // 普通透明洋红仍是用户可指定的真实配色。
        if ( c.r == 1.0f && c.g == 0.0f && c.b == 1.0f && c.a == 1.0f ) {
            c   = skin.getColor("beat_lines.default");
            key = "beat_lines_width.default";
        } else {
            key = "beat_lines_width.beat_" + std::to_string(denominator);
        }

        float width =
            skin.getValue(key, skin.getValue("beat_lines_width.default", 2.0f));
        // 显式配色覆盖皮肤颜色，但线宽仍从皮肤获取。
        // 先将分母映射到有限槽位，不直接用分母下标访问调色板。
        if ( config.visual.overrideBeatLineColors ) {
            const auto& overrideColor =
                config.visual.beatLineColors[Config::beatLineColorPaletteSlot(
                    denominator)];
            c = { overrideColor[0],
                  overrideColor[1],
                  overrideColor[2],
                  overrideColor[3] };
        }
        // 整体拍线透明度乘在最终颜色上。
        // 返回颜色和线宽的值对象，不暴露皮肤表的内部引用。
        return std::pair{
            glm::vec4(c.r, c.g, c.b, c.a * config.visual.beatLineAlpha), width
        };
    };

    // 3. 绘制 Timeline 自身的分拍线
    /// @brief 绘制 Timeline 自身的分拍线；bpmEvents 由 SessionContext
    /// 脏标记缓存维护，避免热路径完整遍历和排序。
    int beatDivisor = config.settings.beatDivisor;
    // 无效设置回退四分拍，防止后面步长除零。
    // 不把这个渲染回退写回配置或持久化。
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    if ( !bpmEvents.empty() ) {
        // 把屏幕上下边界转换为绝对滚动窗口。
        // 窗口可对应多个时间区间，反向滚动不能只做一次单调逆映射。
        double topAbsY       = currentAbsY + judgmentLineY;
        double bottomAbsY    = currentAbsY + judgmentLineY - viewportHeight;
        auto   visibleRanges = cache->getTimeRangesForAbsYWindow(
            std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

        batcher.setTexture(TextureID::None);
        // BPM 列表来自会话缓存，本函数不从 registry 重建或排序。
        // 每个 BPM 只决定自身直到下一 BPM 的拍网格。
        for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
            const auto* currentBPM = bpmEvents[i];
            double      bpmTime    = currentBPM->m_timestamp;
            // 异常 BPM 按统一规则规范化，保证节拍时长可用于步进。
            // 回退值来自当前快照，不读取另一个会话的默认速度。
            const double bpmVal = ::MMM::normalizeBpmValue(
                currentBPM->m_value, snapshot->fallbackBpm);

            // 最后一段向未来延伸到无穷，真正绘制范围仍由可见区间限制。
            // BPM 分段右端不含下一事件，避免边界拍线被两段重复生成。
            double nextBpmTime  = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();
            double beatDuration = 60.0 / bpmVal;
            double stepDuration = beatDuration / beatDivisor;

            // 可见区间与 BPM 生效范围求交后再步进。
            // 跳过整段不可见内容，避免从歌曲开头逐拍走到当前窗口。
            for ( const auto& [startTime, endTime] : visibleRanges ) {
                // 段结束已经不晚于可见起点，无须计算任何拍线。
                // 等号归到下一 BPM 段，与后面严格小于 nextBpmTime 的规则一致。
                if ( nextBpmTime <= startTime ) continue;
                double segmentStartTime = bpmTime;
                // 只允许第一条 BPM 向前延长，后续段不能覆盖前一段节奏。
                // 开关关闭时，第一条 Timing 以前保持无拍线。
                if ( i == 0 && config.visual.drawBeatLinesBeforeFirstTiming ) {
                    segmentStartTime = startTime;
                }
                if ( segmentStartTime >= endTime ) continue;

                // 先求交集的最早候选时间，再计算相对 BPM 锚点的整数步数。
                // 不会因视口移动改变拍线网格的相位。
                double  startCalcTime = std::max(segmentStartTime, startTime);
                int64_t stepOffset    = 0;
                // 锚点之后向上取整，选择不早于可见起点的拍线。
                // 微小容差减少浮点误差把恰在边界上的线跳到下一格。
                if ( startCalcTime > bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::ceil(
                        (startCalcTime - bpmTime) / stepDuration - 1e-4));
                    // 允许首 BPM 向前补线时使用负步数。
                    // 向下取整给出候选，再由下一段循环推进到实际可见起点。
                } else if ( startCalcTime < bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::floor(
                        (startCalcTime - bpmTime) / stepDuration + 1e-4));
                }

                double t = bpmTime + stepOffset * stepDuration;
                // 校正整数换算后的下界误差，保证正式循环不从窗口前开始。
                // 这是有限数据步进，不等待时钟或跨线程状态。
                while ( t < startCalcTime - 1e-4 ) {
                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
                // 每一步都由整数偏移重新乘出时间，不连续累加浮点时长。
                // 同时限制在 BPM 生效段和当前可见区间内。
                while ( t < nextBpmTime && t <= endTime ) {
                    int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                    // C++ 负余数仍为负，先转回零到 beatDivisor 之间的拍内位置。
                    // 这样首 BPM 以前的拍线使用同一套颜色层级。
                    if ( beatIndex < 0 ) beatIndex += beatDivisor;
                    int denominator = 1;
                    // 整数拍使用分母一，其余位置通过最大公约数约分。
                    // 例如八分网格中的中点应归为二分层级，而不是八分颜色。
                    if ( beatIndex != 0 ) {
                        int gcd     = std::gcd(beatIndex, beatDivisor);
                        denominator = beatDivisor / gcd;
                    }

                    auto [color, width] = getBeatLineConfig(denominator);
                    // 用事件时间作为 HS 参考，保留该拍线所处段的显示映射。
                    // 只转换与当前锚点的差，避免直接把巨大绝对坐标压成 float。
                    float y = judgmentLineY -
                              static_cast<float>(
                                  cache->getDisplayDelta(t, currentAbsY, t));
                    // 逆映射窗口用于减少候选，最终仍按屏幕坐标确认可见性。
                    // 专业模式拍线跨整幅画布，普通模式只占中间内容区。
                    if ( y >= 0.0f && y <= viewportHeight ) {
                        color.a *= 0.75f;
                        const float beatLineX =
                            professionalMode ? 0.0f : paddingX;
                        const float beatLineW =
                            professionalMode ? viewportWidth : lineW;
                        batcher.setTexture(TextureID::None);
                        // 吸附提示以时间相等判断，不按整数像素行猜测命中。
                        // 多层宽线模拟柔光，使用相同中心以免高亮看起来偏离网格。
                        if ( snapshot->isSnapped &&
                             std::abs(t - snapshot->snappedTime) < 1e-6 ) {
                            glm::vec4 glowCol = color;
                            glowCol.a *= 0.6f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 4.0f) * 0.5f,
                                             beatLineW,
                                             width + 4.0f,
                                             glowCol);
                            // 外层更宽且更透明，逐层扩散而不改变实际拍线宽度。
                            // 光晕仅是几何叠加，不添加独立纹理资源。
                            glowCol.a *= 0.5f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 10.0f) * 0.5f,
                                             beatLineW,
                                             width + 10.0f,
                                             glowCol);
                            glowCol.a *= 0.5f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 20.0f) * 0.5f,
                                             beatLineW,
                                             width + 20.0f,
                                             glowCol);
                        }
                        batcher.pushQuad(beatLineX,
                                         y + width * 0.5f,
                                         beatLineW,
                                         width,
                                         color);
                    }

                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
            }
        }
    }

    // 5. 绘制 Timing 事件为普通 Note 形状。
    // Timing 标记使用独立尺寸策略，不直接沿用主画布单轨宽度。
    // 专业模式给左右边缘留缝，普通模式填满中央标记宽度。
    const float professionalLaneWidth =
        viewportWidth / static_cast<float>(PROFESSIONAL_TIMELINE_LANE_COUNT);
    float noteW =
        professionalMode ? std::max(1.0f, professionalLaneWidth - 2.0f) : lineW;
    // 缺少 Note 图集数据时保留固定长宽比回退。
    // 有纹理时用归一化 UV 的宽高比还原外观比例。
    float noteH = noteW * 0.36f;
    if ( auto uvIt =
             snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
         uvIt != snapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
        noteH = noteW * (uvIt->second.w / uvIt->second.z);
    }
    float noteX = paddingX;

    // 多保留末端一行，使 y 等于视口高度时仍可索引。
    // 占用表按屏幕像素行而不是时间量化，服务高密度标记绘制。
    int markerRows =
        std::max(1, static_cast<int>(std::ceil(viewportHeight)) + 1);
    // 专业模式四条轨分别去重，避免不同效果互相压掉。
    // 普通模式只有一条合成标记轨。
    const int markerLaneCount =
        professionalMode ? PROFESSIONAL_TIMELINE_LANE_COUNT : 1;
    std::vector<uint8_t> occupiedMarkerRows(
        static_cast<size_t>(markerRows * markerLaneCount), 0);
    // 只控制几何是否绘制，不删除时间线事件。
    // 相同像素行首次出现者保留标记，后续交互记录仍会存在。
    /// @brief 尝试占用当前轨道的一个可见像素行。
    /// @param lane 专业类型轨号或普通模式零号轨。
    /// @param y 标记中心的屏幕纵坐标。
    /// @warning 每个候选标记调用，禁止改为等待其他标记释放位置。
    auto occupyMarkerRow = [&](int lane, float y) {
        // 先拒绝视口外事件，再将像素行限制到分配范围。
        // 不能先钳位屏幕外坐标，否则不可见标记会抢占边缘行。
        if ( y < 0.0f || y > viewportHeight ) return false;
        int row = static_cast<int>(std::floor(y));
        row     = std::clamp(row, 0, markerRows - 1);
        lane    = std::clamp(lane, 0, markerLaneCount - 1);
        // 按轨道分块铺平占用表。
        // 同一行在不同专业轨道拥有不同槽位。
        const size_t index = static_cast<size_t>(lane * markerRows + row);
        if ( occupiedMarkerRows[index] != 0 ) {
            return false;
        }
        // 只有第一次占用返回成功。
        // 先占用再提交几何，使后续事件看到一致的抑制状态。
        occupiedMarkerRows[index] = 1;
        return true;
    };

    // 标记颜色与专业轨道类型保持一致。
    // 未知类型使用皮肤刻度色回退，不读额外资源。
    /// @brief 获取单个 Timing 类型的标记颜色。
    /// @param effect 当前效果类型。
    /// @return 固定类型色或皮肤刻度回退色。
    /// @warning 可见标记热路径，仅做枚举分支和颜色值构造。
    auto markerColorForEffect = [&](::MMM::TimingEffect effect) {
        switch ( effect ) {
        case ::MMM::TimingEffect::BPM:
            return glm::vec4{ 1.0f, 0.2f, 0.2f, 0.8f };
        case ::MMM::TimingEffect::SCROLL:
            return glm::vec4{ 0.2f, 1.0f, 0.2f, 0.8f };
        case ::MMM::TimingEffect::JUMP:
            return glm::vec4{ 0.2f, 0.45f, 1.0f, 0.8f };
        case ::MMM::TimingEffect::HS:
            return glm::vec4{ 1.0f, 0.85f, 0.2f, 0.8f };
        }
        return glm::vec4{ tickCol.r, tickCol.g, tickCol.b, 0.8f };
    };

    // 在追加几何后记录本次新增区间。
    // 长度用当前容器末端减去调用前偏移，兼容填充模式改变顶点数量。
    /// @brief 将刚追加的几何区间登记到指定标记槽。
    /// @param geometry 接收当前快照缓冲区间的槽位。
    /// @param markerVertexOffset 绘制前的顶点末端。
    /// @param markerIndexOffset 绘制前的索引末端。
    /// @warning 标记热路径，调用期间只写区间元数据，不复制几何。
    auto writeMarkerGeometry =
        [&](TimelineInteractiveElement::MarkerGeometry& geometry,
            uint32_t                                    markerVertexOffset,
            uint32_t                                    markerIndexOffset) {
            // 几何偏移属于当前快照的公共缓冲，不是独立 mesh 的局部偏移。
            // 调用前后不能插入其他标记，否则区间会包含无关几何。
            geometry.hasMarkerGeometry  = true;
            geometry.markerVertexOffset = markerVertexOffset;
            geometry.markerVertexCount  = static_cast<uint32_t>(
                snapshot->vertices.size() - markerVertexOffset);
            geometry.markerIndexOffset = markerIndexOffset;
            geometry.markerIndexCount  = static_cast<uint32_t>(
                snapshot->indices.size() - markerIndexOffset);
        };

    // 缓存段已合并相同时间的效果与实体信息。
    // 无效果段只用于滚动映射，不需要生成 Timing 交互元素。
    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.effects == 0 ) continue;

        // 段位置先应用动画缩放，再相对当前视觉锚点计算。
        // HS 只缩放显示距离，不改变交互记录里的原始事件时间。
        const double segmentAbsY = seg.absY * cache->getAnimatedZoomScale();
        float y = judgmentLineY -
                  static_cast<float>((segmentAbsY - currentAbsY) * seg.hs);

        // 先发布完整逻辑记录，再决定是否有可见标记几何。
        // 视口外或被像素行去重的事件仍可通过时间与实体身份检索。
        TimelineInteractiveElement el;
        el.time         = seg.time;
        el.y            = y;
        el.effects      = seg.effects;
        el.bpmEntity    = seg.bpmEntity;
        el.scrollEntity = seg.scrollEntity;
        el.jumpEntity   = seg.jumpEntity;
        el.hsEntity     = seg.hsEntity;
        el.bpmValue     = seg.bpmValue;
        el.scrollValue  = seg.scrollValue;
        el.jumpValue    = seg.jumpValue;
        el.hsValue      = seg.hsValue;
        // 保存下标而非指向 vector 元素的指针。
        // 后续向该容器追加内容可能扩容，下标仍能重新定位元素。
        snapshot->timelineElements.push_back(el);
        size_t interactiveElementIdx = snapshot->timelineElements.size() - 1;

        if ( professionalMode ) {
            // 固定顺序也决定通用几何槽选用哪个首个标记。
            // 每种效果仍有独立槽，不能把多个专业轨道合并为同一个命中框。
            constexpr ::MMM::TimingEffect professionalEffects[] = {
                ::MMM::TimingEffect::BPM,
                ::MMM::TimingEffect::SCROLL,
                ::MMM::TimingEffect::JUMP,
                ::MMM::TimingEffect::HS,
            };
            for ( auto effect : professionalEffects ) {
                // 只绘制本段实际包含的类型。
                // 效果位可以组合，不能把整个掩码强制转换为单个枚举。
                if ( (seg.effects & timelineEffectMask(effect)) == 0 ) {
                    continue;
                }

                const int lane = professionalTimelineLane(effect);
                // 占用失败仅跳过此类型的几何。
                // 已经写入的时间线记录不回滚，其他类型仍可尝试自己的轨道。
                if ( !occupyMarkerRow(lane, y) ) {
                    continue;
                }

                // 标记在所属轨道内部居中。
                // 宽度减去的留缝平均分配到两侧，不改变类型轨道原点。
                noteX = professionalLaneWidth * static_cast<float>(lane) +
                        (professionalLaneWidth - noteW) * 0.5f;
                batcher.setTexture(TextureID::Note);
                // 在提交之前保存两个缓冲的起点。
                // 顶点数与索引数分别记录，不能按固定四边形拓扑相互推导。
                // 普通标记的一个几何区间可供多个效果槽共享。
                // 共享的是缓冲偏移，不另行复制顶点。
                const uint32_t markerVertexOffset =
                    static_cast<uint32_t>(snapshot->vertices.size());
                const uint32_t markerIndexOffset =
                    static_cast<uint32_t>(snapshot->indices.size());
                batcher.pushFilledQuad(noteX,
                                       y + noteH * 0.5f,
                                       noteW,
                                       noteH,
                                       { 1.0f, 1.0f },
                                       config.visual.noteFillMode,
                                       markerColorForEffect(effect));

                // 通过保存的下标重新获取交互元素。
                // 类型槽引用只在本次填写期间使用，不跨容器追加操作保留。
                auto& element =
                    snapshot->timelineElements[interactiveElementIdx];
                auto& geometry = markerGeometryForEffect(element, effect);
                writeMarkerGeometry(
                    geometry, markerVertexOffset, markerIndexOffset);
                // 旧消费入口仍可读取一个通用几何槽。
                // 只用第一个实际绘制的类型填充通用槽，后续类型保留各自独立数据。
                if ( !element.hasMarkerGeometry ) {
                    element.hasMarkerGeometry  = geometry.hasMarkerGeometry;
                    element.markerVertexOffset = geometry.markerVertexOffset;
                    element.markerVertexCount  = geometry.markerVertexCount;
                    element.markerIndexOffset  = geometry.markerIndexOffset;
                    element.markerIndexCount   = geometry.markerIndexCount;
                }
            }
            // 专业模式已按类型分别处理，不再落入普通模式重复绘制。
            // 尤其不能再次抢占普通轨零的像素行。
            continue;
        }

        // 普通模式同像素行只保留一个合成标记。
        // 它可同时代表同段的多种 Timing 类型。
        if ( !occupyMarkerRow(0, y) ) continue;

        // 组合 BPM 与 SCROLL 使用专用橙色。
        // 其余组合按下面固定优先级选择可见颜色，逻辑效果掩码仍完整保留。
        glm::vec4 color = { tickCol.r, tickCol.g, tickCol.b, 0.8f };
        if ( (seg.effects & SCROLL_EFFECT_BPM) &&
             (seg.effects & SCROLL_EFFECT_SCROLL) ) {
            color = { 1.0f, 0.5f, 0.0f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
            color = markerColorForEffect(::MMM::TimingEffect::BPM);
        } else if ( seg.effects & SCROLL_EFFECT_JUMP ) {
            color = markerColorForEffect(::MMM::TimingEffect::JUMP);
        } else if ( seg.effects & SCROLL_EFFECT_HS ) {
            color = markerColorForEffect(::MMM::TimingEffect::HS);
        } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            color = markerColorForEffect(::MMM::TimingEffect::SCROLL);
        }

        batcher.setTexture(TextureID::Note);
        const uint32_t markerVertexOffset =
            static_cast<uint32_t>(snapshot->vertices.size());
        const uint32_t markerIndexOffset =
            static_cast<uint32_t>(snapshot->indices.size());
        batcher.pushFilledQuad(noteX,
                               y + noteH * 0.5f,
                               noteW,
                               noteH,
                               { 1.0f, 1.0f },
                               config.visual.noteFillMode,
                               color);
        // 把通用标记区间写回已发布的逻辑记录。
        // 记录数可能大于实际标记数，消费端必须看 hasMarkerGeometry。
        auto& element = snapshot->timelineElements[interactiveElementIdx];
        element.hasMarkerGeometry  = true;
        element.markerVertexOffset = markerVertexOffset;
        element.markerVertexCount  = static_cast<uint32_t>(
            snapshot->vertices.size() - markerVertexOffset);
        element.markerIndexOffset = markerIndexOffset;
        element.markerIndexCount =
            static_cast<uint32_t>(snapshot->indices.size() - markerIndexOffset);
        // 同段包含哪些效果，就让哪些类型槽指向这次合成几何。
        // 类型实体身份仍各自独立，不能因共享外观合并编辑目标。
        if ( seg.effects & SCROLL_EFFECT_BPM ) {
            writeMarkerGeometry(
                element.bpmMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            writeMarkerGeometry(
                element.scrollMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_JUMP ) {
            writeMarkerGeometry(
                element.jumpMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_HS ) {
            writeMarkerGeometry(
                element.hsMarker, markerVertexOffset, markerIndexOffset);
        }
    }

    // 背景之后到当前末端都随时间线滚动。
    // 紧接着绘制的判定框不包含在这一动态顶点范围内。
    snapshot->dynamicVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size()) -
        snapshot->staticVertexCount;

    // 6. 绘制当前时间判定框，作为时间线最上层覆盖物。
    batcher.flush();
    drawJudgmentGuideBox(batcher, paddingX, judgmentLineY, lineW, 18.0f);
}

/// @brief 计算预览轨道布局及相对主画布的纵向缩放。
/// @param snapshot 读取谱面存在状态并记录静态边界。
/// @param batcher 保留统一布局接口，本入口不直接追加几何。
/// @param currentTime 保留统一布局接口，此布局不依赖时间位置。
/// @param viewportWidth 预览画布宽度。
/// @param viewportHeight 预览画布高度。
/// @param judgmentLineY 保留统一接口，判定线由后续绘制使用。
/// @param trackCount 玩家轨数，用于计算单轨宽度。
/// @param config 包含预览边距、覆盖范围和主画布布局的配置。
/// @param mainViewportHeight 主画布像素高度。
/// @param leftX 输出预览左边界。
/// @param rightX 输出预览右边界。
/// @param topY 输出预览上边界。
/// @param bottomY 输出预览下边界。
/// @param trackAreaW 输出可绘制轨道总宽。
/// @param singleTrackW 输出单轨宽度。
/// @param renderScaleY 输出主画布滚动距离到预览像素的比例。
/// @pre 有谱面时 trackCount、主画布有效高度与 areaRatio 必须为正。
/// @note 没有谱面时保留调用方输出初值。
/// @warning 每个预览快照调用，只读取配置并计算布局，不加载皮肤资源。
void NoteRenderSystem::generatePreviewSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float& renderScaleY)
{
    if ( !snapshot->hasBeatmap ) return;

    // 边距是绝对像素值，不能再次乘视口尺寸。
    // 左右及上下边距各自计算，支持非对称预览布局。
    leftX      = config.visual.previewConfig.margin.left;
    rightX     = viewportWidth - config.visual.previewConfig.margin.right;
    topY       = config.visual.previewConfig.margin.top;
    bottomY    = viewportHeight - config.visual.previewConfig.margin.bottom;
    trackAreaW = rightX - leftX;
    // 仅按玩家轨数压缩显示，不纳入草稿和 BGM 区。
    // 有效轨数由上层谱面状态保证，此处保持原有分母约束。
    singleTrackW = trackAreaW / static_cast<float>(trackCount);

    // 主画布有效高度排除上下布局留白。
    // 预览覆盖倍数以这段高度为基准，窗口外边距不计入倍率。
    float mainEffectiveH =
        (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
        mainViewportHeight;

    float previewDrawH = bottomY - topY;

    // 预览像素高度除以要覆盖的主画布滚动高度。
    // areaRatio 越大，同一时间跨度显示得越紧凑。
    // 调用方随后把同一比例用于音符、拍线与视野包围盒。
    renderScaleY =
        previewDrawH / (mainEffectiveH * config.visual.previewConfig.areaRatio);

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());
}

/// @brief 绘制主画布背景、玩家与辅助区域静态布局。
/// @param registry 保留统一接口，本入口不扫描音符实体。
/// @param timelineRegistry 借用时间线状态，供轨道布局绘制使用。
/// @param snapshot 提供谱面存在状态和水平平移量。
/// @param batcher 接收背景、轨道及判定区几何。
/// @param currentTime 当前视觉时间，供随时间变化的布局使用。
/// @param viewportWidth 主画布宽度。
/// @param viewportHeight 主画布高度。
/// @param judgmentLineY 屏幕判定线位置。
/// @param trackCount 玩家轨数。
/// @param config 会话布局与显示配置。
/// @param bgmTrackCount 自动采样区域轨数。
/// @param draftTrackCount 草稿区域独立轨数。
/// @param cache 当前时间线的滚动缓存。
/// @param leftX 输出玩家轨道左边界。
/// @param rightX 输出玩家轨道右边界。
/// @param topY 输出轨道上边界。
/// @param bottomY 输出轨道下边界。
/// @param trackAreaW 输出玩家轨道总宽。
/// @param singleTrackW 输出玩家单轨宽度。
/// @param renderScaleY 传给轨道布局的纵向比例，按值传入。
/// @pre snapshot 非空；有谱面分支的轨道数和缓存由会话准备。
/// @note 空会话只绘制背景与 Logo，不计算谱面轨道输出。
/// @note 草稿区域先铺纹理，再叠加区分色、边框和判定区。
/// @warning 每个主画布快照执行，不得引入资源解码、目录查询或阻塞同步。
void NoteRenderSystem::generateMainCanvasSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    int32_t bgmTrackCount, int32_t draftTrackCount, const ScrollCache* cache,
    float& leftX, float& rightX, float& topY, float& bottomY, float& trackAreaW,
    float& singleTrackW, float renderScaleY)
{
    // 先绘制全画布背景，再按谱面状态选择占位或轨道布局。
    // 背景资源由其他生命周期入口准备，此处只消费快照与配置。
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    if ( !snapshot->hasBeatmap ) {
        // Logo 属于完整画布占位内容，不应继承轨道布局的水平裁剪范围。
        batcher.setScissor(0.0f, 0.0f, viewportWidth, viewportHeight);
        batcher.setTexture(TextureID::Logo);
        // Logo 以短边为基准，窗口纵横比例变化时保持正方形。
        // 低透明度使空会话占位不会盖住背景主体。
        float logoSize = std::min(viewportWidth, viewportHeight) * 0.4f;
        float cx       = viewportWidth * 0.5f;
        float cy       = viewportHeight * 0.5f;
        batcher.pushQuad(cx - logoSize * 0.5f,
                         cy + logoSize * 0.5f,
                         logoSize,
                         logoSize,
                         { 1.0f, 1.0f, 1.0f, 0.15f });
    } else {
        // 谱面布局只允许在轨道水平范围内生成基础绘制命令。
        const auto projection =
            calculatePlayerTrackProjection(viewportWidth,
                                           trackCount,
                                           config.visual.trackLayout.left,
                                           config.visual.trackLayout.right,
                                           snapshot->canvasHorizontalOffsetX);
        // 轨道左右边界保留水平平移结果。
        // 扩展的是垂直裁剪范围，不能同时把横向裁剪扩大到整个窗口。
        const float lx = projection.leftX;
        const float rx = projection.rightX;
        // 扩展垂直方向的裁剪区域，给予上下各 0.5 倍视口的余量。
        batcher.setScissor(
            lx, -viewportHeight * 0.5f, rx - lx, viewportHeight * 2.0f);
        NoteRenderSystem::renderTrackLayout(batcher,
                                            viewportWidth,
                                            viewportHeight,
                                            judgmentLineY,
                                            trackCount,
                                            config,
                                            timelineRegistry,
                                            currentTime,
                                            cache,
                                            leftX,
                                            rightX,
                                            topY,
                                            bottomY,
                                            trackAreaW,
                                            singleTrackW,
                                            renderScaleY);
        // 玩家布局已写入输出参数，辅助区域再由统一投影计算。
        // 草稿、批注和 BGM 可独立移动，不假定它们永远在玩家区同一侧。
        const auto laneProjection =
            calculateCanvasLaneProjection(viewportWidth,
                                          trackCount,
                                          bgmTrackCount,
                                          config.visual.trackLayout,
                                          snapshot->canvasHorizontalOffsetX,
                                          true,
                                          config.settings.enableBmsEditing,
                                          config.settings.professionalMode,
                                          draftTrackCount,
                                          true);
        const float visibleDraftLeft =
            std::max(0.0F, laneProjection.draftLeftX);
        const float visibleDraftRight =
            std::min(viewportWidth, laneProjection.draftRightX);
        // 草稿背景只在与视口有交集时生成。
        // 原始区域边界仍用于纹理和边框，裁剪仅限制显示范围。
        if ( visibleDraftRight > visibleDraftLeft ) {
            // 纹理叠加色与区域覆盖色分开，便于旧皮肤分别回退。
            // 判定区也拥有独立配色，不强制沿用背景 tint。
            const auto trackTint = laneColor(DRAFT_TRACK_TEXTURE_TINT_COLOR_KEY,
                                             { 1.0F, 1.0F, 1.0F, 1.0F });
            const auto overlay   = laneColor(DRAFT_TRACK_OVERLAY_COLOR_KEY,
                                             { 0.08F, 0.12F, 0.18F, 0.48F });
            const auto border    = laneColor(DRAFT_TRACK_BORDER_COLOR_KEY,
                                             { 0.35F, 0.55F, 0.75F, 1.0F });
            const auto judgmentTint =
                laneColor(DRAFT_TRACK_JUDGMENT_TINT_COLOR_KEY,
                          { 1.0F, 1.0F, 1.0F, 1.0F });
            batcher.setScissor(visibleDraftLeft,
                               topY,
                               visibleDraftRight - visibleDraftLeft,
                               bottomY - topY);
            // 背景使用草稿自己的单轨宽度与数量。
            // 玩家区轨宽变化不能覆盖独立草稿布局。
            NoteRenderSystem::drawTrackBackground(
                batcher,
                static_cast<std::int32_t>(laneProjection.draftLaneCount),
                laneProjection.draftLeftX,
                topY,
                bottomY,
                laneProjection.draftLaneWidth,
                trackTint);
            // 区域覆盖与边框使用纯色几何。
            // 切换到无纹理避免沿用轨道贴图采样。
            batcher.setTexture(TextureID::None);
            batcher.pushQuad(
                laneProjection.draftLeftX,
                bottomY,
                laneProjection.draftRightX - laneProjection.draftLeftX,
                bottomY - topY,
                overlay);
            batcher.pushStrokeRect(laneProjection.draftLeftX,
                                   topY,
                                   laneProjection.draftRightX,
                                   bottomY,
                                   config.visual.trackBoxLineWidth,
                                   border);
            // 草稿判定区仍共享当前屏幕判定线。
            // 只改变横向区域与 tint，不另设一个独立播放时间。
            NoteRenderSystem::drawJudgmentArea(
                batcher,
                static_cast<std::int32_t>(laneProjection.draftLaneCount),
                laneProjection.draftLeftX,
                judgmentLineY,
                laneProjection.draftLaneWidth,
                laneProjection.draftRightX - laneProjection.draftLeftX,
                config,
                judgmentTint);
        }
        // 辅助采样区域在草稿基础层之后绘制。
        // 传入逻辑轨数及投影，显示布局和标签数量由同一上下文决定。
        SampleRenderSystem::renderLaneLayout(batcher,
                                             laneProjection,
                                             draftTrackCount,
                                             bgmTrackCount,
                                             viewportWidth,
                                             topY,
                                             bottomY);
    }
}

/// @brief 将交互实际使用的缩放包围盒叠加到画布。
/// @param batcher 接收无纹理填充及描边，裁剪由调用方设置。
/// @param snapshot 已生成的交互命中框与缩放配置，允许为空。
/// @note 遍历已生成的命中框，不重新遍历 registry 构建几何。
/// @note 空实体与非正面积跳过，不把占位框显示为可交互对象。
/// @note 按命中部件着色，用于区分头、身体、尾部和采样控制点。
/// @warning 调试开关启用时随主画布快照执行；禁止文件访问或共享所有权复制。
void NoteRenderSystem::debugRenderHitboxes(Batcher&        batcher,
                                           RenderSnapshot* snapshot)
{
    // 调试入口允许缺省快照，避免诊断路径成为新的崩溃点。
    // 无快照时不修改批处理器状态。
    if ( !snapshot ) return;

    batcher.setTexture(TextureID::None);
    for ( const auto& rawHitbox : snapshot->hitboxes ) {
        // 应用与实际交互相同的缩放函数。
        // 保留原始命中框，调试绘制不能改变后续 UI 拾取结果。
        const auto hb =
            scaleInteractionHitbox(rawHitbox,
                                   snapshot->interactionHitboxScaleX,
                                   snapshot->interactionHitboxScaleY);
        if ( hb.entity == entt::null || hb.w <= 0.0f || hb.h <= 0.0f ) continue;

        // 填充统一保持低透明度，部件差异主要由描边颜色表达。
        // 重叠命中区域仍能观察到下方物件，避免调试层完全遮挡画布。
        glm::vec4 color{ 0.2f, 0.9f, 1.0f, 0.8f };
        switch ( hb.part ) {
        case HoverPart::Head: color = { 0.25f, 1.0f, 0.25f, 0.85f }; break;
        case HoverPart::HoldBody: color = { 0.0f, 0.8f, 1.0f, 0.75f }; break;
        case HoverPart::HoldEnd: color = { 1.0f, 0.65f, 0.1f, 0.85f }; break;
        case HoverPart::FlickArrow: color = { 1.0f, 0.25f, 1.0f, 0.85f }; break;
        case HoverPart::PolylineNode:
            color = { 1.0f, 1.0f, 0.15f, 0.85f };
            break;
        case HoverPart::SampleAnchor:
            color = { 0.25f, 0.85f, 1.0f, 0.9f };
            break;
        case HoverPart::SampleOffset: color = { 1.0f, 0.5f, 0.2f, 0.9f }; break;
        case HoverPart::None: color = { 1.0f, 1.0f, 1.0f, 0.45f }; break;
        }

        // 命中框使用左上角与尺寸，填充接口需要底边坐标。
        // 描边继续使用原始两端边界，保证两种几何吻合。
        batcher.pushQuad(hb.x,
                         hb.y + hb.h,
                         hb.w,
                         hb.h,
                         { color.r, color.g, color.b, 0.08f });
        batcher.pushStrokeRect(
            hb.x, hb.y, hb.x + hb.w, hb.y + hb.h, 2.0f, color);
    }
}

}  // namespace MMM::Logic::System
