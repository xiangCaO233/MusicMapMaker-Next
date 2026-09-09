#include "logic/ecs/system/NoteRenderSystem.h"

#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

namespace
{

/// @brief 计算自动显示模式下指定分拍线的附加不透明度。
/// @param distanceToCursor 分拍线到光标中心的垂直像素距离。
/// @param viewportHeight 当前画布垂直范围。
/// @param visibleRatio 完全显示区域占画布垂直范围的比例。
/// @param fadeRatio 两侧渐隐区域合计占画布垂直范围的比例。
/// @return 范围在 0 到 1 之间的平滑不透明度倍率。
/// @pre 距离和比例为有限数，viewportHeight 为正；输入不在此统一规范化。
/// @note 只计算附加倍率，皮肤 alpha 和用户总透明度由调用方叠乘。
/// @warning 分拍线热路径逐线调用；仅允许常数次浮点运算。
float calculateCursorRevealAlpha(float distanceToCursor, float viewportHeight,
                                 float visibleRatio, float fadeRatio)
{
    // 配置给出上下合计范围，单侧跨度需再除以二。
    const float visibleHalfSpan =
        viewportHeight * std::clamp(visibleRatio, 0.05f, 0.50f) * 0.5f;
    const float fadeHalfSpan =
        viewportHeight * std::clamp(fadeRatio, 0.02f, 0.40f) * 0.5f;
    if ( distanceToCursor <= visibleHalfSpan ) return 1.0f;
    if ( distanceToCursor >= visibleHalfSpan + fadeHalfSpan ) return 0.0f;

    const float progress = (distanceToCursor - visibleHalfSpan) / fadeHalfSpan;
    // 前面的内外边界判断保证此处处于渐变区，不让多项式延伸到有效区间外。
    // smoothstep 让渐隐两端斜率归零，避免光标移动时出现明显透明度折点。
    const float smoothProgress = progress * progress * (3.0f - 2.0f * progress);
    return 1.0f - smoothProgress;
}

}  // namespace

/// @brief 计算玩家区布局并依次绘制底板、外框和判定区域。
/// @param batcher 当前快照的几何输出器。
/// @param viewportWidth 画布逻辑宽度。
/// @param viewportHeight 画布逻辑高度。
/// @param judgmentLineY 已由调用方计算的判定线纵坐标。
/// @param trackCount 玩家轨道数量。
/// @param config 布局比例、外框宽度及判定区缩放配置。
/// @param timelineRegistry 保留的时间线参数，本实现不读取其实体。
/// @param currentTime 保留的当前时间参数，不影响静态底板布局。
/// @param cache 保留的滚动缓存参数，不在此积分时间位置。
/// @param leftX 输出应用相机平移后的左边界。
/// @param rightX 输出应用相机平移后的右边界。
/// @param topY 输出轨道上边界。
/// @param bottomY 输出轨道下边界。
/// @param trackAreaW 输出整个玩家区宽度。
/// @param singleTrackW 输出单条玩家轨道宽度。
/// @param renderScaleY 保留的纵向缩放参数，底板不随滚动速度伸缩。
/// @pre batcher.snapshot 有效，视口和布局比例满足有限坐标要求。
/// @note 时间线、时间与滚动参数保留在接口中，本函数不绘制分拍线。
/// @note 输出边界是内容坐标，不保证落在视口内；调用方仍需设置裁剪。
/// @warning 快照生成热路径，使用现有皮肤资源，不执行加载或时间线重建。
void NoteRenderSystem::renderTrackLayout(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    float judgmentLineY, int32_t trackCount, const Config::EditorConfig& config,
    const entt::registry& timelineRegistry, double currentTime,
    const ScrollCache* cache, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float renderScaleY)
{
    // 1. 基础布局计算 (确保范围有效，防止后续计算出现无限循环)
    float l = config.visual.trackLayout.left;
    float r = config.visual.trackLayout.right;
    float t = config.visual.trackLayout.top;
    float b = config.visual.trackLayout.bottom;

    // 强制保证 Left < Right, Top < Bottom
    // 只修复本次局部布局，不回写用户配置，也不将边界强制限制到视口内。
    if ( l >= r ) {
        r = l + 0.01f;
    }
    if ( t >= b ) {
        b = t + 0.01f;
    }

    // 水平相机偏移作用于内容坐标，纵向布局仍由视口比例决定。
    const float horizontalOffsetX =
        batcher.snapshot ? batcher.snapshot->canvasHorizontalOffsetX : 0.0F;
    leftX      = viewportWidth * l + horizontalOffsetX;
    rightX     = viewportWidth * r + horizontalOffsetX;
    topY       = viewportHeight * t;
    bottomY    = viewportHeight * b;
    trackAreaW = rightX - leftX;
    // 无有效轨道时仍提供有限的宽度输出，具体纹理绘制再按轨道数量跳过。
    singleTrackW = trackAreaW / std::max(1.0f, static_cast<float>(trackCount));

    // 2. 绘制轨道底板
    NoteRenderSystem::drawTrackBackground(
        batcher, trackCount, leftX, topY, bottomY, singleTrackW);

    // 3. 绘制轨道包围框
    // 外框切回纯色图元，不能继承底板纹理，否则边线会被皮肤内容调制。
    batcher.setTexture(TextureID::None);
    batcher.pushStrokeRect(leftX,
                           topY,
                           rightX,
                           bottomY,
                           config.visual.trackBoxLineWidth,
                           { 0.5f, 0.5f, 0.5f, 1.0f });

    // 4. 绘制判定区域
    // 判定区在底板和外框之后提交，保持其作为交互参照的层级。
    NoteRenderSystem::drawJudgmentArea(batcher,
                                       trackCount,
                                       leftX,
                                       judgmentLineY,
                                       singleTrackW,
                                       trackAreaW,
                                       config);
}

