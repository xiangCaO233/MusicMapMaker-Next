#include "ui/imgui/audio/AudioSpectrumView.h"
#include "audio/AudioManager.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fftw3.h>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <latch>
#include <mutex>
#include <system_error>
#include <utility>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

namespace MMM::UI
{

class BufferSourceNodeProxy : public ice::IAudioNode
{
public:
    void setBuffer(const ice::AudioBuffer* buffer) { m_buffer = buffer; }
    void process(ice::AudioBuffer& buffer) override
    {
        if ( !m_buffer ) return;
        for ( uint16_t ch = 0; ch < m_buffer->num_channels(); ++ch ) {
            size_t frames =
                std::min(buffer.num_frames(), m_buffer->num_frames());
            std::memcpy(buffer.raw_ptrs()[ch],
                        m_buffer->raw_ptrs()[ch],
                        frames * sizeof(float));
        }
    }

private:
    const ice::AudioBuffer* m_buffer{ nullptr };
};

AudioSpectrumView::AudioSpectrumView(const std::string& name)
    : IUIView(name), IRenderableView(name)
{
    m_spectrumDetailLevel =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    const auto profile = Config::spectrumDetailProfile(m_spectrumDetailLevel);
    m_cacheSegmentsPerSecond        = profile.segmentsPerSecond;
    m_numFrequencyBins              = profile.frequencyBins;
    m_pendingSpectrumDetailLevel    = m_spectrumDetailLevel;
    m_pendingCacheSegmentsPerSecond = m_cacheSegmentsPerSecond;
    m_pendingNumFrequencyBins       = m_numFrequencyBins;
    m_processBuffer                 = std::make_unique<ice::AudioBuffer>();
    m_rawBuffer                     = std::make_unique<ice::AudioBuffer>();
}

AudioSpectrumView::~AudioSpectrumView()
{
    if ( m_calcFuture.valid() ) {
        m_calcStopSource.request_stop();
        m_calcFuture.wait();
        m_calcFuture = std::future<void>{};
    }

    auto context = Graphic::VKContext::get();
    if ( context ) {
        (void)context->get().getLogicalDevice().waitIdle();
    }
}

void AudioSpectrumView::update(UIManager* sourceManager)
{
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    std::string   windowTitle = m_name + "###AudioSpectrumViewGlobal";
    LayoutContext layoutContext(
        m_layoutCtx, windowTitle, true, ImGuiWindowFlags_None, &m_isOpen);

    if ( !track ) {
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    const auto configuredDetail =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    if ( configuredDetail != m_spectrumDetailLevel &&
         !m_isCalculating.load(std::memory_order_relaxed) ) {
        startAsyncRecalculate();
    }

    if ( m_isCalculating.load() ) {
        ImGui::OpenPopup("###SpectrumCalcModal");
    }

    {
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(
                 (std::string(TR("ui.spectrum.calc_modal.title").data()) +
                  "###SpectrumCalcModal")
                     .c_str()) ) {
            float progress = m_calcProgress.load();
            ImGui::Text("%s", TR("ui.spectrum.calc_modal.text").data());
            ImGui::Spacing();
            ImGui::ProgressBar(progress, ImVec2(400, 0));
            ImGui::Text("%.0f%%", progress * 100.0f);

            if ( m_calcFinished.load() ) {
                m_calcFinished.store(false);
                m_isCalculating.store(false);
                m_spectrumDetailLevel    = m_pendingSpectrumDetailLevel;
                m_cacheSegmentsPerSecond = m_pendingCacheSegmentsPerSecond;
                m_numFrequencyBins       = m_pendingNumFrequencyBins;
                m_textureReloadStarted   = false;
                m_nextChunkUploadIndex   = 0;
                prepareFullGlobalTextures();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();

            if ( m_isCalculating.load() ) return;
        }
    }

    const auto& visualConfig = Config::AppConfig::instance().getVisualConfig();
    float       globalVisualOffset = visualConfig.getEffectiveVisualOffset();
    float       spectrumVisualOffset =
        visualConfig.getSpectrumEffectiveVisualOffset();
    double audioTime  = audioManager.getCurrentTime();
    double visualTime = audioTime + globalVisualOffset;
    double totalTime  = audioManager.getTotalTime();

    // 优先使用逻辑层的平滑视觉时间，以支持预览拖拽时的实时滚动
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    auto snapshot = Logic::EditorEngine::instance()
                        .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                              : activeCameraId)
                        ->getReadingSnapshot();
    if ( snapshot ) {
        visualTime = snapshot->currentTime;
        audioTime  = visualTime - globalVisualOffset;

        // 亚帧平滑补偿 (同步视觉偏移)
        if ( !snapshot->isPreviewDragging && snapshot->isPlaying &&
             snapshot->snapshotSysTime > 0.0 ) {
            double now =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            double dt = now - snapshot->snapshotSysTime;
            if ( dt > 0.0 && dt < 0.1 ) {
                visualTime += dt * snapshot->playbackSpeed;
                audioTime += dt * snapshot->playbackSpeed;
            }
        }
    }

    ImGuiStyle& style           = ImGui::GetStyle();
    float       frameH          = ImGui::GetFrameHeight();
    auto        calcSliderWidth = [&](float sliderW, const char* label) {
        return sliderW + style.ItemInnerSpacing.x +
               ImGui::CalcTextSize(label).x;
    };
    auto calcButtonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    auto drawSep = [&](Clay_BoundingBox r, bool) {
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(r.x, r.y + 2.0f),
            ImVec2(r.x, r.y + r.height - 2.0f),
            ImGui::GetColorU32(ImGuiCol_Separator));
    };

