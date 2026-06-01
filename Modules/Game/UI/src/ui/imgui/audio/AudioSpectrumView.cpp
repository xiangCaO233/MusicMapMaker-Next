#include "ui/imgui/audio/AudioSpectrumView.h"
#include "audio/AudioManager.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/layout/box/CLayBox.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fftw3.h>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <latch>
#include <mutex>
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
    : IUIView(name), ITextureLoader(name)
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
    buildColormapLUT();
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

    if ( m_fftPlan ) fftw_destroy_plan(m_fftPlan);
    if ( m_fftIn ) fftw_free(m_fftIn);
    if ( m_fftOut ) fftw_free(m_fftOut);
}

void AudioSpectrumView::buildColormapLUT()
{
    ImPlot::PushColormap(ImPlotColormap_Hot);
    for ( int i = 0; i < 256; ++i ) {
        float  t         = static_cast<float>(i) / 255.0f;
        ImVec4 col       = ImPlot::SampleColormap(t, ImPlotColormap_Hot);
        m_colormapLUT[i] = { static_cast<unsigned char>(col.x * 255.0f),
                             static_cast<unsigned char>(col.y * 255.0f),
                             static_cast<unsigned char>(col.z * 255.0f),
                             255 };
    }
    ImPlot::PopColormap();
}

