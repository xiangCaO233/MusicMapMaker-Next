#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <numeric>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

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
    if ( l >= r ) {
        r = l + 0.01f;
    }
    if ( t >= b ) {
        b = t + 0.01f;
    }

    leftX        = viewportWidth * l;
    rightX       = viewportWidth * r;
    topY         = viewportHeight * t;
    bottomY      = viewportHeight * b;
    trackAreaW   = rightX - leftX;
    singleTrackW = trackAreaW / std::max(1.0f, static_cast<float>(trackCount));

    // 2. 绘制轨道底板
    NoteRenderSystem::drawTrackBackground(
        batcher, trackCount, leftX, topY, bottomY, singleTrackW);

    // 3. 绘制轨道包围框
    batcher.setTexture(TextureID::None);
    batcher.pushStrokeRect(leftX,
                           topY,
                           rightX,
                           bottomY,
                           config.visual.trackBoxLineWidth,
                           { 0.5f, 0.5f, 0.5f, 1.0f });

    // 4. 绘制判定区域
    NoteRenderSystem::drawJudgmentArea(batcher,
                                       trackCount,
                                       leftX,
                                       judgmentLineY,
                                       singleTrackW,
                                       trackAreaW,
                                       config);
}

void NoteRenderSystem::drawTrackBackground(Batcher& batcher, int32_t trackCount,
                                           float leftX, float topY,
                                           float bottomY, float singleTrackW)
{
    if ( trackCount <= 0 || singleTrackW <= 0.001f ) return;

    batcher.setTexture(TextureID::Track);
    auto uvIt =
        batcher.snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Track));
    if ( uvIt == batcher.snapshot->uvMap.end() ) return;

    float texW_px = uvIt->second.z * 2048.0f;
    float texH_px = uvIt->second.w * 2048.0f;

    if ( texW_px <= 0 || texH_px <= 0 ) return;

    float texAspect = texW_px / texH_px;
    float drawH     = singleTrackW / texAspect;

    // 严防无限循环：若平铺高度过小，则跳过绘制
    if ( drawH <= 1.0f ) return;

    const float halfPixelU = 0.5f / 2048.0f;
    const float halfPixelV = 0.5f / 2048.0f;

    float uMin = uvIt->second.x + halfPixelU;
    float uMax = uvIt->second.x + uvIt->second.z - halfPixelU;

    for ( int i = 0; i < trackCount; ++i ) {
        float trackX   = leftX + i * singleTrackW;
        float currentY = bottomY;
        float drawW    = singleTrackW + 0.5f;

        while ( currentY > topY ) {
            float remainH     = currentY - topY;
            float actualDrawH = std::min(drawH, remainH);

            float vMax = 1.0f;
            float vMin = 1.0f - (actualDrawH / drawH);

            float finalVMin =
                uvIt->second.y + vMin * uvIt->second.w + halfPixelV;
            float finalVMax =
                uvIt->second.y + vMax * uvIt->second.w - halfPixelV;

            batcher.pushUVQuad(trackX,
                               currentY,
                               drawW,
                               actualDrawH + 0.5f,
                               glm::vec2(uMin, finalVMin),
                               glm::vec2(uMax, finalVMax),
                               { 1.0f, 1.0f, 1.0f, 1.0f });

            currentY -= drawH;
        }
    }
}

void NoteRenderSystem::drawJudgmentArea(Batcher& batcher, int32_t trackCount,
                                        float leftX, float judgmentLineY,
                                        float singleTrackW, float trackAreaW,
                                        const Config::EditorConfig& config)
{
    batcher.setTexture(TextureID::JudgeArea);
    auto judgeUvIt = batcher.snapshot->uvMap.find(
        static_cast<uint32_t>(TextureID::JudgeArea));

    if ( judgeUvIt != batcher.snapshot->uvMap.end() ) {
        float texW = judgeUvIt->second.z * 2048.0f;
        float texH = judgeUvIt->second.w * 2048.0f;
        if ( texW > 0 && texH > 0 ) {
            float aspect = texW / texH;
            float drawW  = singleTrackW * config.visual.noteScaleX;
            float drawH  = (singleTrackW / aspect) * config.visual.noteScaleY;

            const float halfPixelU = 0.5f / 2048.0f;
            const float halfPixelV = 0.5f / 2048.0f;

            for ( int i = 0; i < trackCount; ++i ) {
                float trackCenterX =
                    leftX + i * singleTrackW + singleTrackW * 0.5f;
                float drawX = trackCenterX - drawW * 0.5f;

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
                    { 1.0f, 1.0f, 1.0f, 1.0f });
            }
        }
    } else {
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(leftX,
                         judgmentLineY + 2.0f * 0.5f,
                         trackAreaW,
                         2.0f,
                         { 1.0f, 1.0f, 1.0f, 1.0f });
    }
}