    CLayVBox    topContainer;
    const float toolbarDpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    const float rowPadding =
        std::ceil(std::max(4.0f * toolbarDpiScale, style.FramePadding.x));
    const float spacing =
        std::ceil(std::max(8.0f * toolbarDpiScale, style.ItemSpacing.x));
    topContainer.setPadding(0, 0, 0, 0)
        .setSpacing(toLayoutPixels(
            std::max(4.0f * toolbarDpiScale, style.ItemSpacing.y * 0.5f)));
    std::deque<CLayHBox> rows;
    CLayHBox*            currentRow = nullptr;
    float                currentW   = 0.0f;
    float                availW     = ImGui::GetContentRegionAvail().x;

    auto pushGroup = [&](const std::string& id, float w, float h, auto drawCb) {
        bool  addSep = false;
        float totalW = w;
        if ( currentRow ) {
            totalW += 1.0f + spacing;  // Sep + spacing
        }
        if ( !currentRow || currentW + totalW > availW ) {
            rows.emplace_back();
            currentRow = &rows.back();
            currentRow
                ->setPadding(toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding))
                .setSpacing(toLayoutPixels(spacing));
            topContainer.addLayout(
                ("Row_" + std::to_string(rows.size())).c_str(),
                *currentRow,
                Sizing::Grow(),
                Sizing::Fit());
            currentW = rowPadding * 2.0f;
        } else {
            addSep = true;
        }

        if ( addSep ) {
            currentRow->addElement(
                id + "_Sep", Sizing::Fixed(1.0f), Sizing::Fixed(h), drawSep);
            currentW += 1.0f + spacing;
        }
        currentRow->addElement(id, Sizing::Fixed(w), Sizing::Fixed(h), drawCb);
        currentW += w + spacing;
    };

    pushGroup("ZoomSlider",
              calcSliderWidth(100.0f, TR("ui.waveform.zoom").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  ImGui::AlignTextToFramePadding();
                  ImGui::Text("%s", TR("ui.waveform.zoom").data());
                  ImGui::SameLine();
                  ImGui::SetNextItemWidth(100);
                  ::MMM::UI::FeedbackSliderFloat(
                      "##zoom", &m_zoom, 0.1f, 10.0f, "%.1fs");
              });
    pushGroup(
        "MaxFreqSlider",
        calcSliderWidth(120.0f, TR("ui.spectrum.max_freq").data()),
        frameH,
        [&](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", TR("ui.spectrum.max_freq").data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##max_freq", &m_maxFreq, 2000.0f, 24000.0f, "%.0f Hz") ) {
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    startAsyncRecalculate();
                }
            }
        });
    pushGroup("LogBiasSlider",
              calcSliderWidth(120.0f, TR("ui.spectrum.log_bias").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  ImGui::AlignTextToFramePadding();
                  ImGui::Text("%s", TR("ui.spectrum.log_bias").data());
                  ImGui::SameLine();
                  ImGui::SetNextItemWidth(120);
                  if ( ::MMM::UI::FeedbackSliderFloat(
                           "##log_bias", &m_logBias, 0.01f, 20.0f, "%.2f") ) {
                      if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                          startAsyncRecalculate();
                      }
                  }
              });
    pushGroup("SyncEffectsBtn",
              calcButtonWidth(TR("ui.spectrum.sync_effects").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  if ( ::MMM::UI::FeedbackButton(
                           TR("ui.spectrum.sync_effects").data()) ) {
                      startAsyncRecalculate();
                  }
              });

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = topContainer.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    syncEQ();

    // --- 渲染逻辑：离屏 Vulkan 绘制频谱图，ImGui 叠加交互层 ---
    ImVec2 surfacePos = ImGui::GetCursorScreenPos();
    ImVec2 avail      = ImGui::GetContentRegionAvail();
    if ( avail.x <= 1.0f || avail.y <= 1.0f ) return;

    float textH    = ImGui::GetTextLineHeightWithSpacing();
    float plotH    = std::max(1.0f, (avail.y - 2.0f * textH) * 0.5f);
    float surfaceH = textH * 2.0f + plotH * 2.0f;
    if ( surfaceH > avail.y ) {
        surfaceH = avail.y;
        plotH    = std::max(1.0f, (surfaceH - 2.0f * textH) * 0.5f);
    }

    double viewStart = visualTime - m_zoom;
    double viewEnd   = visualTime + m_zoom;

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    setTargetSize(static_cast<uint32_t>(std::max(1.0f, avail.x)),
                  static_cast<uint32_t>(std::max(1.0f, surfaceH)),
                  dpiScale);

    m_vertices.clear();
    m_indices.clear();
    m_spectrumDrawCmds.clear();
    buildChannelGeometry(m_texturesL,
                         textH,
                         avail.x,
                         plotH,
                         viewStart,
                         viewEnd,
                         spectrumVisualOffset);
    buildChannelGeometry(m_texturesR,
                         textH + plotH + textH,
                         avail.x,
                         plotH,
                         viewStart,
                         viewEnd,
                         spectrumVisualOffset);

    vk::DescriptorSet surfaceTexture = getDescriptorSet();
    if ( surfaceTexture != VK_NULL_HANDLE ) {
        ImGui::Image(reinterpret_cast<ImTextureID>(
                         static_cast<VkDescriptorSet>(surfaceTexture)),
                     ImVec2(avail.x, surfaceH));
    } else {
        ImGui::Dummy(ImVec2(avail.x, surfaceH));
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(surfacePos,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      TR("ui.spectrum.channel_l").data());
    drawList->AddText(ImVec2(surfacePos.x, surfacePos.y + textH + plotH),
                      ImGui::GetColorU32(ImGuiCol_Text),
                      TR("ui.spectrum.channel_r").data());

    ImVec2 leftMin = ImVec2(surfacePos.x, surfacePos.y + textH);
    ImVec2 leftMax = ImVec2(surfacePos.x + avail.x, leftMin.y + plotH);
    ImVec2 rightMin =
        ImVec2(surfacePos.x, surfacePos.y + textH + plotH + textH);
    ImVec2 rightMax = ImVec2(surfacePos.x + avail.x, rightMin.y + plotH);

    renderChannelInteractionOverlay("##SeekL",
                                    leftMin,
                                    leftMax,
                                    viewStart,
                                    viewEnd,
                                    globalVisualOffset,
                                    totalTime,
                                    visualTime,
                                    snapshot);
    renderChannelInteractionOverlay("##SeekR",
                                    rightMin,
                                    rightMax,
                                    viewStart,
                                    viewEnd,
                                    globalVisualOffset,
                                    totalTime,
                                    visualTime,
                                    snapshot);

    ImGui::SetCursorScreenPos(ImVec2(surfacePos.x, surfacePos.y + surfaceH));
}