/// @brief 按单轨纹理纵向平铺底板，保持各轨道纹理起点一致。
/// @param batcher 输出图元并提供轨道纹理图集信息。
/// @param trackCount 当前区域的轨道数量。
/// @param leftX 区域左边界。
/// @param topY 平铺终止的上边界。
/// @param bottomY 平铺起始的下边界。
/// @param singleTrackW 单轨宽度，同时决定纹理等比平铺高度。
/// @param color 轨道底板统一乘色。
/// @pre topY 不大于 bottomY，纹理 UV 对应当前固定规格图集。
/// @note 函数不主动 flush，后续图元可继续由批处理器合并提交。
/// @note 平铺相位固定在底边，不跟随谱面播放时间变化。
/// @warning 渲染热路径只按可绘制区域平铺，不请求纹理或创建独立缓冲。
void NoteRenderSystem::drawTrackBackground(Batcher& batcher, int32_t trackCount,
                                           float leftX, float topY,
                                           float bottomY, float singleTrackW,
                                           glm::vec4 color)
{
    if ( trackCount <= 0 || singleTrackW <= 0.001f ) return;

    // 底板不提供纯色替代物；无纹理时留给画布背景显示。
    batcher.setTexture(TextureID::Track);
    auto uvIt =
        batcher.snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Track));
    if ( uvIt == batcher.snapshot->uvMap.end() ) return;

    // 图集记录是归一化宽高，此处按当前图集规格换算纹理原始比例。
    float texW_px = uvIt->second.z * 2048.0f;
    float texH_px = uvIt->second.w * 2048.0f;

    if ( texW_px <= 0 || texH_px <= 0 ) return;

    float texAspect = texW_px / texH_px;
    // 使用单轨宽度恢复原图纵横比例，不把整片玩家区当作一张底板的宽度。
    float drawH = singleTrackW / texAspect;

    // 亚像素高度会导致单轨生成过多图元；这里只画高度超过一像素的块。
    if ( drawH <= 1.0f ) return;

    // UV 内缩半像素，防止线性采样混入相邻图集条目。
    const float halfPixelU = 0.5f / 2048.0f;
    const float halfPixelV = 0.5f / 2048.0f;

    float uMin = uvIt->second.x + halfPixelU;
    float uMax = uvIt->second.x + uvIt->second.z - halfPixelU;

    for ( int i = 0; i < trackCount; ++i ) {
        // 这里按完整轨道布局平铺，横向可见性由调用方的裁剪状态处理。
        float trackX = leftX + i * singleTrackW;
        // 各轨从同一底边起铺，顶部不足一块时不会错开相邻轨的纹理相位。
        float currentY = bottomY;
        // 少量横向重叠用于遮住相邻轨道间的采样接缝。
        float drawW = singleTrackW + 0.5f;

        while ( currentY > topY ) {
            // 高度先裁到区域内；遮缝增量仅在提交几何时追加。
            float remainH     = currentY - topY;
            float actualDrawH = std::min(drawH, remainH);
            // 最后一块可能只有部分高度，截取对应 UV 而不是压缩整块纹理。

            float vMax = 1.0f;
            float vMin = 1.0f - (actualDrawH / drawH);

            // vMin/vMax 是单张纹理内比例，不可直接当作整张图集坐标使用。
            float finalVMin =
                uvIt->second.y + vMin * uvIt->second.w + halfPixelV;
            float finalVMax =
                uvIt->second.y + vMax * uvIt->second.w - halfPixelV;

            // 批处理器以底边加高度表示矩形，currentY 因而无需减 actualDrawH。
            batcher.pushUVQuad(trackX,
                               currentY,
                               drawW,
                               actualDrawH + 0.5f,
                               glm::vec2(uMin, finalVMin),
                               glm::vec2(uMax, finalVMax),
                               color);

            // 位置按完整块高度推进，不累积用于遮缝的额外半像素。
            currentY -= drawH;
        }
    }
}

