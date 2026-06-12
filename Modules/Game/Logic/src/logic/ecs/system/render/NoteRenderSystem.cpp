#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

namespace
{

/// @brief 绘制时间线/预览用的小型判定框。
/// @warning 热路径：每个 Timeline/Preview 快照生成时执行；只推送固定数量几何。
void drawJudgmentGuideBox(Batcher& batcher, float leftX, float centerY,
                          float width, float height)
{
    if ( width <= 0.5f || height <= 0.5f ) return;

    auto& skin        = Config::SkinManager::instance();
    auto  fillColor   = skin.getColor("preview.judgment_guide.fill");
    auto  borderColor = skin.getColor("preview.judgment_guide.border");
    constexpr float strokeWidth = 2.0f;

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
/// registry 全量无缓存扫描或阻塞同步。
void NoteRenderSystem::generateSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    const std::vector<const TimelineComponent*>& bpmEvents,
    RenderSnapshot* snapshot, const std::string& cameraId, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight, HitFXSystem* hitFXSystem)
{
    const bool isMainCanvas = SessionUtils::isMainCanvasCameraId(cameraId);

    // 核心同步：如果预览区正在拖拽，主画布渲染的时间应该是预览区当前的悬停时间
    double renderTime = currentTime;
    if ( isMainCanvas && snapshot->isPreviewDragging ) {
        renderTime = snapshot->previewHoverTime;
    }

    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 指针存入 context 供 renderPolyline 等后续使用
    registry.ctx().erase<const ScrollCache*>();
    registry.ctx().emplace<const ScrollCache*>(cache);

    // Timeline 右键创建事件需要完整映射；其他画布只在播放亚帧插值时需要。
    if ( cameraId == "Timeline" || snapshot->isPlaying ) {
        cache->copyAnimatedSegmentsTo(snapshot->scrollSegments);
    }

    Batcher batcher(snapshot);
    float   leftX = 0, rightX = 0, topY = 0, bottomY = 0, trackAreaW = 0,
          singleTrackW = 0;
    float renderScaleY = 1.0f;

    // --- Phase 1: 静态布局与打击特效预生成 ---
    // 我们需要打击特效绘制在音符上方，但它的顶点位置是相对于判定线的（静态的，不随时间偏移）。
    // 因此，我们在设置 staticVertexCount
    // 之前生成它的顶点，但将其命令延迟到最后插入。
    uint32_t fxCmdStart = static_cast<uint32_t>(snapshot->cmds.size());
    if ( hitFXSystem && (isMainCanvas || cameraId == "Preview") ) {
        // 提前计算轨道参数
        float tempLX = 0, tempRX = 0;
        if ( isMainCanvas ) {
            tempLX = viewportWidth * config.visual.trackLayout.left;
            tempRX = viewportWidth * config.visual.trackLayout.right;
        } else {
            tempLX = config.visual.previewConfig.margin.left;
            tempRX = viewportWidth - config.visual.previewConfig.margin.right;
        }
        float tempSTW = (tempRX - tempLX) / static_cast<float>(trackCount);

        hitFXSystem->generateSnapshot(batcher,
                                      renderTime,
                                      config,
                                      trackCount,
                                      judgmentLineY,
                                      tempLX,
                                      tempSTW);
    }
    uint32_t fxCmdEnd = static_cast<uint32_t>(snapshot->cmds.size());

    // 提取并暂存打击特效命令
    std::vector<UI::BrushDrawCmd> deferredHitCmds;
    if ( fxCmdEnd > fxCmdStart ) {
        deferredHitCmds.assign(snapshot->cmds.begin() + fxCmdStart,
                               snapshot->cmds.end());
        snapshot->cmds.erase(snapshot->cmds.begin() + fxCmdStart,
                             snapshot->cmds.end());
    }

    // 正常生成基础布局
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
                                                     cache,
                                                     leftX,
                                                     rightX,
                                                     topY,
                                                     bottomY,
                                                     trackAreaW,
                                                     singleTrackW,
                                                     renderScaleY);