void AudioSpectrumView::addSpectrumQuad(float x, float y, float w, float h,
                                        float uv0X, float uv1X,
                                        Graphic::VKTexture* texture)
{
    if ( !texture || w <= 0.0f || h <= 0.0f ) return;

    const uint32_t baseIndex   = static_cast<uint32_t>(m_vertices.size());
    const uint32_t indexOffset = static_cast<uint32_t>(m_indices.size());

    m_vertices.push_back(
        { { x, y, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv0X, 0.0f } });
    m_vertices.push_back(
        { { x + w, y, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv1X, 0.0f } });
    m_vertices.push_back(
        { { x, y + h, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv0X, 1.0f } });
    m_vertices.push_back(
        { { x + w, y + h, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv1X, 1.0f } });

    m_indices.push_back(baseIndex + 0U);
    m_indices.push_back(baseIndex + 1U);
    m_indices.push_back(baseIndex + 2U);
    m_indices.push_back(baseIndex + 1U);
    m_indices.push_back(baseIndex + 3U);
    m_indices.push_back(baseIndex + 2U);

    m_spectrumDrawCmds.push_back({ texture, 6U, indexOffset });
}

void AudioSpectrumView::buildChannelGeometry(
    const std::vector<std::unique_ptr<Graphic::VKTexture>>& textures,
    float plotY, float plotW, float plotH, double viewStart, double viewEnd,
    float spectrumVisualOffset)
{
    if ( textures.empty() || plotW <= 0.0f || plotH <= 0.0f ) return;

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const double fftOffset =
        sampleRate > 0.0 ? (2048.0 / 2.0) / sampleRate : 0.0;
    const double audioViewStart = viewStart - spectrumVisualOffset - fftOffset;
    const double audioViewEnd   = viewEnd - spectrumVisualOffset - fftOffset;
    const double pixelStart     = audioViewStart * m_cacheSegmentsPerSecond;
    const double pixelEnd       = audioViewEnd * m_cacheSegmentsPerSecond;
    const double pixelWidth     = pixelEnd - pixelStart;
    if ( pixelWidth <= 0.0 ) return;

    for ( std::size_t i = 0; i < textures.size(); ++i ) {
        auto* texture = textures[i].get();
        if ( !texture ) continue;

        const double texGlobalStart = static_cast<double>(i * MAX_TEXTURE_W);
        const double texGlobalEnd   = texGlobalStart + texture->width();
        if ( texGlobalEnd < pixelStart || texGlobalStart > pixelEnd ) {
            continue;
        }

        const double intersectStart = std::max(texGlobalStart, pixelStart);
        const double intersectEnd   = std::min(texGlobalEnd, pixelEnd);
        if ( intersectEnd <= intersectStart ) continue;

        const float uv0X = static_cast<float>(
            (intersectStart - texGlobalStart) / texture->width());
        const float uv1X = static_cast<float>((intersectEnd - texGlobalStart) /
                                              texture->width());
        const float x    = static_cast<float>((intersectStart - pixelStart) /
                                           pixelWidth * plotW);
        const float w    = static_cast<float>((intersectEnd - intersectStart) /
                                           pixelWidth * plotW);
        addSpectrumQuad(x, plotY, w, plotH, uv0X, uv1X, texture);
    }
}