/// @brief 在判定线位置绘制各轨判定区，缺少纹理时退回贯穿轨道的细线。
/// @param batcher 当前图元批处理器。
/// @param trackCount 当前区域轨道数。
/// @param leftX 区域左边界。
/// @param judgmentLineY 判定区中心纵坐标。
/// @param singleTrackW 单轨基础宽度。
/// @param trackAreaW 无纹理回退线的总跨度。
/// @param config 提供判定区沿用的物件横纵缩放。
/// @param color 判定区统一乘色。
/// @note 纹理尺寸取宽高比而非绝对像素大小，实际大小由轨宽和用户缩放决定。
/// @note 判定区只提交视觉几何，不在此生成拾取框或修改判定时间。
/// @pre 调用方已设置所需 Scissor，判定区自身不新建裁剪命令。
/// @warning 快照生成热路径只读取图集，不尝试补载缺失皮肤资源。
void NoteRenderSystem::drawJudgmentArea(Batcher& batcher, int32_t trackCount,
                                        float leftX, float judgmentLineY,
                                        float singleTrackW, float trackAreaW,
                                        const Config::EditorConfig& config,
                                        glm::vec4                   color)
{
    batcher.setTexture(TextureID::JudgeArea);
    auto judgeUvIt = batcher.snapshot->uvMap.find(
        static_cast<uint32_t>(TextureID::JudgeArea));

    if ( judgeUvIt != batcher.snapshot->uvMap.end() ) {
        // 图集存在键不保证尺寸有效；无效尺寸和键缺失采用不同的既有处理路径。
        // 尺寸检查先于求宽高比，空图集条目不得参与除法。
        float texW = judgeUvIt->second.z * 2048.0f;
        float texH = judgeUvIt->second.w * 2048.0f;
        if ( texW > 0 && texH > 0 ) {
            float aspect = texW / texH;
            float drawW  = singleTrackW * config.visual.noteScaleX;
            // 横纵缩放独立应用，中心锚点仍固定在对应轨道和判定线上。
            float drawH = (singleTrackW / aspect) * config.visual.noteScaleY;

            const float halfPixelU = 0.5f / 2048.0f;
            // 和底板使用相同图集规格，采样边界内缩避免邻接纹理串色。
            const float halfPixelV = 0.5f / 2048.0f;

            for ( int i = 0; i < trackCount; ++i ) {
                // 缩放围绕单轨中心展开，不改变轨道本身的宽度或相邻轨间距。
                float trackCenterX =
                    leftX + i * singleTrackW + singleTrackW * 0.5f;
                float drawX = trackCenterX - drawW * 0.5f;

                // 接口接收底边坐标，增加半高才能让纹理中心落在判定线上。
                batcher.pushUVQuad(
                    drawX,
                    judgmentLineY + drawH * 0.5f,
                    drawW,
                    drawH,
                    glm::vec2(judgeUvIt->second.x + halfPixelU,
                              judgeUvIt->second.y + halfPixelV),
                    glm::vec2(
                        judgeUvIt->second.x + judgeUvIt->second.z - halfPixelU,
                        judgeUvIt->second.y + judgeUvIt->second.w - halfPixelV),
                    color);
            }
        }
    } else {
        // 只在纹理键缺失时采用细线；已有但尺寸无效的条目由上方检查跳过。
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(
            leftX, judgmentLineY + 2.0f * 0.5f, trackAreaW, 2.0f, color);
    }
}

