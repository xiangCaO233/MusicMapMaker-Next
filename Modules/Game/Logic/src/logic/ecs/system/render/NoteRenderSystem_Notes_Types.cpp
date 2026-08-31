#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

static glm::vec2 getDrawSize(RenderSnapshot* snapshot, TextureID id,
                             float baseW, float baseH)
{
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return { baseW, baseH };

    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return { baseW, baseH };

    float wRatio = it->second.z / itBase->second.z;
    float hRatio = it->second.w / itBase->second.w;

    return { baseW * wRatio, baseH * hRatio };
}

static float getTexAspect(RenderSnapshot* snapshot, TextureID id)
{
    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return 1.0f;
    return it->second.z / it->second.w;
}

void NoteRenderSystem::renderTap(Batcher&                           batcher,
                                 const ::MMM::Logic::NoteComponent& note,
                                 const Config::EditorConfig& config, float x,
                                 float y, float w, float h, float aspect,
                                 glm::vec4 color)
{
    batcher.setTexture(TextureID::Note);
    batcher.pushFilledQuad(x,
                           y + h * 0.5f,
                           w,
                           h,
                           { aspect, 1.0f },
                           config.visual.noteFillMode,
                           color);
}

void NoteRenderSystem::renderHold(
    Batcher& batcher, const ::MMM::Logic::NoteComponent& note,
    const Config::EditorConfig& config, RenderSnapshot* snapshot, float x,
    float w, float h, float singleTrackW, glm::vec4 headColor,
    glm::vec4 bodyColor, glm::vec4 endColor, const ScrollCache* cache,
    double currentAbsY, float judgmentLineY, float renderScaleY, float topY,
    float bottomY, HoverPart glowPart)
{
    // 三种纹理分别按 Note 基准尺寸换算，裁剪不得改变其横向比例。
    // 主体宽度仅由皮肤 UV 和 noteScaleX 决定，与 SV 数值完全无关。
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, w, h);
    glm::vec2 endSize  = getDrawSize(snapshot, TextureID::HoldEnd, w, h);
    glm::vec2 bodySize =
        getDrawSize(snapshot, TextureID::HoldBodyVertical, w, h);

    // 头尾与主体各自在轨道内居中，左右边界不参与纵向裁剪。
    // 固定 X 可保证裁剪后的四边形仍是等宽矩形。
    float headX = x;
    float endX  = x + (w - endSize.x) * 0.5f;
    float bodyX = x + (w - bodySize.x) * 0.5f;
    // 结束时间保持 double，避免长持续时间在投影前损失精度。
    const double holdEndTime = note.m_timestamp + note.m_duration;

    // 保留 double 投影直到裁剪完成，避免极大 SV 先转换为超大 float。
    // 头部锚点使用自身时间，确保跨 Timing 段时选择正确积分基准。
    // renderScaleY 在 double 域中相乘，避免中间结果先降为 float。
    const double headY =
        judgmentLineY - cache->getDisplayDelta(
                            note.m_timestamp, currentAbsY, note.m_timestamp) *
                            static_cast<double>(renderScaleY);
    // 尾部沿用 Hold 起点作为显示锚点，与既有滚动语义保持一致。
    const double endY =
        judgmentLineY -
        cache->getDisplayDelta(holdEndTime, currentAbsY, note.m_timestamp) *
            static_cast<double>(renderScaleY);
    // 调用方可能提供翻转边界，先规整为递增区间再执行 clamp。
    // 使用实际轨道上下边界而非硬编码窗口高度，兼容主画布与预览缩放。
    const float clipTop    = std::min(topY, bottomY);
    const float clipBottom = std::max(topY, bottomY);

    // 1. 连接体只向 Batcher 提交视口内坐标，防止超长三角形触发光栅精度异常。
    if ( (glowPart == HoverPart::None || glowPart == HoverPart::HoldBody) &&
         std::isfinite(headY) && std::isfinite(endY) ) {
        // 非有限投影不会进入 Batcher，避免污染整个顶点缓冲。
        // 起点与终点分别裁剪，正向和负向滚动都能保留可见跨度。
        // 仅在范围收敛后转换为 float，保证提交坐标接近视口量级。
        const float clippedHeadY =
            static_cast<float>(std::clamp(headY,
                                          static_cast<double>(clipTop),
                                          static_cast<double>(clipBottom)));
        // 尾部使用相同边界，主体两侧因此始终共享同一组 Y。
        const float clippedEndY =
            static_cast<float>(std::clamp(endY,
                                          static_cast<double>(clipTop),
                                          static_cast<double>(clipBottom)));
        // 完全离屏的连接体会折叠到同一边界，不生成退化三角形。
        if ( std::abs(clippedHeadY - clippedEndY) > 1e-4f ) {
            // 纹理状态只在确有可见主体时切换，避免产生空绘制指令。
            batcher.setTexture(TextureID::HoldBodyVertical);
            // 四个顶点只组合固定 X 与裁剪 Y，不会形成宽度变化或斜边。
            batcher.pushFreeQuad({ bodyX, clippedHeadY },
                                 { bodyX + bodySize.x, clippedHeadY },
                                 { bodyX + bodySize.x, clippedEndY },
                                 { bodyX, clippedEndY },
                                 bodyColor);
        }
    }

    // 固定尺寸端点只在与视口相交时提交，离屏端点不再携带超大坐标。
    const auto isEndpointVisible = [clipTop, clipBottom](double y, float size) {
        // 端点按中心与半高判断相交，边缘露出时仍应完整交给 Scissor 裁剪。
        const double halfSize = static_cast<double>(size) * 0.5;
        // 有限性检查先于 float 转换，离屏超大值也不会进入顶点数据。
        return std::isfinite(y) && y + halfSize >= clipTop &&
               y - halfSize <= clipBottom;
    };

    // 2. 头部。
    if ( (glowPart == HoverPart::None || glowPart == HoverPart::Head) &&
         isEndpointVisible(headY, headSize.y) ) {
        // 可见头部保持原始中心坐标，不对固定尺寸纹理进行拉伸。
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(
            headX,
            static_cast<float>(headY) + headSize.y * 0.5f,
            headSize.x,
            headSize.y,
            { getTexAspect(snapshot, TextureID::Note), 1.0f },
            config.visual.noteFillMode,
            headColor);
    }

    // 3. 尾部。
    if ( (glowPart == HoverPart::None || glowPart == HoverPart::HoldEnd) &&
         isEndpointVisible(endY, endSize.y) ) {
        // 可见尾部同样保持皮肤尺寸，只有完全离屏时才跳过。
        batcher.setTexture(TextureID::HoldEnd);
        batcher.pushFilledQuad(
            endX,
            static_cast<float>(endY) + endSize.y * 0.5f,
            endSize.x,
            endSize.y,
            { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
            config.visual.noteFillMode,
            endColor);
    }
}