void AudioSpectrumView::renderChannelInteractionOverlay(
    const char* seekId, ImVec2 groupMin, ImVec2 groupMax, double viewStart,
    double viewEnd, float globalVisualOffset, double totalTime,
    double visualTime, const Logic::RenderSnapshot* snapshot)
{
    const float width  = groupMax.x - groupMin.x;
    const float height = groupMax.y - groupMin.y;
    if ( width <= 0.0f || height <= 0.0f ) return;

    const double viewRange = viewEnd - viewStart;
    if ( viewRange <= 0.001 ) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if ( visualTime >= viewStart && visualTime <= viewEnd ) {
        const float relativePos =
            static_cast<float>((visualTime - viewStart) / viewRange);
        const float lineX = groupMin.x + relativePos * width;
        drawList->AddLine(ImVec2(lineX, groupMin.y),
                          ImVec2(lineX, groupMax.y),
                          IM_COL32(255, 0, 0, 255),
                          2.0f);
    }

    if ( snapshot && snapshot->hasBeatmap ) {
        const float xStart =
            groupMin.x +
            static_cast<float>((snapshot->visibleTimeStart - viewStart) /
                               viewRange) *
                width;
        const float xEnd =
            groupMin.x +
            static_cast<float>((snapshot->visibleTimeEnd - viewStart) /
                               viewRange) *
                width;
        const float drawX1 = std::clamp(xStart, groupMin.x, groupMax.x);
        const float drawX2 = std::clamp(xEnd, groupMin.x, groupMax.x);

        if ( drawX2 > drawX1 ) {
            drawList->AddRectFilled(ImVec2(drawX1, groupMin.y),
                                    ImVec2(drawX2, groupMax.y),
                                    IM_COL32(128, 0, 255, 40));
            drawList->AddRect(ImVec2(drawX1, groupMin.y),
                              ImVec2(drawX2, groupMax.y),
                              IM_COL32(128, 0, 255, 180),
                              0.0f,
                              0,
                              1.5f);
        }
    }

    ImGui::SetCursorScreenPos(groupMin);
    ImGui::InvisibleButton(seekId, ImVec2(width, height));

    if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        const float  relX =
            std::clamp((mousePos.x - groupMin.x) / width, 0.0f, 1.0f);
        const double hoverVisualTime = viewStart + relX * viewRange;
        const double hoverAudioTime  = hoverVisualTime - globalVisualOffset;

        const auto timeText =
            Canvas::formatCanvasTime(hoverVisualTime, snapshot);
        ImGui::SetTooltip("%s", timeText.c_str());

        const float hoverLineX = groupMin.x + relX * width;
        drawList->AddLine(ImVec2(hoverLineX, groupMin.y),
                          ImVec2(hoverLineX, groupMax.y),
                          IM_COL32(0, 255, 0, 150),
                          1.0f);

        if ( ImGui::IsItemActive() ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetMousePosition{ "AudioSpectrum",
                                            mousePos.x - groupMin.x,
                                            mousePos.y - groupMin.y,
                                            width,
                                            height,
                                            true,
                                            true,
                                            hoverVisualTime }));

            if ( snapshot && snapshot->hasBeatmap ) {
                const double offsetStart =
                    snapshot->visibleTimeStart - snapshot->currentTime;
                const double offsetEnd =
                    snapshot->visibleTimeEnd - snapshot->currentTime;
                const float preX1 =
                    groupMin.x + static_cast<float>((hoverVisualTime +
                                                     offsetStart - viewStart) /
                                                    viewRange) *
                                     width;
                const float preX2 =
                    groupMin.x +
                    static_cast<float>(
                        (hoverVisualTime + offsetEnd - viewStart) / viewRange) *
                        width;
                const float drawPreX1 =
                    std::clamp(preX1, groupMin.x, groupMax.x);
                const float drawPreX2 =
                    std::clamp(preX2, groupMin.x, groupMax.x);

                if ( drawPreX2 > drawPreX1 ) {
                    drawList->AddRectFilled(ImVec2(drawPreX1, groupMin.y),
                                            ImVec2(drawPreX2, groupMax.y),
                                            IM_COL32(128, 0, 255, 80));
                    drawList->AddRect(ImVec2(drawPreX1, groupMin.y),
                                      ImVec2(drawPreX2, groupMax.y),
                                      IM_COL32(128, 0, 255, 230),
                                      0.0f,
                                      0,
                                      1.5f);
                }
            }
        }

        if ( ImGui::IsItemDeactivated() && ImGui::GetIO().MouseReleased[0] ) {
            Audio::AudioManager::instance().seek(
                std::clamp(hoverAudioTime, 0.0, totalTime));
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSeek{ hoverAudioTime }));
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetMousePosition{ "AudioSpectrum",
                                            mousePos.x - groupMin.x,
                                            mousePos.y - groupMin.y,
                                            width,
                                            height,
                                            false,
                                            false,
                                            -1.0 }));
        }
    }
}

bool AudioSpectrumView::needReload()
{
    return m_texturesNeedReload;
}