        if ( snapshot->isHoveringCanvas || snapshot->isPreviewDragging ) {
            double hoveredTime = snapshot->isPreviewDragging
                                     ? snapshot->previewHoverTime
                                     : snapshot->hoveredTime;

            if ( !bpmEvents.empty() ) {
                int64_t totalBeats = 0;
                bool    found      = false;
                for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                    const auto* currentBPM = bpmEvents[i];
                    double      bpmTime    = currentBPM->m_timestamp;
                    double      bpmVal     = currentBPM->m_value;
                    if ( bpmVal <= 0.0 ) {
                        bpmVal = 120.0;
                        if ( auto session =
                                 EditorEngine::instance().getActiveSession() ) {
                            if ( auto beatmap =
                                     session->getContext().currentBeatmap ) {
                                if ( beatmap->m_baseMapMetadata.preference_bpm >
                                     0.0 ) {
                                    bpmVal = beatmap->m_baseMapMetadata
                                                 .preference_bpm;
                                }
                            }
                        }
                    }
                    if ( bpmVal > 10000.0 ) bpmVal = 10000.0;

                    double nextBpmTime =
                        (i + 1 < bpmEvents.size())
                            ? bpmEvents[i + 1]->m_timestamp
                            : std::numeric_limits<double>::infinity();

                    if ( hoveredTime >= bpmTime && hoveredTime < nextBpmTime ) {
                        double  beatDuration = 60.0 / bpmVal;
                        double  timeInBpm    = hoveredTime - bpmTime;
                        int64_t beatsInBpm   = static_cast<int64_t>(
                            std::floor(timeInBpm / beatDuration + 1e-6));
                        snapshot->hoveredBeatIndex =
                            static_cast<int>(totalBeats + beatsInBpm + 1);
                        found = true;
                        break;
                    } else if ( hoveredTime >= nextBpmTime ) {
                        double beatDuration = 60.0 / bpmVal;
                        double bpmDuration  = nextBpmTime - bpmTime;
                        totalBeats += static_cast<int64_t>(
                            std::round(bpmDuration / beatDuration));
                    } else {
                        break;
                    }
                }
                if ( !found ) snapshot->hoveredBeatIndex = 0;
            }
        }
    }

    // 记录静态边界 (此时 snapshot->vertices 包含了特效和布局的顶点)
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    // --- Phase 2: 动态内容生成 (拍线、音符等) ---
    // 这些内容会受到 UI 线程 yOffset 补偿的影响，从而消除亚帧抖动
    snapshot->trackCount   = trackCount;
    snapshot->renderScaleY = renderScaleY;

    if ( cameraId != "Timeline" ) {
        // 先绘制拍线，使其在物件下方
        bool shouldDrawBeatLines   = config.visual.drawBeatLines;
        bool shouldDrawTimingLines = false;

        if ( cameraId == "Preview" ) {
            // 预览区逻辑：若全局开启，则由预览区具体开关决定；若全局关闭，则强制关闭
            shouldDrawBeatLines = config.visual.drawBeatLines &&
                                  config.visual.previewConfig.drawBeatLines;
            shouldDrawTimingLines = config.visual.previewConfig.drawTimingLines;
        }

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
                                            renderScaleY);
        }

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

        batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
        NoteRenderSystem::renderNotes(registry,
                                      snapshot,
                                      cameraId,
                                      renderTime,
                                      judgmentLineY,
                                      trackCount,
                                      config,
                                      batcher,
                                      leftX,
                                      rightX,
                                      topY,
                                      bottomY,
                                      singleTrackW,
                                      renderScaleY);
        if ( cameraId == "Preview" ) {
            float lx = config.visual.previewConfig.margin.left;
            float rx = viewportWidth - config.visual.previewConfig.margin.right;
            float ty = config.visual.previewConfig.margin.top;
            float by =
                viewportHeight - config.visual.previewConfig.margin.bottom;
            batcher.setScissor(lx, ty, rx - lx, by - ty);
        } else {
            batcher.setScissor(leftX,
                               -viewportHeight * 0.5f,
                               trackAreaW,
                               viewportHeight * 2.0f);
        }
        if ( isMainCanvas && config.visual.debugDrawHitboxes ) {
            NoteRenderSystem::debugRenderHitboxes(batcher, snapshot);
        }
    }

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

    // 记录动态顶点数量
    snapshot->dynamicVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size()) -
        snapshot->staticVertexCount;

    // --- Phase 3: 置顶层渲染 (静态或动态) ---
    // 将之前生成的打击特效命令插入到最后，使其绘制在物件上方
    if ( !deferredHitCmds.empty() ) {
        snapshot->cmds.insert(snapshot->cmds.end(),
                              deferredHitCmds.begin(),
                              deferredHitCmds.end());
    }

    // 预览区包围盒：用户要求作为静态元素且在最上层
    if ( cameraId == "Preview" ) {
        auto& skin       = Config::SkinManager::instance();
        auto  boxCol     = skin.getColor("preview.boundingbox");
        bool  isDragging = snapshot->isPreviewDragging;

        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float boxDrawH = mainEffectiveH * renderScaleY;

        // 1. [展示中] 始终绘制当前主视窗位置的包围盒 (除非正在拖拽)
        if ( !isDragging ) {
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
        if ( snapshot->isPreviewHovered || isDragging ) {
            float hoverBoxBottom =
                snapshot->previewHoverY + (config.visual.trackLayout.bottom -
                                           config.visual.judgeline_pos) *
                                              mainViewportHeight * renderScaleY;

            auto hoverBoxCol = skin.getColor("preview.hoverbox");
            if ( hoverBoxCol.r == 1.0f && hoverBoxCol.g == 0.0f &&
                 hoverBoxCol.b == 1.0f ) {
                hoverBoxCol = { 1.0f, 1.0f, 0.6f, 0.3f };
            }
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
        batcher.flush();
        drawJudgmentGuideBox(batcher, leftX, judgmentLineY, trackAreaW, 18.0f);
    }

    batcher.flush();
}

void NoteRenderSystem::renderMarqueeBox(
    Batcher& batcher, const RenderSnapshot::MarqueeBoxSnapshot& box,
    float judgmentLineY, float leftX, float singleTrackW, float renderScaleY,
    const ScrollCache* cache, double renderTime, float viewportWidth,
    float viewportHeight)
{
    float x1 = leftX + box.startTrack * singleTrackW;
    float x2 = leftX + box.endTrack * singleTrackW;

    double currentAbsY = cache->getAbsY(renderTime);
    double startAbsY   = cache->getAbsY(box.startTime);
    double endAbsY     = cache->getAbsY(box.endTime);

    float y1 = judgmentLineY -
               static_cast<float>(startAbsY - currentAbsY) * renderScaleY;
    float y2 = judgmentLineY -
               static_cast<float>(endAbsY - currentAbsY) * renderScaleY;

    float left   = std::min(x1, x2);
    float right  = std::max(x1, x2);
    float top    = std::min(y1, y2);
    float bottom = std::max(y1, y2);
    float w      = right - left;
    float h      = bottom - top;

    if ( w < 1.0f || h < 1.0f ) return;

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    float borderW  = settings.marqueeThickness;
    float cornerR  = settings.marqueeRounding;

    // 重置 scissor 到全屏，确保框选矩形不被轨道裁剪掉
    batcher.setScissor(0, 0, viewportWidth, viewportHeight);
    batcher.setTexture(TextureID::None);

    // 绘制半透明填充
    glm::vec4 fillCol   = { 0.2f, 0.6f, 1.0f, 0.15f };
    glm::vec4 strokeCol = { 0.3f, 0.7f, 1.0f, 0.85f };

    batcher.pushRoundedQuad(left, bottom, w, h, cornerR, fillCol);
    batcher.pushRoundedStrokeRect(
        left, bottom, w, h, cornerR, borderW, strokeCol);
}

void NoteRenderSystem::generateTimelineSnapshot(
    RenderSnapshot*                              snapshot,
    const std::vector<const TimelineComponent*>& bpmEvents, Batcher& batcher,
    double currentTime, float viewportWidth, float viewportHeight,
    float judgmentLineY, const Config::EditorConfig& config,
    const ScrollCache* cache)
{
    if ( !snapshot->hasBeatmap ) return;

    batcher.setTexture(TextureID::None);

    // 绘制背景 (确保全覆盖，消除透明混合带来的边缘可疑像素)
    batcher.pushQuad(
        0, viewportHeight, viewportWidth, viewportHeight, { 0, 0, 0, 0.01f });

    auto& skin    = Config::SkinManager::instance();
    auto  tickCol = skin.getColor("timeline.tick");

    double currentAbsY = cache->getAbsY(currentTime);

    float paddingX = 30.0f;
    float lineW    = std::max(1.0f, viewportWidth - paddingX * 2.0f);

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    auto getBeatLineConfig = [&](int denominator) {
        if ( denominator <= 0 ) denominator = 1;
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
        return std::pair{
            glm::vec4(c.r, c.g, c.b, c.a * config.visual.beatLineAlpha), width
        };
    };

    // 3. 绘制 Timeline 自身的分拍线
    /// @brief 绘制 Timeline 自身的分拍线；bpmEvents 由 SessionContext
    /// 脏标记缓存维护，避免热路径完整遍历和排序。
    int beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    if ( !bpmEvents.empty() ) {
        double topAbsY       = currentAbsY + judgmentLineY;
        double bottomAbsY    = currentAbsY + judgmentLineY - viewportHeight;
        auto   visibleRanges = cache->getTimeRangesForAbsYWindow(
            std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

        batcher.setTexture(TextureID::None);
        for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
            const auto* currentBPM = bpmEvents[i];
            double      bpmTime    = currentBPM->m_timestamp;
            double      bpmVal     = currentBPM->m_value;
            if ( bpmVal <= 0.0 ) {
                bpmVal = 120.0;
                if ( auto session =
                         EditorEngine::instance().getActiveSession() ) {
                    if ( auto beatmap = session->getContext().currentBeatmap ) {
                        if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                            bpmVal = beatmap->m_baseMapMetadata.preference_bpm;
                        }
                    }
                }
            }
            if ( bpmVal > 10000.0 ) bpmVal = 10000.0;

            double nextBpmTime  = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();
            double beatDuration = 60.0 / bpmVal;
            double stepDuration = beatDuration / beatDivisor;

            for ( const auto& [startTime, endTime] : visibleRanges ) {
                if ( nextBpmTime <= startTime ) continue;
                double segmentStartTime = bpmTime;
                if ( i == 0 && config.visual.drawBeatLinesBeforeFirstTiming ) {
                    segmentStartTime = startTime;
                }
                if ( segmentStartTime >= endTime ) continue;

                double  startCalcTime = std::max(segmentStartTime, startTime);
                int64_t stepOffset    = 0;
                if ( startCalcTime > bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::ceil(
                        (startCalcTime - bpmTime) / stepDuration - 1e-4));
                } else if ( startCalcTime < bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::floor(
                        (startCalcTime - bpmTime) / stepDuration + 1e-4));
                }

                double t = bpmTime + stepOffset * stepDuration;
                while ( t < startCalcTime - 1e-4 ) {
                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
                while ( t < nextBpmTime && t <= endTime ) {
                    int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                    if ( beatIndex < 0 ) beatIndex += beatDivisor;
                    int denominator = 1;
                    if ( beatIndex != 0 ) {
                        int gcd     = std::gcd(beatIndex, beatDivisor);
                        denominator = beatDivisor / gcd;
                    }

                    auto [color, width] = getBeatLineConfig(denominator);
                    float y             = judgmentLineY -
                              static_cast<float>(
                                  cache->getDisplayDelta(t, currentAbsY, t));
                    if ( y >= 0.0f && y <= viewportHeight ) {
                        color.a *= 0.75f;
                        batcher.setTexture(TextureID::None);
                        batcher.pushQuad(
                            paddingX, y + width * 0.5f, lineW, width, color);
                    }

                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
            }
        }
    }

    // 5. 绘制 Timing 事件为普通 Note 形状。
    float noteW = lineW;
    float noteH = noteW * 0.36f;
    if ( auto uvIt =
             snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
         uvIt != snapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
        noteH = noteW * (uvIt->second.w / uvIt->second.z);
    }
    float noteX = paddingX;

    int markerRows =
        std::max(1, static_cast<int>(std::ceil(viewportHeight)) + 1);
    std::vector<uint8_t> occupiedMarkerRows(static_cast<size_t>(markerRows), 0);
    auto                 occupyMarkerRow = [&](float y) {
        if ( y < 0.0f || y > viewportHeight ) return false;
        int row = static_cast<int>(std::floor(y));
        row     = std::clamp(row, 0, markerRows - 1);
        if ( occupiedMarkerRows[static_cast<size_t>(row)] != 0 ) {
            return false;
        }
        occupiedMarkerRows[static_cast<size_t>(row)] = 1;
        return true;
    };

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.effects == 0 ) continue;

        const double segmentAbsY = seg.absY * cache->getAnimatedZoomScale();
        float        y           = judgmentLineY -
                  static_cast<float>((segmentAbsY - currentAbsY) * seg.hs);

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
        snapshot->timelineElements.push_back(el);
        size_t interactiveElementIdx = snapshot->timelineElements.size() - 1;

        if ( !occupyMarkerRow(y) ) continue;

        glm::vec4 color = { tickCol.r, tickCol.g, tickCol.b, 0.8f };
        if ( (seg.effects & SCROLL_EFFECT_BPM) &&
             (seg.effects & SCROLL_EFFECT_SCROLL) ) {
            color = { 1.0f, 0.5f, 0.0f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
            color = { 1.0f, 0.2f, 0.2f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_JUMP ) {
            color = { 0.2f, 0.45f, 1.0f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_HS ) {
            color = { 1.0f, 0.85f, 0.2f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            color = { 0.2f, 1.0f, 0.2f, 0.8f };
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
        auto& element = snapshot->timelineElements[interactiveElementIdx];
        element.hasMarkerGeometry  = true;
        element.markerVertexOffset = markerVertexOffset;
        element.markerVertexCount  = static_cast<uint32_t>(
            snapshot->vertices.size() - markerVertexOffset);
        element.markerIndexOffset = markerIndexOffset;
        element.markerIndexCount =
            static_cast<uint32_t>(snapshot->indices.size() - markerIndexOffset);
    }

    // 6. 绘制当前时间判定框，作为时间线最上层覆盖物。
    batcher.flush();
    drawJudgmentGuideBox(batcher, paddingX, judgmentLineY, lineW, 18.0f);
}

void NoteRenderSystem::generatePreviewSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float& renderScaleY)
{
    if ( !snapshot->hasBeatmap ) return;

    leftX        = config.visual.previewConfig.margin.left;
    rightX       = viewportWidth - config.visual.previewConfig.margin.right;
    topY         = config.visual.previewConfig.margin.top;
    bottomY      = viewportHeight - config.visual.previewConfig.margin.bottom;
    trackAreaW   = rightX - leftX;
    singleTrackW = trackAreaW / static_cast<float>(trackCount);

    float mainEffectiveH =
        (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
        mainViewportHeight;

    float previewDrawH = bottomY - topY;

    renderScaleY =
        previewDrawH / (mainEffectiveH * config.visual.previewConfig.areaRatio);

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());
}

void NoteRenderSystem::generateMainCanvasSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    const ScrollCache* cache, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float renderScaleY)
{
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    // 设置轨道区域裁剪
    float lx = viewportWidth * config.visual.trackLayout.left;
    float rx = viewportWidth * config.visual.trackLayout.right;
    // 扩展垂直方向的裁剪区域，给予上下各 0.5 倍视口的余量
    batcher.setScissor(
        lx, -viewportHeight * 0.5f, rx - lx, viewportHeight * 2.0f);

    if ( !snapshot->hasBeatmap ) {
        batcher.setTexture(TextureID::Logo);
        float logoSize = std::min(viewportWidth, viewportHeight) * 0.4f;
        float cx       = viewportWidth * 0.5f;
        float cy       = viewportHeight * 0.5f;
        batcher.pushQuad(cx - logoSize * 0.5f,
                         cy + logoSize * 0.5f,
                         logoSize,
                         logoSize,
                         { 1.0f, 1.0f, 1.0f, 0.15f });
    } else {
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
    }
}

void NoteRenderSystem::debugRenderHitboxes(Batcher&        batcher,
                                           RenderSnapshot* snapshot)
{
    if ( !snapshot ) return;

    batcher.setTexture(TextureID::None);
    for ( const auto& hb : snapshot->hitboxes ) {
        if ( hb.entity == entt::null || hb.w <= 0.0f || hb.h <= 0.0f ) continue;

        glm::vec4 color{ 0.2f, 0.9f, 1.0f, 0.8f };
        switch ( hb.part ) {
        case HoverPart::Head: color = { 0.25f, 1.0f, 0.25f, 0.85f }; break;
        case HoverPart::HoldBody: color = { 0.0f, 0.8f, 1.0f, 0.75f }; break;
        case HoverPart::HoldEnd: color = { 1.0f, 0.65f, 0.1f, 0.85f }; break;
        case HoverPart::FlickArrow: color = { 1.0f, 0.25f, 1.0f, 0.85f }; break;
        case HoverPart::PolylineNode:
            color = { 1.0f, 1.0f, 0.15f, 0.85f };
            break;
        case HoverPart::None: color = { 1.0f, 1.0f, 1.0f, 0.45f }; break;
        }

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