void NoteRenderSystem::drawBeatLines(
    Batcher& batcher, float viewportHeight, float judgmentLineY,
    const Config::EditorConfig& config, const entt::registry& timelineRegistry,
    double currentTime, const ScrollCache* cache, float leftX, float topY,
    float bottomY, float trackAreaW, float renderScaleY)
{
    if ( !cache ) return;

    int beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tl);
        }
    }
    if ( bpmEvents.empty() ) return;

    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    double currentAbsY = cache->getAbsY(currentTime);
    double startTime   = cache->getTime(
        currentAbsY - (viewportHeight - judgmentLineY) / renderScaleY);
    double endTime = cache->getTime(currentAbsY + judgmentLineY / renderScaleY);

    batcher.setTexture(TextureID::None);

    auto& skin        = Config::SkinManager::instance();
    float globalAlpha = config.visual.beatLineAlpha;
    auto  getBeatLineConfig =
        [&skin, globalAlpha](int denominator) -> std::pair<glm::vec4, float> {
        std::string   key = "beat_lines.beat_" + std::to_string(denominator);
        Config::Color c   = skin.getColor(key);
        if ( c.r == 1.0f && c.g == 0.0f && c.b == 1.0f && c.a == 1.0f ) {
            c   = skin.getColor("beat_lines.default");
            key = "beat_lines_width.default";
        } else {
            key = "beat_lines_width.beat_" + std::to_string(denominator);
        }
        float width =
            skin.getValue(key, skin.getValue("beat_lines_width.default", 2.0f));
        return { glm::vec4(c.r, c.g, c.b, c.a * globalAlpha), width };
    };

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBPM = bpmEvents[i];
        double      bpmTime    = currentBPM->m_timestamp;
        double      bpmVal     = currentBPM->m_value;
        if ( bpmVal <= 0.0 ) {
            bpmVal = 120.0;
            if ( auto session = EditorEngine::instance().getActiveSession() ) {
                if ( auto beatmap = session->getContext().currentBeatmap ) {
                    if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                        bpmVal = beatmap->m_baseMapMetadata.preference_bpm;
                    }
                }
            }
        }

        // 限制极端 BPM 导致的无限循环 (例如 osu! 谱面中的 6E-96 ms_per_beat)
        if ( bpmVal > 10000.0 ) bpmVal = 10000.0;

        double nextBpmTime = (i + 1 < bpmEvents.size())
                                 ? bpmEvents[i + 1]->m_timestamp
                                 : std::numeric_limits<double>::infinity();

        if ( nextBpmTime <= startTime ) continue;
        if ( bpmTime >= endTime ) break;

        double beatDuration = 60.0 / bpmVal;
        double stepDuration = beatDuration / beatDivisor;

        double  startCalcTime = std::max(bpmTime, startTime);
        int64_t stepOffset    = 0;
        if ( startCalcTime > bpmTime ) {
            stepOffset = static_cast<int64_t>(
                std::ceil((startCalcTime - bpmTime) / stepDuration - 1e-4));
        }

        double t = bpmTime + stepOffset * stepDuration;
        while ( t < nextBpmTime && t <= endTime ) {
            int beatIndex   = stepOffset % beatDivisor;
            int denominator = 1;
            if ( beatIndex != 0 ) {
                int gcd     = std::gcd(beatIndex, beatDivisor);
                denominator = beatDivisor / gcd;
            }

            auto [color, width] = getBeatLineConfig(denominator);
            double absY         = cache->getAbsY(t);
            float  y = judgmentLineY -
                       static_cast<float>(absY - currentAbsY) * renderScaleY;

            if ( y >= topY && y <= bottomY ) {
                if ( batcher.snapshot->isSnapped &&
                     std::abs(t - batcher.snapshot->snappedTime) < 1e-6 ) {
                    glm::vec4 glowCol = color;
                    glowCol.a *= 0.6f;
                    batcher.pushQuad(leftX,
                                     y + (width + 4.0f) * 0.5f,
                                     trackAreaW,
                                     width + 4.0f,
                                     glowCol);
                    glowCol.a *= 0.5f;
                    batcher.pushQuad(leftX,
                                     y + (width + 10.0f) * 0.5f,
                                     trackAreaW,
                                     width + 10.0f,
                                     glowCol);
                    glowCol.a *= 0.5f;
                    batcher.pushQuad(leftX,
                                     y + (width + 20.0f) * 0.5f,
                                     trackAreaW,
                                     width + 20.0f,
                                     glowCol);
                }
                batcher.pushQuad(
                    leftX, y + width * 0.5f, trackAreaW, width, color);
            }
            stepOffset++;
            t = bpmTime + stepOffset * stepDuration;
        }
    }
}

void NoteRenderSystem::drawTimingLines(Batcher& batcher, float viewportHeight,
                                       float judgmentLineY,
                                       const Config::EditorConfig& config,
                                       double                      currentTime,
                                       const ScrollCache* cache, float leftX,
                                       float topY, float bottomY,
                                       float trackAreaW, float renderScaleY)
{
    if ( !cache ) return;

    double currentAbsY = cache->getAbsY(currentTime);
    double startTime   = cache->getTime(
        currentAbsY - (viewportHeight - judgmentLineY) / renderScaleY);
    double endTime = cache->getTime(currentAbsY + judgmentLineY / renderScaleY);

    batcher.setTexture(TextureID::None);

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.time < startTime || seg.time > endTime ) continue;
        if ( seg.effects == 0 ) continue;  // 忽略没有效果的段（通常是第0段）

        float y = judgmentLineY -
                  static_cast<float>(seg.absY - currentAbsY) * renderScaleY;

        if ( y >= topY && y <= bottomY ) {
            glm::vec4 color = { 1.0f, 1.0f, 1.0f, 0.5f };
            if ( (seg.effects & SCROLL_EFFECT_BPM) &&
                 (seg.effects & SCROLL_EFFECT_SCROLL) ) {
                color = { 1.0f, 0.5f, 0.0f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
                color = { 1.0f, 0.2f, 0.2f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
                color = { 0.2f, 1.0f, 0.2f, 0.8f };
            }

            batcher.pushQuad(leftX, y + 1.0f, trackAreaW, 2.0f, color);
        }
    }
}

}  // namespace MMM::Logic::System