void AudioSpectrumView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    cmdPool,
                                       vk::Queue&          queue)
{
    if ( !m_texturesNeedReload ) return;

    if ( !m_textureReloadStarted ) {
        m_retiredTexturesL.clear();
        m_retiredTexturesR.clear();
        m_loadingTexturesL.clear();
        m_loadingTexturesR.clear();
        m_loadingTexturesL.reserve(m_pendingChunksL.size());
        m_loadingTexturesR.reserve(m_pendingChunksR.size());
        m_nextChunkUploadIndex = 0;
        m_textureReloadStarted = true;
    }

    std::size_t uploadedThisFrame = 0;
    while ( m_nextChunkUploadIndex < m_pendingChunksL.size() &&
            uploadedThisFrame < MAX_UPLOAD_CHUNK_PAIRS_PER_FRAME ) {
        const auto& chunkL = m_pendingChunksL[m_nextChunkUploadIndex];
        const auto& chunkR = m_pendingChunksR[m_nextChunkUploadIndex];
        ++m_nextChunkUploadIndex;
        ++uploadedThisFrame;

        if ( chunkL.pixels.empty() || chunkR.pixels.empty() ||
             chunkL.width == 0 || chunkL.height == 0 || chunkR.width == 0 ||
             chunkR.height == 0 ) {
            continue;
        }

        m_loadingTexturesL.push_back(std::make_unique<Graphic::VKTexture>(
            chunkL.pixels.data(),
            chunkL.width,
            chunkL.height,
            physicalDevice,
            logicalDevice,
            cmdPool,
            queue,
            Graphic::VKTexturePixelFormat::Rgba8));
        m_loadingTexturesR.push_back(std::make_unique<Graphic::VKTexture>(
            chunkR.pixels.data(),
            chunkR.width,
            chunkR.height,
            physicalDevice,
            logicalDevice,
            cmdPool,
            queue,
            Graphic::VKTexturePixelFormat::Rgba8));
    }

    if ( m_nextChunkUploadIndex >= m_pendingChunksL.size() ) {
        m_retiredTexturesL = std::move(m_texturesL);
        m_retiredTexturesR = std::move(m_texturesR);
        m_texturesL        = std::move(m_loadingTexturesL);
        m_texturesR        = std::move(m_loadingTexturesR);

        m_pendingChunksL.clear();
        m_pendingChunksR.clear();
        m_nextChunkUploadIndex = 0;
        m_textureReloadStarted = false;
        m_texturesNeedReload   = false;
    }
}

void AudioSpectrumView::syncEQ()
{
    auto& audioManager = Audio::AudioManager::instance();
    if ( !audioManager.isMainTrackEQEnabled() ) {
        m_previewEQ.reset();
        return;
    }

    if ( !m_previewEQ || m_previewEQ->get_band_count() !=
                             audioManager.getMainTrackEQBandCount() ) {
        std::vector<double> freqs;
        size_t              count = audioManager.getMainTrackEQBandCount();
        for ( size_t i = 0; i < count; ++i ) {
            freqs.push_back(audioManager.getMainTrackEQBandFrequency(i));
        }
        m_previewEQ = std::make_shared<ice::GraphicEqualizer>(freqs);
        m_previewEQ->set_inputnode(std::make_shared<BufferSourceNodeProxy>());
    }

    for ( size_t i = 0; i < m_previewEQ->get_band_count(); ++i ) {
        m_previewEQ->set_band_gain_db(i,
                                      audioManager.getMainTrackEQBandGain(i));
        m_previewEQ->set_band_q_factor(i, audioManager.getMainTrackEQBandQ(i));
    }
}

void AudioSpectrumView::startAsyncRecalculate()
{
    if ( m_isCalculating.load() ) return;

    if ( m_calcFuture.valid() ) {
        m_calcFuture.wait();
        m_calcFuture = std::future<void>{};
    }

    auto&      audioManager = Audio::AudioManager::instance();
    EQSettings eq;
    eq.enabled = audioManager.isMainTrackEQEnabled();
    if ( eq.enabled ) {
        size_t count = audioManager.getMainTrackEQBandCount();
        for ( size_t i = 0; i < count; ++i ) {
            eq.freqs.push_back(audioManager.getMainTrackEQBandFrequency(i));
            eq.gains.push_back(audioManager.getMainTrackEQBandGain(i));
            eq.qs.push_back(audioManager.getMainTrackEQBandQ(i));
        }
    }

    m_isCalculating.store(true);
    m_calcProgress.store(0.0f);
    m_calcFinished.store(false);
    m_texturesNeedReload   = false;
    m_textureReloadStarted = false;
    m_nextChunkUploadIndex = 0;
    m_pendingChunksL.clear();
    m_pendingChunksR.clear();

    const auto detailLevel =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    const auto detailProfile = Config::spectrumDetailProfile(detailLevel);

    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        m_isCalculating.store(false);
        XERROR("AppThreadPool is not initialized before spectrum calculation.");
        return;
    }

    m_calcStopSource                = std::stop_source{};
    const std::stop_token stopToken = m_calcStopSource.get_token();
    m_calcFuture                    = appThreadPool->enqueue([this,
                                           stopToken,
                                           eq      = std::move(eq),
                                           maxFreq = m_maxFreq,
                                           logBias = m_logBias,
                                           detailLevel,
                                           detailProfile]() {
        backgroundRecalculate(
            stopToken, eq, maxFreq, logBias, detailLevel, detailProfile);
    });
}