/// @brief 在可见滚动区间内绘制分拍网格及可选悬浮细分预览。
/// @param batcher 提供悬浮状态、细分预览及皮肤图集的当前快照输出器。
/// @param viewportHeight 自动渐隐范围所依据的画布高度。
/// @param judgmentLineY 当前显示原点对应的画布纵坐标。
/// @param config 分拍数、颜色覆盖和预览显示配置。
/// @param bpmEvents 按时间排序的 BPM 组件指针，调用期间必须保持有效。
/// @param currentTime 当前显示时间，用于取得视觉锚点。
/// @param cache 已建立的滚动映射，空指针时不绘制。
/// @param leftX 网格区域左边界。
/// @param topY 网格可见范围的一侧纵向边界。
/// @param bottomY 另一侧纵向边界，绘制内部会统一边界顺序。
/// @param trackAreaW 普通分拍线横向覆盖的区域宽度。
/// @param renderScaleY 从滚动距离到画布纵坐标的倍率，允许反向映射。
/// @param revealNearCursor 是否仅在悬浮光标附近渐隐显示。
/// @param opacityScale 调用方追加的整体透明度倍率，限制在 0～1。
/// @param allowHoverSubdivisionPreview 是否允许以悬浮细分替换目标轨道网格。
/// @pre BPM 列表已经过滤出 BPM 事件，指针非空且时间顺序有效。
/// @note 空 BPM 列表不构造默认网格；非法分拍数则兼容回退为四分拍。
/// @warning 快照生成路径含可见区间查询和逐线绘制，不得加入磁盘访问或阻塞等待。
/// @note 本实现每次调用创建像素行占用表，不能将其描述成完全无分配路径。
void NoteRenderSystem::drawBeatLines(
    Batcher& batcher, float viewportHeight, float judgmentLineY,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents, double currentTime,
    const ScrollCache* cache, float leftX, float topY, float bottomY,
    float trackAreaW, float renderScaleY, bool revealNearCursor,
    float opacityScale, bool allowHoverSubdivisionPreview)
{
    if ( !cache ) return;
    // 自动显示依赖当前画布悬浮状态，不以编辑会话焦点代替鼠标命中。
    if ( revealNearCursor && !batcher.snapshot->isHoveringCanvas ) return;

    int beatDivisor = config.settings.beatDivisor;
    // 分拍数直接作为除数和取模基数，必须先排除零及负值。
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    if ( bpmEvents.empty() ) return;

    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    // 锚点可平滑微脉冲，网格位置仍按各时间点的原始显示距离求出。
    // 此后需要反解视窗边界，接近零的缩放不具有可用的逆映射。
    if ( std::abs(renderScaleY) < 1e-6f ) return;
    const float cursorY =
        // 渐隐中心来自悬浮时间的投影，不直接沿用另一画布的鼠标像素位置。
        revealNearCursor
            ? judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                  batcher.snapshot->hoveredTime,
                                  currentAbsY,
                                  batcher.snapshot->hoveredTime)) *
                                  renderScaleY
            : judgmentLineY;
    // 从画布空间退回滚动空间时保留缩放符号，再排序得到查询窗口。
    double topAbsY = currentAbsY +
                     (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double bottomAbsY = currentAbsY + (judgmentLineY - bottomY) /
                                          static_cast<double>(renderScaleY);
    // 反向 SV 可能让同一视窗对应多段不连续时间，不能只反解上下端点。
    auto visibleRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

    batcher.setTexture(TextureID::None);

    float visibleTop    = std::min(topY, bottomY);
    float visibleBottom = std::max(topY, bottomY);
    // 上下边界都允许绘制，额外一行容纳恰好落在整数高度底边的候选。
    int rowCount = std::max(
        1, static_cast<int>(std::ceil(visibleBottom - visibleTop)) + 1);
    std::vector<uint8_t> occupiedRows(static_cast<size_t>(rowCount), 0);
    // 每个像素行至多保留一条普通分拍线，压缩视图下避免重叠网格无限堆积。
    int occupiedRowCount = 0;
    /// @brief 标记本次绘制已占用的像素行，重复位置返回 false。
    /// @param y 分拍线中心纵坐标，不包含线宽和光晕范围。
    /// @return 位于可见区且该像素行首次占用时为 true。
    /// @note 按生成顺序先到先得，不在重叠后重新比较拍位或吸附优先级。
    auto occupyRow = [&](float y) {
        if ( y < visibleTop || y > visibleBottom ) return false;
        // 占行依据中心位置而非线宽或光晕，粗线仍可能覆盖相邻行。
        int row = static_cast<int>(std::floor(y - visibleTop));
        row     = std::clamp(row, 0, rowCount - 1);
        if ( occupiedRows[static_cast<size_t>(row)] != 0 ) return false;
        occupiedRows[static_cast<size_t>(row)] = 1;
        ++occupiedRowCount;
        return true;
    };

    auto& skin = Config::SkinManager::instance();
    // 皮肤 alpha、用户总透明度与调用方倍率叠乘，保留各层透明度语义。
    float globalAlpha =
        config.visual.beatLineAlpha * std::clamp(opacityScale, 0.0F, 1.0F);
    /// @brief 按约分后的分母选择颜色与线宽，并应用全局透明度。
    /// @param denominator 最简拍内分数的分母，整拍使用一。
    /// @return 乘入全局 alpha 的颜色和皮肤提供的线宽。
    /// @note 字符串键在调用内构造，此处没有跨帧样式缓存。
    auto getBeatLineConfig =
        [&skin, &config, globalAlpha](
            int denominator) -> std::pair<glm::vec4, float> {
        std::string   key = "beat_lines.beat_" + std::to_string(denominator);
        Config::Color c   = skin.getColor(key);
        // 皮肤缺色以洋红哨兵表示；此时颜色与宽度一并走默认项。
        if ( c.r == 1.0f && c.g == 0.0f && c.b == 1.0f && c.a == 1.0f ) {
            c   = skin.getColor("beat_lines.default");
            key = "beat_lines_width.default";
        } else {
            key = "beat_lines_width.beat_" + std::to_string(denominator);
        }
        float width =
            skin.getValue(key, skin.getValue("beat_lines_width.default", 2.0f));
        if ( config.visual.overrideBeatLineColors ) {
            // 用户调色只替换颜色，线宽仍沿用相应皮肤配置。
            const auto& overrideColor =
                config.visual.beatLineColors[Config::beatLineColorPaletteSlot(
                    denominator)];
            c = { overrideColor[0],
                  overrideColor[1],
                  overrideColor[2],
                  overrideColor[3] };
        }
        return { glm::vec4(c.r, c.g, c.b, c.a * globalAlpha), width };
    };

    const auto& subdivisionPreview = batcher.snapshot->hoverSubdivisionPreview;
    // 屏蔽未定义标志位，避免把未来或损坏配置解释成额外分拍方案。
    const auto commonBeatDivisorMask =
        subdivisionPreview.commonBeatDivisorMask &
        Config::COMMON_BEAT_DIVISOR_MASK_ALL;
    const bool usesCommonBeatDivisors = commonBeatDivisorMask != 0U;
    // 预览必须同时具备合法轨道、正拍长和允许的分母范围。
    // 常用分拍组合可替代单一分母，但仍沿用其上限检查。
    // 输入侧准备预览上下文，这里不重新推导鼠标所在拍。
    const bool hasSubdivisionPreview =
        allowHoverSubdivisionPreview && subdivisionPreview.show &&
        batcher.snapshot->trackCount > 0 &&
        subdivisionPreview.track >= -batcher.snapshot->trackCount &&
        subdivisionPreview.track < batcher.snapshot->trackCount &&
        (usesCommonBeatDivisors || subdivisionPreview.denominator > 1) &&
        subdivisionPreview.denominator <= 128 &&
        subdivisionPreview.beatEndTime > subdivisionPreview.beatStartTime &&
        subdivisionPreview.beatDuration > 0.0;
    // 预览未启用时避免除以轨道数，候选边界不会用于提交图元。
    const float subdivisionTrackWidth =
        hasSubdivisionPreview
            ? trackAreaW / static_cast<float>(batcher.snapshot->trackCount)
            : 0.0F;
    // 有符号轨号直接参与布局，负轨号的候选仍交由后续裁剪约束可见部分。
    const float subdivisionLeft =
        leftX +
        static_cast<float>(subdivisionPreview.track) * subdivisionTrackWidth;
    const float subdivisionRight = subdivisionLeft + subdivisionTrackWidth;
    // 延伸比例只影响预览线的横向长度，不扩大替换旧网格的轨道范围。
    const float subdivisionLineExtensionRatio = std::clamp(
        config.visual.hoverSubdivisionLineExtensionRatio, 0.0F, 1.0F);
    const float subdivisionLineLeft =
        std::max(leftX,
                 subdivisionLeft -
                     subdivisionTrackWidth * subdivisionLineExtensionRatio);
    const float subdivisionLineRight =
        std::min(leftX + trackAreaW,
                 subdivisionRight +
                     subdivisionTrackWidth * subdivisionLineExtensionRatio);
    /// @brief 将预览中的绝对时间转换为和普通网格一致的画布坐标。
    /// @param time 谱面时间，单位秒。
    /// @return 相对当前判定线与视觉锚点的纵坐标。
    const auto timeToCanvasY = [&](double time) {
        // 每条线以自身时间作为 HS 锚点，与正常分拍线保持同一滚动语义。
        return judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                   time, currentAbsY, time)) *
                                   renderScaleY;
    };
    /// @brief 返回指定纵坐标的自动显示倍率，常显模式返回单位值。
    /// @param y 当前线条中心纵坐标。
    /// @return 仅包含光标渐隐，不包含皮肤和全局 alpha 的倍率。
    const auto revealAlphaAt = [&](float y) {
        // 常显模式完全跳过光标渐隐，悬浮细分和普通线共用相同透明度规则。
        if ( !revealNearCursor ) return 1.0F;
        return calculateCursorRevealAlpha(
            std::abs(y - cursorY),
            viewportHeight,
            config.visual.beatLineCursorVisibleRatio,
            config.visual.beatLineCursorFadeRatio);
    };
    // 分段绘制允许只挖掉目标轨道该拍内的旧分拍线，同时保留其它轨道。
    /// @brief 绘制一段水平网格及可选吸附光晕，不改变全局占行状态。
    /// @param x 当前残段左边界。
    /// @param segmentWidth 当前残段横向跨度，不是线条厚度。
    /// @param y 线条中心纵坐标。
    /// @param lineWidth 纵向厚度，光晕在此基础上扩展。
    /// @param color 已合成各级透明度的线条颜色。
    /// @param glow 是否额外提交三层吸附提示几何。
    const auto drawBeatLineSegment = [&](float     x,
                                         float     segmentWidth,
                                         float     y,
                                         float     lineWidth,
                                         glm::vec4 color,
                                         bool      glow) {
        // 挖去目标轨道或裁剪延伸范围后，左右残段可能为空或方向反转。
        if ( segmentWidth <= 0.001F ) return;
        if ( glow ) {
            // 光晕是同一快照内的纯色图元，不写独立发光后处理命令。
            // 先绘制三层宽且逐渐透明的光晕，再覆盖清晰的中心线。
            glm::vec4 glowColor = color;
            glowColor.a *= 0.6F;
            batcher.pushQuad(x,
                             y + (lineWidth + 4.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 4.0F,
                             glowColor);
            glowColor.a *= 0.5F;
            batcher.pushQuad(x,
                             y + (lineWidth + 10.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 10.0F,
                             glowColor);
            glowColor.a *= 0.5F;
            batcher.pushQuad(x,
                             y + (lineWidth + 20.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 20.0F,
                             glowColor);
        }
        batcher.pushQuad(
            x, y + lineWidth * 0.5F, segmentWidth, lineWidth, color);
    };

    if ( hasSubdivisionPreview ) {
        // 背景先于细分线提交，让线条保持清晰；普通网格在预览完成后再绘制。
        // 反向滚动会交换拍首尾的画布位置，背景高亮按有序边界裁到视窗。
        const float beatStartY =
            timeToCanvasY(subdivisionPreview.beatStartTime);
        const float beatEndY = timeToCanvasY(subdivisionPreview.beatEndTime);
        const float highlightTop =
            std::max(visibleTop, std::min(beatStartY, beatEndY));
        const float highlightBottom =
            std::min(visibleBottom, std::max(beatStartY, beatEndY));
        // 背景只覆盖这一拍与窗口的交集，不按细分线延伸范围扩大。
        if ( highlightBottom > highlightTop ) {
            // 背景不代表某一个约分拍位，使用预览配置的分母来选择强调样式。
            auto [highlightColor, outlineWidth] =
                getBeatLineConfig(subdivisionPreview.denominator);
            // 描边设有最小厚度，避免皮肤细线设置使外框不可辨认。
            // 焦点限制在目标拍内，避免范围外的悬浮值使整块高亮意外消失。
            const double inspectedTime =
                std::clamp(subdivisionPreview.focusTime,
                           subdivisionPreview.beatStartTime,
                           subdivisionPreview.beatEndTime);
            const float highlightReveal =
                revealAlphaAt(timeToCanvasY(inspectedTime));
            // 背景整块使用焦点处的倍率，细分线则各自按纵坐标渐隐。
            glm::vec4 outlineColor = highlightColor;
            highlightColor.a *= 0.38F * highlightReveal;
            outlineColor.a *= 0.95F * highlightReveal;
            batcher.pushQuad(subdivisionLeft,
                             highlightBottom,
                             subdivisionTrackWidth,
                             highlightBottom - highlightTop,
                             highlightColor);
            batcher.pushStrokeRect(subdivisionLeft,
                                   highlightTop,
                                   subdivisionRight,
                                   highlightBottom,
                                   std::max(2.0F, outlineWidth),
                                   outlineColor);
        }

        /// @brief 在目标拍内绘制单个细分位置，端点和不可见位置直接跳过。
        /// @pre divisor 为正，step 严格位于零与 divisor 之间。
        /// @param step 当前分拍方案中严格拍内的步号。
        /// @param divisor 当前方案每拍的等分数。
        /// @note 预览不登记普通网格的像素行；两者只在目标轨道内做替换。
        const auto drawSubdivisionLine = [&](int step, int divisor) {
            // 用最大公约数将细分位置化成最简分数，供颜色选择和去重统一使用。
            const int    common      = std::gcd(step, divisor);
            const int    numerator   = step / common;
            const int    denominator = divisor / common;
            const double lineTime    = subdivisionPreview.beatStartTime +
                                    subdivisionPreview.beatDuration *
                                        static_cast<double>(numerator) /
                                        static_cast<double>(denominator);
            // 预览只补拍内线，拍末边界仍由常规网格负责。
            if ( lineTime >= subdivisionPreview.beatEndTime - 1e-6 ) return;
            // BPM 截断一拍时，以实际拍末限制候选，不能只看理论拍长。
            const float y = timeToCanvasY(lineTime);
            if ( y < visibleTop || y > visibleBottom ) return;

            auto [color, width] = getBeatLineConfig(denominator);
            // 最简分母决定统一样式，例如 2/4 与 1/2 共用颜色。
            // 预览线仅展示候选节奏，不沿用普通网格中当前吸附点的光晕。
            color.a *= revealAlphaAt(y);
            drawBeatLineSegment(subdivisionLineLeft,
                                subdivisionLineRight - subdivisionLineLeft,
                                y,
                                width,
                                color,
                                false);
        };

        if ( usesCommonBeatDivisors ) {
            // 表仅存在于这一拍的预览中，不跨帧或跨轨道保留去重状态。
            std::array<std::array<bool, Config::COMMON_BEAT_DIVISOR_MAX + 1>,
                       Config::COMMON_BEAT_DIVISOR_MAX + 1>
                renderedFractions{};
            // 数组按最简分母和分子索引，零初始化表示当前拍尚未输出任何候选。
            // 多个分拍方案会产生同一位置，按最简分母和分子记录已绘制项。
            for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
                  divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
                  ++divisor ) {
                if ( !Config::isCommonBeatDivisorEnabled(commonBeatDivisorMask,
                                                         divisor) ) {
                    continue;
                }
                for ( int step = 1; step < divisor; ++step ) {
                    const int common      = std::gcd(step, divisor);
                    const int numerator   = step / common;
                    const int denominator = divisor / common;
                    if ( renderedFractions[denominator][numerator] ) continue;
                    renderedFractions[denominator][numerator] = true;
                    // 按精确整数分数去重，不依赖浮点时间或像素位置相等。
                    drawSubdivisionLine(step, divisor);
                }
            }
        } else {
            // 单一分母的拍内步号互不重复，无需再建立分数去重表。
            for ( int step = 1; step < subdivisionPreview.denominator;
                  ++step ) {
                // 排除零和分母本身，避免重绘拍首与拍末边界。
                drawSubdivisionLine(step, subdivisionPreview.denominator);
            }
        }
    }

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        // 每段用当前 BPM 独立计算拍长，不能跨 BPM 边界沿用上一段步长。
        const auto* currentBPM = bpmEvents[i];
        double      bpmTime    = currentBPM->m_timestamp;
        // 与模型共用 BPM 规范化入口，避免渲染自行定义另一套异常值语义。
        // 这里只规范本次计算值，不回写谱面中的时间点。
        const double bpmVal = ::MMM::normalizeBpmValue(
            currentBPM->m_value, batcher.snapshot->fallbackBpm);

        // 尾段无需人为构造结束事件，实际循环上界仍受可见时间段限制。
        double nextBpmTime = (i + 1 < bpmEvents.size())
                                 ? bpmEvents[i + 1]->m_timestamp
                                 : std::numeric_limits<double>::infinity();

        double beatDuration = 60.0 / bpmVal;
        // BPM 定义每分钟拍数，时间线使用秒；每拍再分成 beatDivisor 份。
        double stepDuration = beatDuration / beatDivisor;

        for ( const auto& [startTime, endTime] : visibleRanges ) {
            // 先求 BPM 段与可见时间段的交集，省去离屏范围的逐拍推进。
            if ( nextBpmTime <= startTime ) continue;
            double segmentStartTime = bpmTime;
            if ( i == 0 && config.visual.drawBeatLinesBeforeFirstTiming ) {
                // 仅首段向前延伸，不能为每个 BPM 段都补一套历史网格。
                // 首 BPM 之前可向前延伸，但拍位仍以首时间点为相位基准。
                segmentStartTime = startTime;
            }
            if ( segmentStartTime >= endTime ) continue;

            double startCalcTime = std::max(segmentStartTime, startTime);
            // 直接算首个候选步号，避免从 BPM 起点循环推进到远处视窗。
            int64_t stepOffset = 0;
            if ( startCalcTime > bpmTime ) {
                stepOffset = static_cast<int64_t>(
                    std::ceil((startCalcTime - bpmTime) / stepDuration - 1e-4));
            } else if ( startCalcTime < bpmTime ) {
                stepOffset = static_cast<int64_t>(std::floor(
                    (startCalcTime - bpmTime) / stepDuration + 1e-4));
            }

            double t = bpmTime + stepOffset * stepDuration;
            // 步号估算有浮点容差，再用时间条件剔除明显位于交集之前的候选。
            while ( t < startCalcTime - 1e-4 ) {
                // 校正只推进步号，不改变 BPM 原点和拍位颜色对应关系。
                stepOffset++;
                t = bpmTime + stepOffset * stepDuration;
            }
            while ( t < nextBpmTime && t <= endTime ) {
                // 下一 BPM 时刻交给下一段生成，避免不同拍长重复生成边界线。
                int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                if ( beatIndex < 0 ) beatIndex += beatDivisor;
                // 首 BPM 前的负步号转换到正拍内索引，颜色约分规则保持一致。
                // 整拍使用分母 1；例如 4 分拍的第 2 步仍取 2 分拍样式。
                int denominator = 1;
                if ( beatIndex != 0 ) {
                    int gcd     = std::gcd(beatIndex, beatDivisor);
                    denominator = beatDivisor / gcd;
                }

                float y =
                    judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                        t, currentAbsY, t)) *
                                        renderScaleY;

                float cursorRevealAlpha = 1.0f;
                if ( revealNearCursor ) {
                    cursorRevealAlpha = calculateCursorRevealAlpha(
                        std::abs(y - cursorY),
                        viewportHeight,
                        config.visual.beatLineCursorVisibleRatio,
                        config.visual.beatLineCursorFadeRatio);
                    if ( cursorRevealAlpha <= 0.0f ) {
                        // 全透明线不占据像素行，后续可见线仍有机会绘制。
                        stepOffset++;
                        t = bpmTime + stepOffset * stepDuration;
                        continue;
                    }
                }

                if ( y >= visibleTop && y <= visibleBottom && occupyRow(y) ) {
                    auto [color, width] = getBeatLineConfig(denominator);
                    color.a *= cursorRevealAlpha;
                    // 按时间匹配吸附，避免压缩视图中误亮同像素行的相邻拍位。
                    const bool glow =
                        batcher.snapshot->isSnapped &&
                        std::abs(t - batcher.snapshot->snappedTime) < 1e-6;
                    // 只替换严格位于拍内的旧线，保留两端整拍边界作为定位参照。
                    const bool replaceTargetTrack =
                        hasSubdivisionPreview &&
                        t > subdivisionPreview.beatStartTime + 1e-6 &&
                        t < subdivisionPreview.beatEndTime - 1e-6;
                    if ( replaceTargetTrack ) {
                        // 残段沿用同一 alpha 与吸附状态，不改变其他轨道的提示。
                        // 只移除目标轨道拍内旧线，左右其他轨道保留原分拍网格。
                        drawBeatLineSegment(leftX,
                                            subdivisionLeft - leftX,
                                            y,
                                            width,
                                            color,
                                            glow);
                        drawBeatLineSegment(
                            subdivisionRight,
                            leftX + trackAreaW - subdivisionRight,
                            y,
                            width,
                            color,
                            glow);
                    } else {
                        drawBeatLineSegment(
                            leftX, trackAreaW, y, width, color, glow);
                    }
                    // 行表已满时再生成候选也不会增加可见线，提前结束整个绘制。
                    if ( occupiedRowCount >= rowCount ) return;
                }
                // 从 BPM 原点与整数步号重算，避免长时间逐次相加累积漂移。
                stepOffset++;
                t = bpmTime + stepOffset * stepDuration;
            }
        }
    }
}