void NoteRenderSystem::renderFlick(Batcher&                           batcher,
                                   const ::MMM::Logic::NoteComponent& note,
                                   const Config::EditorConfig&        config,
                                   RenderSnapshot* snapshot, float x, float y,
                                   float w, float h, float singleTrackW,
                                   glm::vec4 headColor, glm::vec4 bodyColor,
                                   glm::vec4 arrowColor, HoverPart glowPart)
{
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, w, h);
    float     headX    = x;

    // 1. 横向连接体。
    if ( note.m_dtrack != 0 &&
         (glowPart == HoverPart::None || glowPart == HoverPart::HoldBody) ) {
        auto itBodyH = snapshot->uvMap.find(
            static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
        if ( itBodyH != snapshot->uvMap.end() ) {
            float drawH = h * (itBodyH->second.w /
                               snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
            float drawW = std::abs(note.m_dtrack) * singleTrackW;
            float startTrack = std::min(0.0f, (float)note.m_dtrack);
            float bodyX      = x + (w - singleTrackW) * 0.5f +
                               startTrack * singleTrackW + singleTrackW * 0.5f;

            batcher.setTexture(TextureID::HoldBodyHorizontal);
            batcher.pushQuad(bodyX, y + drawH * 0.5f, drawW, drawH, bodyColor);
        }
    }

    // 2. 头部。
    if ( glowPart == HoverPart::None || glowPart == HoverPart::Head ) {
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(
            headX,
            y + headSize.y * 0.5f,
            headSize.x,
            headSize.y,
            { getTexAspect(snapshot, TextureID::Note), 1.0f },
            config.visual.noteFillMode,
            headColor);
    }

    // 3. 箭头。
    if ( note.m_dtrack != 0 &&
         (glowPart == HoverPart::None || glowPart == HoverPart::FlickArrow) ) {
        TextureID arrowId   = (note.m_dtrack < 0) ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
        glm::vec2 arrowSize = getDrawSize(snapshot, arrowId, w, h);
        float     arrowX    = x + (w - singleTrackW) * 0.5f +
                              note.m_dtrack * singleTrackW +
                              (singleTrackW - arrowSize.x) * 0.5f;
        batcher.setTexture(arrowId);
        batcher.pushFilledQuad(arrowX,
                               y + arrowSize.y * 0.5f,
                               arrowSize.x,
                               arrowSize.y,
                               { getTexAspect(snapshot, arrowId), 1.0f },
                               config.visual.noteFillMode,
                               arrowColor);
    }
}

}  // namespace MMM::Logic::System