void AudioSpectrumView::backgroundRecalculate(
    std::stop_token stopToken, const EQSettings& eq, float maxFreq,
    float logBias, Config::SpectrumDetailLevel detailLevel,
    Config::SpectrumDetailProfile detailProfile)
{
    if ( stopToken.stop_requested() ) {
        m_isCalculating.store(false);
        return;
    }

    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();
    if ( !track ) {
        m_isCalculating.store(false);
        return;
    }

    double       totalTime         = audioManager.getTotalTime();
    double       sampleRate        = ice::ICEConfig::internal_format.samplerate;
    const double segmentsPerSecond = detailProfile.segmentsPerSecond;
    const int    frequencyBins     = detailProfile.frequencyBins;
    int numTotalSegments = static_cast<int>(totalTime * segmentsPerSecond) + 1;
    uint16_t numChannels = ice::ICEConfig::internal_format.channels;

    const int    fftSize = 2048;
    const size_t hopSize = static_cast<size_t>(sampleRate / segmentsPerSecond);

    auto*     appThreadPool    = MMM::Runtime::AppThreadPool::instance().get();
    const int requestedWorkers = std::max<int>(
        1, MMM::Runtime::AppThreadPool::instance().requestedWorkerCount());
    const int numWorkers = std::max(1, requestedWorkers / 2);

    XINFO(
        "Spectrum async recalculate: {} workers reserved from {}, {} "
        "segments, {} bins, "
        "maxFreq: {}, logBias: {}",
        numWorkers,
        requestedWorkers,
        numTotalSegments,
        frequencyBins,
        maxFreq,
        logBias);

    std::vector<float> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] =
            0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) *
                                    static_cast<float>(i) / (fftSize - 1)));
    }

    /// @brief 预计算的 FFT bin 范围。
    struct FrequencyBinRange {
        /// @brief 起始 FFT bin。
        int start{ 0 };

        /// @brief 结束 FFT bin。
        int end{ 0 };
    };

    const float fmin       = 20.0f;
    const float fmax       = std::max(maxFreq, fmin + 1.0f);
    const float k          = logBias;
    const float fRange     = fmax - fmin;
    const float expKMinus1 = std::exp(k) - 1.0f;
    auto        getFreq    = [&](float progress) {
        if ( std::abs(k) < 1e-4f ) {
            return fmin + fRange * progress;
        }
        return fmin + fRange * (std::exp(k * progress) - 1.0f) / expKMinus1;
    };

    std::vector<FrequencyBinRange> binRanges;
    binRanges.reserve(static_cast<size_t>(frequencyBins));
    for ( int b = 0; b < frequencyBins; ++b ) {
        const float freqStart =
            getFreq(static_cast<float>(b) / static_cast<float>(frequencyBins));
        const float freqEnd = getFreq(static_cast<float>(b + 1) /
                                      static_cast<float>(frequencyBins));
        int         bStart = static_cast<int>(freqStart * fftSize / sampleRate);
        int         bEnd   = static_cast<int>(freqEnd * fftSize / sampleRate);
        bStart             = std::clamp(bStart, 0, fftSize / 2);
        bEnd               = std::clamp(bEnd, bStart, fftSize / 2);
        binRanges.push_back({ bStart, bEnd });
    }

    const float scaleMin      = -80.0f;
    const float scaleMax      = -10.0f;
    const float scaleRange    = scaleMax - scaleMin;
    auto        dbToIntensity = [&](float db) -> std::uint8_t {
        const float t = std::clamp((db - scaleMin) / scaleRange, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(std::lround(t * 255.0f));
    };

    std::vector<std::uint8_t> heatmapL(
        static_cast<size_t>(frequencyBins) * numTotalSegments, 0U);
    std::vector<std::uint8_t> heatmapR(
        static_cast<size_t>(frequencyBins) * numTotalSegments, 0U);

    std::atomic<int> completedSegments{ 0 };
    int              totalWork = numTotalSegments * (numChannels > 1 ? 2 : 1);

    static std::mutex s_fftwPlanMutex;

    auto processChannel = [&](int                        chIdx,
                              std::vector<std::uint8_t>& heatmap,
                              int                        startSeg,
                              int                        endSeg) {
        double* localIn =
            static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
        fftw_complex* localOut = static_cast<fftw_complex*>(
            fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1)));

        fftw_plan localPlan;
        {
            std::lock_guard<std::mutex> lock(s_fftwPlanMutex);
            localPlan =
                fftw_plan_dft_r2c_1d(fftSize, localIn, localOut, FFTW_ESTIMATE);
        }

        auto localRawBuffer  = std::make_unique<ice::AudioBuffer>();
        auto localProcBuffer = std::make_unique<ice::AudioBuffer>();
        localRawBuffer->resize(ice::ICEConfig::internal_format, fftSize);
        localProcBuffer->resize(ice::ICEConfig::internal_format, fftSize);

        auto localBufferSource = std::make_shared<BufferSourceNodeProxy>();
        std::shared_ptr<ice::GraphicEqualizer> localEQ;
        if ( eq.enabled ) {
            localEQ = std::make_shared<ice::GraphicEqualizer>(eq.freqs);
            localEQ->set_inputnode(localBufferSource);
            for ( size_t i = 0; i < eq.gains.size(); ++i ) {
                localEQ->set_band_gain_db(i, eq.gains[i]);
                localEQ->set_band_q_factor(i, eq.qs[i]);
            }
        }

        for ( int t = startSeg; t < endSeg; ++t ) {
            if ( stopToken.stop_requested() ) {
                break;
            }

            size_t startFrame = static_cast<size_t>(t) * hopSize;
            if ( startFrame + fftSize > track->num_frames() ) break;

            track->read(*localRawBuffer, startFrame, fftSize);

            float* chanData = localRawBuffer->raw_ptrs()[chIdx];
            if ( localEQ ) {
                localBufferSource->setBuffer(localRawBuffer.get());
                localEQ->process(*localProcBuffer);
                chanData = localProcBuffer->raw_ptrs()[chIdx];
            }

            for ( int i = 0; i < fftSize; ++i )
                localIn[i] = static_cast<double>(chanData[i] * window[i]);

            fftw_execute(localPlan);

            for ( int b = 0; b < frequencyBins; ++b ) {
                const auto [bStart, bEnd] = binRanges[b];

                float maxMag = 0.0f;
                for ( int i = bStart; i <= bEnd; ++i ) {
                    const float real  = static_cast<float>(localOut[i][0]);
                    const float imag  = static_cast<float>(localOut[i][1]);
                    const float magSq = real * real + imag * imag;
                    if ( magSq > maxMag ) maxMag = magSq;
                }
                const float db =
                    (maxMag > 1e-9f)
                        ? 20.0f * std::log10(std::sqrt(maxMag) /
                                             static_cast<float>(fftSize))
                        : -100.0f;
                heatmap[static_cast<size_t>(b) * numTotalSegments + t] =
                    dbToIntensity(db);
            }

            completedSegments.fetch_add(1, std::memory_order_relaxed);
            m_calcProgress.store(static_cast<float>(completedSegments.load(
                                     std::memory_order_relaxed)) /
                                     totalWork,
                                 std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(s_fftwPlanMutex);
            fftw_destroy_plan(localPlan);
        }
        fftw_free(localIn);
        fftw_free(localOut);
    };

    auto runChannel = [&](int chIdx, std::vector<std::uint8_t>& heatmap) {
        if ( stopToken.stop_requested() ) {
            return;
        }

        const int segsPerWorker =
            (numTotalSegments + numWorkers - 1) / numWorkers;
        std::vector<std::pair<int, int>> ranges;
        ranges.reserve(static_cast<size_t>(numWorkers));

        for ( int w = 0; w < numWorkers; ++w ) {
            int startSeg = w * segsPerWorker;
            int endSeg   = std::min(startSeg + segsPerWorker, numTotalSegments);
            if ( startSeg >= numTotalSegments ) break;
            ranges.emplace_back(startSeg, endSeg);
        }

        if ( ranges.empty() ) {
            return;
        }

        if ( !appThreadPool || ranges.size() <= 1 ) {
            const auto [startSeg, endSeg] = ranges.front();
            processChannel(chIdx, heatmap, startSeg, endSeg);
            return;
        }

        std::latch done(static_cast<std::ptrdiff_t>(ranges.size()));
        for ( const auto [startSeg, endSeg] : ranges ) {
            appThreadPool->enqueue_void([&, chIdx, startSeg, endSeg]() {
                processChannel(chIdx, heatmap, startSeg, endSeg);
                done.count_down();
            });
        }
        done.wait();
    };

    runChannel(0, heatmapL);
    if ( stopToken.stop_requested() ) {
        m_isCalculating.store(false);
        return;
    }

    if ( numChannels > 1 ) {
        runChannel(1, heatmapR);
        if ( stopToken.stop_requested() ) {
            m_isCalculating.store(false);
            return;
        }
    } else {
        heatmapR = heatmapL;
    }

    m_cachedIntensityL              = std::move(heatmapL);
    m_cachedIntensityR              = std::move(heatmapR);
    m_cachedNumTotalSegments        = numTotalSegments;
    m_pendingSpectrumDetailLevel    = detailLevel;
    m_pendingCacheSegmentsPerSecond = segmentsPerSecond;
    m_pendingNumFrequencyBins       = frequencyBins;

    m_calcProgress.store(1.0f);
    m_calcFinished.store(true);
}