void AudioSpectrumView::prepareFFT(int fftSize)
{
    if ( m_currentFFTSize == fftSize && m_fftPlan ) return;
    if ( m_fftPlan ) fftw_destroy_plan(m_fftPlan);
    if ( m_fftIn ) fftw_free(m_fftIn);
    if ( m_fftOut ) fftw_free(m_fftOut);

    m_currentFFTSize = fftSize;
    m_fftIn          = (double*)fftw_malloc(sizeof(double) * fftSize);
    m_fftOut =
        (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1));
    m_fftPlan = fftw_plan_dft_r2c_1d(fftSize, m_fftIn, m_fftOut, FFTW_MEASURE);

    m_window.resize(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        m_window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
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
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen("###SpectrumCalcModal");
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }
    if ( ImGui::BeginPopupModal(
             (std::string(TR("ui.spectrum.calc_modal.title").data()) +
              "###SpectrumCalcModal")
                 .c_str(),
             nullptr,
             ImGuiWindowFlags_None) ) {
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
            // 计算完成，立即触发全量光栅化
            prepareFullGlobalTextures();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();

        if ( m_isCalculating.load() ) return;
    }

    float  visualOffset = Config::AppConfig::instance()
                              .getVisualConfig()
                              .getEffectiveVisualOffset();
    double audioTime    = audioManager.getCurrentTime();
    double visualTime   = audioTime + visualOffset;
    double totalTime    = audioManager.getTotalTime();

    // 优先使用逻辑层的平滑视觉时间，以支持预览拖拽时的实时滚动
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    auto snapshot = Logic::EditorEngine::instance()
                        .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                              : activeCameraId)
                        ->getReadingSnapshot();
    if ( snapshot ) {
        visualTime = snapshot->currentTime;
        audioTime  = visualTime - visualOffset;

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

    CLayVBox topContainer;
    topContainer.setPadding(0, 0, 0, 0).setSpacing(4);
    std::deque<CLayHBox> rows;
    CLayHBox*            currentRow = nullptr;
    float                currentW   = 0.0f;
    float                availW     = ImGui::GetContentRegionAvail().x;
    float                spacing    = 8.0f;

    auto pushGroup = [&](const std::string& id, float w, float h, auto drawCb) {
        bool  addSep = false;
        float totalW = w;
        if ( currentRow ) {
            totalW += 1.0f + spacing;  // Sep + spacing
        }
        if ( !currentRow || currentW + totalW > availW ) {
            rows.emplace_back();
            currentRow = &rows.back();
            currentRow->setPadding(4, 4, 4, 4).setSpacing(spacing);
            topContainer.addLayout(
                ("Row_" + std::to_string(rows.size())).c_str(),
                *currentRow,
                Sizing::Grow(),
                Sizing::Fit());
            currentW = 8.0f;  // 4 + 4 padding
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
                  ImGui::SliderFloat("##zoom", &m_zoom, 0.1f, 10.0f, "%.1fs");
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
            if ( ImGui::SliderFloat(
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
                  if ( ImGui::SliderFloat(
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
                  if ( ImGui::Button(TR("ui.spectrum.sync_effects").data()) ) {
                      startAsyncRecalculate();
                  }
              });

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = topContainer.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    syncEQ();

    // --- 渲染逻辑：计算视口范围并绘制静态贴图块 ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  textH = ImGui::GetTextLineHeightWithSpacing();
    float  plotH = (avail.y - 2.0f * textH) * 0.5f;

    double viewStart = visualTime - m_zoom;
    double viewEnd   = visualTime + m_zoom;

    auto renderChannelBank = [&](const std::vector<std::unique_ptr<
                                     Graphic::VKTexture>>& textures) {
        ImGui::BeginGroup();

        // 捕获绘图区域在屏幕上的绝对起始位置
        ImVec2 plotStartPos = ImGui::GetCursorScreenPos();

        // 计算全局像素坐标范围（使用音频时间）
        // 补偿 FFT 窗口造成的中心点偏移 (2048样本 @ 44100Hz =
        // 约 46.4ms，中心点为 23.2ms)
        double fftOffset      = (2048.0 / 2.0) / 44100.0;
        double audioViewStart = viewStart - visualOffset - fftOffset;
        double audioViewEnd   = viewEnd - visualOffset - fftOffset;
        double pixelStart     = audioViewStart * m_cacheSegmentsPerSecond;
        double pixelEnd       = audioViewEnd * m_cacheSegmentsPerSecond;
        double pixelWidth     = pixelEnd - pixelStart;

        if ( pixelStart < 0.0 ) {
            float emptyScreenW =
                static_cast<float>((0.0 - pixelStart) / pixelWidth * avail.x);
            ImGui::Dummy(ImVec2(emptyScreenW, plotH));
            ImGui::SameLine(0, 0);
        }

        for ( size_t i = 0; i < textures.size(); ++i ) {
            double texGlobalStart = static_cast<double>(i * MAX_TEXTURE_W);
            double texGlobalEnd   = texGlobalStart + textures[i]->width();

            // 检查贴图块是否在视口内
            if ( texGlobalEnd < pixelStart || texGlobalStart > pixelEnd )
                continue;

            // 计算贴图在视口内的局部 UV
            double intersectStart = std::max(texGlobalStart, pixelStart);
            double intersectEnd   = std::min(texGlobalEnd, pixelEnd);

            float uv0_x = static_cast<float>((intersectStart - texGlobalStart) /
                                             textures[i]->width());
            float uv1_x = static_cast<float>((intersectEnd - texGlobalStart) /
                                             textures[i]->width());

            // 计算屏幕上的宽度比例
            float screenW = static_cast<float>((intersectEnd - intersectStart) /
                                               pixelWidth * avail.x);

            // 绘制贴图块
            ImGui::Image(textures[i]->getImTextureID(),
                         ImVec2(screenW, plotH),
                         ImVec2(uv0_x, 0),
                         ImVec2(uv1_x, 1));
            ImGui::SameLine(0, 0);
        }

        // --- 绘制游标 ---
        if ( visualTime >= viewStart && visualTime <= viewEnd ) {
            float relativePos = static_cast<float>((visualTime - viewStart) /
                                                   (viewEnd - viewStart));

            float lineX      = plotStartPos.x + (relativePos * avail.x);
            float lineTop    = plotStartPos.y;
            float lineBottom = plotStartPos.y + plotH;

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddLine(ImVec2(lineX, lineTop),
                              ImVec2(lineX, lineBottom),
                              IM_COL32(255, 0, 0, 255),
                              2.0f);
        }

        ImGui::EndGroup();

        // 交互层：使用 InvisibleButton 捕获鼠标，防止拖动时移动窗口
        ImVec2 groupMin = ImGui::GetItemRectMin();
        ImVec2 groupMax = ImGui::GetItemRectMax();
        ImGui::SetCursorScreenPos(groupMin);
        std::string btnId =
            std::string(textures == m_texturesL ? "##SeekL" : "##SeekR");
        ImGui::InvisibleButton(btnId.c_str(),
                               ImVec2(avail.x, groupMax.y - groupMin.y));

        if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float  relX =
                std::clamp((mousePos.x - groupMin.x) / avail.x, 0.0f, 1.0f);
            double hoverVisualTime = viewStart + relX * (viewEnd - viewStart);
            double hoverAudioTime  = hoverVisualTime - visualOffset;

            const auto timeText =
                Canvas::formatCanvasTime(hoverVisualTime, snapshot);
            ImGui::SetTooltip("%s", timeText.c_str());

            // 绘制悬停绿色竖线
            float hoverLineX = groupMin.x + relX * avail.x;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(hoverLineX, groupMin.y),
                                                ImVec2(hoverLineX, groupMax.y),
                                                IM_COL32(0, 255, 0, 150),
                                                1.0f);

            if ( ImGui::IsItemActive() ) {


                // 发送预览同步指令给逻辑线程
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSetMousePosition{ "AudioSpectrum",
                                                mousePos.x - groupMin.x,
                                                mousePos.y - groupMin.y,
                                                avail.x,
                                                groupMax.y - groupMin.y,
                                                true,
                                                true,
                                                hoverVisualTime }));

                // 绘制预览包围框 (拖拽时跟随鼠标的预测框)
                auto session =
                    Logic::EditorEngine::instance().getActiveSession();
                if ( session ) {
                    std::string activeCameraId =
                        Logic::EditorEngine::instance().getActiveCameraId();
                    auto snapshot = Logic::EditorEngine::instance()
                                        .getSyncBuffer(activeCameraId.empty()
                                                           ? "Basic2DCanvas"
                                                           : activeCameraId)
                                        ->getReadingSnapshot();
                    if ( snapshot && snapshot->hasBeatmap ) {
                        double offsetStart =
                            snapshot->visibleTimeStart - snapshot->currentTime;
                        double offsetEnd =
                            snapshot->visibleTimeEnd - snapshot->currentTime;
                        float viewRange =
                            static_cast<float>(viewEnd - viewStart);

                        if ( viewRange > 0.001f ) {
                            float preX1 =
                                groupMin.x +
                                (static_cast<float>(hoverVisualTime +
                                                    offsetStart - viewStart) /
                                 viewRange) *
                                    avail.x;
                            float preX2 =
                                groupMin.x +
                                (static_cast<float>(hoverVisualTime +
                                                    offsetEnd - viewStart) /
                                 viewRange) *
                                    avail.x;

                            // 裁剪并绘制
                            float drawPreX1 =
                                std::clamp(preX1, groupMin.x, groupMax.x);
                            float drawPreX2 =
                                std::clamp(preX2, groupMin.x, groupMax.x);

                            if ( drawPreX2 > drawPreX1 ) {
                                ImGui::GetWindowDrawList()->AddRectFilled(
                                    ImVec2(drawPreX1, groupMin.y),
                                    ImVec2(drawPreX2, groupMax.y),
                                    IM_COL32(
                                        128, 0, 255, 80));  // 较明显的预览紫色
                                ImGui::GetWindowDrawList()->AddRect(
                                    ImVec2(drawPreX1, groupMin.y),
                                    ImVec2(drawPreX2, groupMax.y),
                                    IM_COL32(128, 0, 255, 230),
                                    0.0f,
                                    0,
                                    1.5f);
                            }
                        }
                    }
                }
            }
        }

        // 绘制主画布可见范围包围框
        auto session = Logic::EditorEngine::instance().getActiveSession();
        if ( session ) {
            std::string activeCameraId =
                Logic::EditorEngine::instance().getActiveCameraId();
            auto snapshot =
                Logic::EditorEngine::instance()
                    .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                          : activeCameraId)
                    ->getReadingSnapshot();
            if ( snapshot && snapshot->hasBeatmap ) {
                float viewRange = static_cast<float>(viewEnd - viewStart);
                if ( viewRange > 0.001f ) {
                    float xStart =
                        groupMin.x +
                        (static_cast<float>(snapshot->visibleTimeStart -
                                            viewStart) /
                         viewRange) *
                            avail.x;
                    float xEnd = groupMin.x +
                                 (static_cast<float>(snapshot->visibleTimeEnd -
                                                     viewStart) /
                                  viewRange) *
                                     avail.x;

                    // 裁剪到组范围内显示
                    float drawX1 = std::clamp(xStart, groupMin.x, groupMax.x);
                    float drawX2 = std::clamp(xEnd, groupMin.x, groupMax.x);

                    if ( drawX2 > drawX1 ) {
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(drawX1, groupMin.y),
                            ImVec2(drawX2, groupMax.y),
                            IM_COL32(128, 0, 255, 40));  // 半透明紫色
                        ImGui::GetWindowDrawList()->AddRect(
                            ImVec2(drawX1, groupMin.y),
                            ImVec2(drawX2, groupMax.y),
                            IM_COL32(128, 0, 255, 180),
                            0.0f,
                            0,
                            1.5f);
                    }
                }
            }
        }

        if ( ImGui::IsItemDeactivated() && ImGui::GetIO().MouseReleased[0] ) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float  relX =
                std::clamp((mousePos.x - groupMin.x) / avail.x, 0.0f, 1.0f);
            double hoverVisualTime = viewStart + relX * (viewEnd - viewStart);
            double hoverAudioTime  = hoverVisualTime - visualOffset;

            audioManager.seek(std::clamp(hoverAudioTime, 0.0, totalTime));
            // 核心修复：同步逻辑层时间
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSeek{ hoverAudioTime }));

            // 停止预览状态
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetMousePosition{ "AudioSpectrum",
                                            mousePos.x - groupMin.x,
                                            mousePos.y - groupMin.y,
                                            avail.x,
                                            groupMax.y - groupMin.y,
                                            false,
                                            false,
                                            -1.0 }));
        }
    };

    ImGui::Text("%s", TR("ui.spectrum.channel_l").data());
    if ( !m_texturesL.empty() ) {
        renderChannelBank(m_texturesL);
    } else {
        ImGui::Dummy(ImVec2(avail.x, plotH));
    }

    ImGui::Text("%s", TR("ui.spectrum.channel_r").data());
    if ( !m_texturesR.empty() ) {
        renderChannelBank(m_texturesR);
    } else {
        ImGui::Dummy(ImVec2(avail.x, plotH));
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

    // 清理旧纹理
    (void)logicalDevice.waitIdle();
    m_texturesL.clear();
    m_texturesR.clear();

    // 上传新分块
    auto upload =
        [&](const std::vector<TextureChunkData>&              chunks,
            std::vector<std::unique_ptr<Graphic::VKTexture>>& target) {
            for ( const auto& chunk : chunks ) {
                target.push_back(
                    std::make_unique<Graphic::VKTexture>(chunk.pixels.data(),
                                                         chunk.width,
                                                         chunk.height,
                                                         physicalDevice,
                                                         logicalDevice,
                                                         cmdPool,
                                                         queue));
            }
        };

    upload(m_pendingChunksL, m_texturesL);
    upload(m_pendingChunksR, m_texturesR);

    m_pendingChunksL.clear();
    m_pendingChunksR.clear();
    m_texturesNeedReload = false;
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
    m_calcFuture = appThreadPool->enqueue([this,
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

    auto*     appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    const int numWorkers    = std::max<int>(
        1, MMM::Runtime::AppThreadPool::instance().requestedWorkerCount());

    XINFO(
        "Spectrum async recalculate: {} workers, {} segments, {} bins, "
        "maxFreq: {}, logBias: {}",
        numWorkers,
        numTotalSegments,
        frequencyBins,
        maxFreq,
        logBias);

    std::vector<double> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
    }

    std::vector<double> heatmapL(
        static_cast<size_t>(frequencyBins) * numTotalSegments, -100.0);
    std::vector<double> heatmapR(
        static_cast<size_t>(frequencyBins) * numTotalSegments, -100.0);

    std::atomic<int> completedSegments{ 0 };
    int              totalWork = numTotalSegments * (numChannels > 1 ? 2 : 1);

    static std::mutex s_fftwPlanMutex;

    auto processChannel = [&](int                  chIdx,
                              std::vector<double>& heatmap,
                              int                  startSeg,
                              int                  endSeg) {
        double*       localIn  = (double*)fftw_malloc(sizeof(double) * fftSize);
        fftw_complex* localOut = (fftw_complex*)fftw_malloc(
            sizeof(fftw_complex) * (fftSize / 2 + 1));

        fftw_plan localPlan;
        {
            std::lock_guard<std::mutex> lock(s_fftwPlanMutex);
            localPlan =
                fftw_plan_dft_r2c_1d(fftSize, localIn, localOut, FFTW_MEASURE);
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
                localIn[i] = chanData[i] * window[i];

            fftw_execute(localPlan);

            double fmin       = 20.0;
            double fmax       = static_cast<double>(maxFreq);
            double k          = static_cast<double>(logBias);
            double fRange     = fmax - fmin;
            double expKMinus1 = std::exp(k) - 1.0;

            for ( int b = 0; b < frequencyBins; ++b ) {
                auto getFreq = [&](double progress) {
                    if ( std::abs(k) < 1e-4 ) {
                        return fmin + fRange * progress;
                    }
                    return fmin +
                           fRange * (std::exp(k * progress) - 1.0) / expKMinus1;
                };

                double freqStart = getFreq((double)b / frequencyBins);
                double freqEnd   = getFreq((double)(b + 1) / frequencyBins);
                int bStart = static_cast<int>(freqStart * fftSize / sampleRate);
                int bEnd =
                    std::min(static_cast<int>(freqEnd * fftSize / sampleRate),
                             fftSize / 2);

                double maxMag = 0;
                for ( int i = bStart; i <= bEnd; ++i ) {
                    double magSq = localOut[i][0] * localOut[i][0] +
                                   localOut[i][1] * localOut[i][1];
                    if ( magSq > maxMag ) maxMag = magSq;
                }
                double db = (maxMag > 1e-9)
                                ? 20.0 * std::log10(std::sqrt(maxMag) / fftSize)
                                : -100.0;
                heatmap[static_cast<size_t>(b) * numTotalSegments + t] =
                    std::clamp(db, -100.0, 0.0);
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

    auto runChannel = [&](int chIdx, std::vector<double>& heatmap) {
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

    m_cachedHeatmapL                = std::move(heatmapL);
    m_cachedHeatmapR                = std::move(heatmapR);
    m_cachedNumTotalSegments        = numTotalSegments;
    m_pendingSpectrumDetailLevel    = detailLevel;
    m_pendingCacheSegmentsPerSecond = segmentsPerSecond;
    m_pendingNumFrequencyBins       = frequencyBins;

    m_calcProgress.store(1.0f);
    m_calcFinished.store(true);
}

void AudioSpectrumView::prepareFullGlobalTextures()
{
    if ( m_cachedHeatmapL.empty() ) return;

    int totalW = m_cachedNumTotalSegments;
    int texH   = m_numFrequencyBins;

    m_pendingChunksL.clear();
    m_pendingChunksR.clear();

    const double scaleMin = -80.0;
    const double scaleMax = -10.0;
    const double range    = scaleMax - scaleMin;

    auto dbToIdx = [&](double db) -> int {
        float t =
            static_cast<float>(std::clamp((db - scaleMin) / range, 0.0, 1.0));
        return static_cast<int>(t * 255.0f);
    };

    int numChunks = (totalW + MAX_TEXTURE_W - 1) / MAX_TEXTURE_W;

    for ( int c = 0; c < numChunks; ++c ) {
        uint32_t chunkStart = c * MAX_TEXTURE_W;
        uint32_t chunkW =
            std::min(MAX_TEXTURE_W, (uint32_t)totalW - chunkStart);

        TextureChunkData chunkL, chunkR;
        chunkL.width = chunkR.width = chunkW;
        chunkL.height = chunkR.height = texH;
        chunkL.pixels.resize(chunkW * texH * 4);
        chunkR.pixels.resize(chunkW * texH * 4);

        for ( uint32_t py = 0; py < (uint32_t)texH; ++py ) {
            int b = texH - 1 - py;
            for ( uint32_t px = 0; px < chunkW; ++px ) {
                uint32_t globalX = chunkStart + px;
                size_t   offset  = (py * chunkW + px) * 4;

                double valL =
                    m_cachedHeatmapL[b * m_cachedNumTotalSegments + globalX];
                auto& colL = m_colormapLUT[dbToIdx(valL)];
                std::memcpy(&chunkL.pixels[offset], colL.data(), 4);

                double valR =
                    m_cachedHeatmapR[b * m_cachedNumTotalSegments + globalX];
                auto& colR = m_colormapLUT[dbToIdx(valR)];
                std::memcpy(&chunkR.pixels[offset], colR.data(), 4);
            }
        }
        m_pendingChunksL.push_back(std::move(chunkL));
        m_pendingChunksR.push_back(std::move(chunkR));
    }

    m_texturesNeedReload = true;
}

}  // namespace MMM::UI