/// @brief 绘制缓存段落上记录的 BPM、SV、Jump 和 HS 时间点标记。
/// @param batcher 当前快照图元输出器。
/// @param viewportHeight 保留的视口高度参数，裁剪以 topY/bottomY 为准。
/// @param judgmentLineY 显示原点对应的纵坐标。
/// @param config 保留的配置参数，时间点类别颜色由本实现固定选择。
/// @param cache 提供原始段落位置与事件位标记，空指针时不绘制。
/// @param currentTime 用于取得本帧视觉锚点的时间。
/// @param leftX 标记线起点横坐标。
/// @param topY 可见区域的一侧纵向边界。
/// @param bottomY 可见区域另一侧纵向边界，允许顺序反转。
/// @param trackAreaW 时间点标记线的水平跨度。
/// @param renderScaleY 滚动距离到画布纵坐标的倍率。
/// @note 相同像素行保留先遇到的时间点，颜色由段内效果优先级决定。
/// @note 效果位只决定颜色，不在渲染时执行 BPM、Jump 或 HS 变换。
/// @pre 缓存段落位置与动画缩放来自同一有效快照周期。
/// @note 本函数同样创建局部占行向量，并非无分配路径。
/// @warning 快照生成路径遍历缓存段落，不查询或排序时间线实体。
void NoteRenderSystem::drawTimingLines(Batcher& batcher, float viewportHeight,
                                       float judgmentLineY,
                                       const Config::EditorConfig& config,
                                       double                      currentTime,
                                       const ScrollCache* cache, float leftX,
                                       float topY, float bottomY,
                                       float trackAreaW, float renderScaleY)
{
    if ( !cache ) return;

    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    if ( std::abs(renderScaleY) < 1e-6f ) return;
    batcher.setTexture(TextureID::None);

    float visibleTop    = std::min(topY, bottomY);
    float visibleBottom = std::max(topY, bottomY);
    int   rowCount      = std::max(
        1, static_cast<int>(std::ceil(visibleBottom - visibleTop)) + 1);
    std::vector<uint8_t> occupiedRows(static_cast<size_t>(rowCount), 0);
    int                  occupiedRowCount = 0;
    /// @brief 将命中的时间点登记到像素行，限制同一行只提交一条标记。
    /// @param y 时间点标记中心位置。
    /// @return 首次占用可见像素行时为 true，否则不再生成几何。
    /// @note 此表独立于分拍网格，时间点标记不会被普通分拍线占行过滤。
    auto occupyRow = [&](float y) {
        if ( y < visibleTop || y > visibleBottom ) return false;
        int row = static_cast<int>(std::floor(y - visibleTop));
        row     = std::clamp(row, 0, rowCount - 1);
        if ( occupiedRows[static_cast<size_t>(row)] != 0 ) return false;
        occupiedRows[static_cast<size_t>(row)] = 1;
        ++occupiedRowCount;
        return true;
    };

    for ( const auto& seg : cache->getSegments() ) {
        // 遍历顺序决定同像素行保留项，不按颜色或事件类别重新排序。
        // 初始积分段可能不对应真实时间点，没有效果位时不画标记。
        if ( seg.effects == 0 ) continue;

        const double segmentAbsY = seg.absY * cache->getAnimatedZoomScale();
        // 段落存未动画缩放的位置，先统一空间再减锚点并应用该段 HS。
        float y = judgmentLineY -
                  static_cast<float>((segmentAbsY - currentAbsY) * seg.hs) *
                      renderScaleY;

        if ( occupyRow(y) ) {
            // 未识别的非零效果位仍显示默认白色，不把未知类别完全隐藏。
            glm::vec4 color = { 1.0f, 1.0f, 1.0f, 0.5f };
            // 同段复合效果优先使用 BPM+SV 色，其余按下列顺序选择单一颜色。
            if ( (seg.effects & SCROLL_EFFECT_BPM) &&
                 (seg.effects & SCROLL_EFFECT_SCROLL) ) {
                color = { 1.0f, 0.5f, 0.0f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
                color = { 1.0f, 0.2f, 0.2f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_JUMP ) {
                // 非 BPM 复合事件优先突出 Jump，其后才是 HS 和普通滚速。
                color = { 0.2f, 0.45f, 1.0f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_HS ) {
                color = { 1.0f, 0.85f, 0.2f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
                color = { 0.2f, 1.0f, 0.2f, 0.8f };
            }

            batcher.pushQuad(leftX, y + 1.0f, trackAreaW, 2.0f, color);
            // 标记贯穿传入区域，其宽度不随时间段的速度大小变化。
            // 两像素标记围绕 y 居中，占行仍只按中心计算，不合并相邻粗线覆盖。
            if ( occupiedRowCount >= rowCount ) return;
        }
    }
}

}  // namespace MMM::Logic::System