void AudioSpectrumView::prepareFullGlobalTextures()
{
    if ( m_cachedIntensityL.empty() ) return;

    int totalW = m_cachedNumTotalSegments;
    int texH   = m_numFrequencyBins;

    m_pendingChunksL.clear();
    m_pendingChunksR.clear();
    m_loadingTexturesL.clear();
    m_loadingTexturesR.clear();
    m_nextChunkUploadIndex = 0;
    m_textureReloadStarted = false;

    const int numChunks = (totalW + MAX_TEXTURE_W - 1) / MAX_TEXTURE_W;
    m_pendingChunksL.reserve(static_cast<size_t>(numChunks));
    m_pendingChunksR.reserve(static_cast<size_t>(numChunks));

    constexpr size_t rgbaBytesPerPixel = 4U;
    auto             writeHotPixel     = [](std::vector<unsigned char>& pixels,
                            size_t                      offset,
                            std::uint8_t                intensity) {
        const float t      = static_cast<float>(intensity) / 255.0f;
        auto        toByte = [](float value) {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<unsigned char>(std::lround(clamped * 255.0f));
        };

        pixels[offset]      = toByte(t * 3.0f);
        pixels[offset + 1U] = toByte(t * 3.0f - 1.0f);
        pixels[offset + 2U] = toByte(t * 3.0f - 2.0f);
        pixels[offset + 3U] = 255U;
    };

    for ( int c = 0; c < numChunks; ++c ) {
        uint32_t chunkStart = static_cast<uint32_t>(c) * MAX_TEXTURE_W;
        uint32_t chunkW =
            std::min(MAX_TEXTURE_W, static_cast<uint32_t>(totalW) - chunkStart);

        TextureChunkData chunkL, chunkR;
        chunkL.width = chunkR.width = chunkW;
        chunkL.height = chunkR.height = texH;
        chunkL.pixels.resize(chunkW * texH * rgbaBytesPerPixel);
        chunkR.pixels.resize(chunkW * texH * rgbaBytesPerPixel);

        for ( uint32_t py = 0; py < static_cast<uint32_t>(texH); ++py ) {
            int b = texH - 1 - static_cast<int>(py);
            for ( uint32_t px = 0; px < chunkW; ++px ) {
                uint32_t globalX = chunkStart + px;
                size_t   offset =
                    (static_cast<size_t>(py) * chunkW + px) * rgbaBytesPerPixel;

                writeHotPixel(
                    chunkL.pixels,
                    offset,
                    m_cachedIntensityL[b * m_cachedNumTotalSegments + globalX]);
                writeHotPixel(
                    chunkR.pixels,
                    offset,
                    m_cachedIntensityR[b * m_cachedNumTotalSegments + globalX]);
            }
        }
        m_pendingChunksL.push_back(std::move(chunkL));
        m_pendingChunksR.push_back(std::move(chunkR));
    }

    m_texturesNeedReload = !m_pendingChunksL.empty();
}

bool AudioSpectrumView::isDirty() const
{
    return true;
}

void AudioSpectrumView::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                                   uint32_t h) const
{
    (void)oldW;
    (void)oldH;
    (void)w;
    (void)h;
}

std::vector<std::string> AudioSpectrumView::getShaderSources(
    const std::string& shaderName)
{
    if ( m_shaderSourceCache.count(shaderName) ) {
        return m_shaderSourceCache[shaderName];
    }

    Config::SkinData::CanvasConfig canvasConfig =
        Config::SkinManager::instance().getCanvasConfig("AudioSpectrumView");
    if ( canvasConfig.canvas_name.empty() ) {
        XERROR("AudioSpectrumView: failed to resolve shader config.");
        return {};
    }

    auto shaderModuleIt = canvasConfig.canvas_shader_modules.find(shaderName);
    if ( shaderModuleIt == canvasConfig.canvas_shader_modules.end() ) {
        return {};
    }

    const auto      shaderPath = shaderModuleIt->second;
    std::error_code shaderPathError;
    if ( !std::filesystem::exists(shaderPath, shaderPathError) ||
         shaderPathError ) {
        XWARN("AudioSpectrumView shader module path not found: {}",
              Config::pathToUtf8(shaderPath));
        return {};
    }

    std::vector<std::string> result{
        Graphic::VKShader::readFile(
            Config::pathToUtf8(shaderPath / "VertexShader.spv")),
        Graphic::VKShader::readFile(
            Config::pathToUtf8(shaderPath / "FragmentShader.spv"))
    };
    m_shaderSourceCache[shaderName] = result;
    return result;
}

std::string AudioSpectrumView::getShaderName(
    const std::string& shaderModuleName)
{
    return "AudioSpectrumView:" + shaderModuleName;
}

/// @brief 清空缓存的 shader 源码。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void AudioSpectrumView::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
AudioSpectrumView::getVertices() const
{
    return m_vertices;
}

const std::vector<uint32_t>& AudioSpectrumView::getIndices() const
{
    return m_indices;
}

void AudioSpectrumView::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                         vk::PipelineLayout      pipelineLayout,
                                         vk::DescriptorSetLayout setLayout,
                                         vk::DescriptorSet defaultDescriptor,
                                         uint32_t          frameIndex)
{
    (void)frameIndex;
    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    for ( const auto& cmd : m_spectrumDrawCmds ) {
        vk::DescriptorSet descriptor = defaultDescriptor;
        if ( cmd.texture ) {
            descriptor = cmd.texture->getNativeDescriptorSet(pool, setLayout);
        }

        if ( descriptor != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &descriptor,
                                      0,
                                      nullptr);
            lastBound = descriptor;
        }

        cmdBuf.drawIndexed(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
    }
}

}  // namespace MMM::UI
