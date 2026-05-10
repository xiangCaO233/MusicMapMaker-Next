#include "ui/imgui/audio/AudioWaveformView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/UIManager.h"
#include "logic/EditorEngine.h"
#include <algorithm>
#include <cmath>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::UI
{

/**
 * @brief 一个简单的节点，用于在离线处理中将已有的 Buffer 提供给 EffectNode
 */
class BufferSourceNode : public ice::IAudioNode
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

static std::shared_ptr<BufferSourceNode> g_bufferSource =
    std::make_shared<BufferSourceNode>();

AudioWaveformView::AudioWaveformView(const std::string& name) : IUIView(name)
{
    m_processBuffer = std::make_unique<ice::AudioBuffer>();
    m_rawBuffer     = std::make_unique<ice::AudioBuffer>();
    m_times.resize(m_samplePoints);
    m_viewMinL.resize(m_samplePoints);
    m_maxEnvelopeL.resize(m_samplePoints);
    m_viewMinR.resize(m_samplePoints);
    m_maxEnvelopeR.resize(m_samplePoints);
}

AudioWaveformView::~AudioWaveformView() = default;

void AudioWaveformView::update(UIManager* sourceManager)
{
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);

    std::string   windowTitle = m_name + "###AudioWaveformViewGlobal";
    LayoutContext layoutContext(
        m_layoutCtx, windowTitle, true, ImGuiWindowFlags_None, &m_isOpen);

    if ( !track ) {
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    if ( m_isCalculating ) {
        ImGui::OpenPopup("ProcessingWaveform");
        if ( ImGui::BeginPopupModal("ProcessingWaveform",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoTitleBar) ) {
            ImGui::Text("%s", TR("ui.waveform.processing.text").data());
            ImGui::EndPopup();
        }
        return;
    }

    double currentTime = audioManager.getCurrentTime();
    double totalTime   = audioManager.getTotalTime();
    double speed       = audioManager.getPlaybackSpeed();

    // 优先使用逻辑层的平滑视觉时间，以支持预览拖拽时的实时滚动
    auto snapshot =
        Logic::EditorEngine::instance().getSyncBuffer("Basic2DCanvas")->getReadingSnapshot();
    if ( snapshot ) {
        currentTime = snapshot->currentTime;
        // 亚帧平滑补偿 (同步视觉偏移)
        if ( !snapshot->isPreviewDragging && snapshot->isPlaying && snapshot->snapshotSysTime > 0.0 ) {
            double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            double dt = now - snapshot->snapshotSysTime;
            if ( dt > 0.0 && dt < 0.1 ) {
                currentTime += dt * snapshot->playbackSpeed;
            }
        }
    }

    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat(
        TR("ui.waveform.zoom").data(), &m_zoom, 0.1f, 10.0f, "%.1fs");
    ImGui::SameLine();
    if ( ImGui::Button(TR("ui.waveform.reset_zoom").data()) ) m_zoom = 1.0f;
    ImGui::SameLine();

    if ( ImGui::Button(TR("ui.waveform.sync_effects").data()) ) {
        fullRecalculate();
    }

    syncEQ();
    updateEnvelopes(currentTime, totalTime, speed);

    // 计算平分高度
    float totalAvailH = ImGui::GetContentRegionAvail().y;
    float channelH    = totalAvailH * 0.5f - ImGui::GetStyle().ItemSpacing.y;

    auto renderChannel = [&](const char*   id,
                             const double* minEnv,
                             const double* maxEnv,
                             const char*   label) {
        
        double viewStart = currentTime - m_zoom;
        double viewEnd   = currentTime + m_zoom;

        // 使用静态变量存储上一帧的交互状态，以便在本帧 BeginPlot 中使用
        static bool s_lastActive[2] = { false, false };
        int chanIdx = (std::string(id) == "##WaveL") ? 0 : 1;

        if ( ImPlot::BeginPlot(
                 id,
                 ImVec2(-1, channelH),
                 ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText) ) {
            
            ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
            ImPlot::SetupAxis(
                ImAxis_Y1,
                label,
                ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(
                ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImGuiCond_Always);

            ImPlot::PlotShaded("##Wave",
                               m_times.data(),
                               minEnv,
                               maxEnv,
                               m_samplePoints,
                               ImPlotSpec(ImPlotProp_FillAlpha, 0.5f));

            // 获取视觉偏移并绘制游标
            float visualOffset =
                Config::AppConfig::instance().getVisualConfig().visualOffset;
            double adjustedTime = currentTime + visualOffset;

            double playheadX[2] = { adjustedTime, adjustedTime };
            double playheadY[2] = { -1.1, 1.1 };
            ImPlot::PlotLine("##Playhead",
                             playheadX,
                             playheadY,
                             2,
                             ImPlotSpec(ImPlotProp_LineColor,
                                        ImVec4(1, 0, 0, 1),
                                        ImPlotProp_LineWeight,
                                        2.0f));

            // --- 装饰物绘制 (必须在 EndPlot 之前) ---
            ImPlot::PushPlotClipRect();

            // 1. 绘制主画布可见范围包围框
            auto session = Logic::EditorEngine::instance().getActiveSession();
            if ( session ) {
                auto snapshot = Logic::EditorEngine::instance().getSyncBuffer("Basic2DCanvas")->getReadingSnapshot();
                if ( snapshot && snapshot->hasBeatmap ) {
                    double boxX[2] = { snapshot->visibleTimeStart, snapshot->visibleTimeEnd };
                    double boxY[2] = { 1.0, 1.0 };
                    ImVec4 boxCol = ImVec4(0.5f, 0.0f, 1.0f, 0.15f);
                    ImPlot::PlotShaded("##MainViewBox", &boxX[0], &boxY[0], 2, -1.0, ImPlotSpec(ImPlotProp_FillColor, boxCol));
                    double edgeL[2] = { snapshot->visibleTimeStart, snapshot->visibleTimeStart };
                    double edgeR[2] = { snapshot->visibleTimeEnd, snapshot->visibleTimeEnd };
                    double edgeY[2] = { -1.0, 1.0 };
                    ImPlot::PlotLine("##BoxEdgeL", edgeL, edgeY, 2, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.5f, 0.0f, 1.0f, 0.8f)));
                    ImPlot::PlotLine("##BoxEdgeR", edgeR, edgeY, 2, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.5f, 0.0f, 1.0f, 0.8f)));
                }
            }

            // 2. 绘制悬停绿色竖线和预览框
            if ( ImPlot::IsPlotHovered() || s_lastActive[chanIdx] ) {
                ImVec2 plotMin = ImPlot::GetPlotPos();
                ImVec2 plotMax = { plotMin.x + ImPlot::GetPlotSize().x, plotMin.y + ImPlot::GetPlotSize().y };
                ImVec2 mousePos = ImGui::GetMousePos();
                float  relX = std::clamp((mousePos.x - plotMin.x) / (plotMax.x - plotMin.x), 0.0f, 1.0f);
                double hoverTime = viewStart + relX * (viewEnd - viewStart);
                hoverTime = std::clamp(hoverTime, 0.0, totalTime);

                double hLineX[2] = { hoverTime, hoverTime };
                double hLineY[2] = { -1.1, 1.1 };
                ImPlot::PlotLine("##HoverLine", hLineX, hLineY, 2, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0, 1, 0, 0.6f)));

                if ( s_lastActive[chanIdx] ) {
                    auto snapshot = Logic::EditorEngine::instance().getSyncBuffer("Basic2DCanvas")->getReadingSnapshot();
                    if ( snapshot && snapshot->hasBeatmap ) {
                        double offsetStart = snapshot->visibleTimeStart - snapshot->currentTime;
                        double offsetEnd   = snapshot->visibleTimeEnd - snapshot->currentTime;
                        double preBoxX[2] = { hoverTime + offsetStart, hoverTime + offsetEnd };
                        double preBoxY[2] = { 1.0, 1.0 };
                        ImPlot::PlotShaded("##PreviewViewBox", &preBoxX[0], &preBoxY[0], 2, -1.0, ImPlotSpec(ImPlotProp_FillColor, ImVec4(0.5f, 0.0f, 1.0f, 0.35f)));
                    }
                }
            }

            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();

            // 3. 交互逻辑 (获取本帧状态供下一帧绘制使用)
            ImVec2 plotMin = ImGui::GetItemRectMin();
            ImVec2 plotMax = ImGui::GetItemRectMax();
            s_lastActive[chanIdx] = ImGui::IsItemActive();

            if ( ImGui::IsItemHovered() || ImGui::IsItemActive() ) {
                ImVec2 mousePos = ImGui::GetMousePos();
                float  relX = std::clamp((mousePos.x - plotMin.x) / (plotMax.x - plotMin.x), 0.0f, 1.0f);
                double hoverTime = viewStart + relX * (viewEnd - viewStart);
                hoverTime = std::clamp(hoverTime, 0.0, totalTime);

                ImGui::SetTooltip("%.3fs", hoverTime);

                if ( ImGui::IsItemActive() ) {

                    Event::EventBus::instance().publish(Event::LogicCommandEvent(
                        Logic::CmdSetMousePosition{ "AudioWaveform", mousePos.x - plotMin.x, mousePos.y - plotMin.y,
                                                    plotMax.x - plotMin.x, plotMax.y - plotMin.y,
                                                    true, true, hoverTime }));
                }

                if ( ImGui::IsItemDeactivated() && ImGui::GetIO().MouseReleased[0] ) {
                    audioManager.seek(hoverTime);
                    Event::EventBus::instance().publish(Event::LogicCommandEvent(Logic::CmdSeek{ hoverTime }));
                    Event::EventBus::instance().publish(Event::LogicCommandEvent(
                        Logic::CmdSetMousePosition{ "AudioWaveform", mousePos.x - plotMin.x, mousePos.y - plotMin.y,
                                                    plotMax.x - plotMin.x, plotMax.y - plotMin.y,
                                                    false, false, -1.0 }));
                }
            }
        }
    };

    renderChannel("##WaveL",
                  m_viewMinL.data(),
                  m_maxEnvelopeL.data(),
                  TR("ui.waveform.channel_l").data());
    renderChannel("##WaveR",
                  m_viewMinR.data(),
                  m_maxEnvelopeR.data(),
                  TR("ui.waveform.channel_r").data());
}

void AudioWaveformView::syncEQ()
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
        m_previewEQ->set_inputnode(g_bufferSource);
    }

    for ( size_t i = 0; i < m_previewEQ->get_band_count(); ++i ) {
        m_previewEQ->set_band_gain_db(i,
                                      audioManager.getMainTrackEQBandGain(i));
        m_previewEQ->set_band_q_factor(i, audioManager.getMainTrackEQBandQ(i));
    }
}

void AudioWaveformView::fullRecalculate()
{
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();
    if ( !track ) return;

    m_isCalculating   = true;
    double totalTime  = audioManager.getTotalTime();
    double sampleRate = ice::ICEConfig::internal_format.samplerate;
    size_t totalPoints =
        static_cast<size_t>(totalTime * m_cachePointsPerSecond) + 1;

    m_cachedMinL.assign(totalPoints, 0.0f);
    m_cachedMaxL.assign(totalPoints, 0.0f);
    m_cachedMinR.assign(totalPoints, 0.0f);
    m_cachedMaxR.assign(totalPoints, 0.0f);

    syncEQ();  // 确保 EQ 参数同步

    const size_t chunkSize = 44100;  // 1秒一块
    m_rawBuffer->resize(ice::ICEConfig::internal_format, chunkSize);
    m_processBuffer->resize(ice::ICEConfig::internal_format, chunkSize);

    for ( size_t p = 0; p < totalPoints; ++p ) {
        double startTime  = p / m_cachePointsPerSecond;
        double endTime    = (p + 1) / m_cachePointsPerSecond;
        size_t startFrame = static_cast<size_t>(startTime * sampleRate);
        size_t endFrame   = static_cast<size_t>(endTime * sampleRate);
        size_t frames = (endFrame > startFrame) ? (endFrame - startFrame) : 0;
        if ( frames == 0 ) continue;

        if ( frames > chunkSize ) frames = chunkSize;

        track->read(*m_rawBuffer, startFrame, frames);

        if ( m_previewEQ ) {
            g_bufferSource->setBuffer(m_rawBuffer.get());
            m_processBuffer->resize(ice::ICEConfig::internal_format, frames);
            m_previewEQ->process(*m_processBuffer);
        } else {
            // 直接使用原始数据
            m_processBuffer->resize(ice::ICEConfig::internal_format, frames);
            for ( uint16_t ch = 0; ch < m_rawBuffer->num_channels(); ++ch )
                std::memcpy(m_processBuffer->raw_ptrs()[ch],
                            m_rawBuffer->raw_ptrs()[ch],
                            frames * sizeof(float));
        }

        float** data     = m_processBuffer->raw_ptrs();
        int     channels = m_processBuffer->num_channels();
        float   minL = 0, maxL = 0, minR = 0, maxR = 0;
        for ( size_t f = 0; f < frames; ++f ) {
            float sL = data[0][f];
            float sR = (channels > 1) ? data[1][f] : sL;
            if ( sL < minL ) minL = sL;
            if ( sL > maxL ) maxL = sL;
            if ( sR < minR ) minR = sR;
            if ( sR > maxR ) maxR = sR;
        }
        m_cachedMinL[p] = minL;
        m_cachedMaxL[p] = maxL;
        m_cachedMinR[p] = minR;
        m_cachedMaxR[p] = maxR;
    }

    m_isCalculating = false;
}

void AudioWaveformView::updateEnvelopes(double currentTime, double totalTime,
                                        double speed)
{
    if ( m_cachedMinL.empty() ) return;

    double viewStart = currentTime - m_zoom;
    double viewEnd   = currentTime + m_zoom;

    for ( int i = 0; i < m_samplePoints; ++i ) {
        double t   = viewStart + (static_cast<double>(i) / m_samplePoints) *
                                     (viewEnd - viewStart);
        m_times[i] = t;

        if ( t < 0 || t >= totalTime ) {
            m_viewMinL[i] = m_maxEnvelopeL[i] = m_viewMinR[i] =
                m_maxEnvelopeR[i]             = 0;
            continue;
        }

        size_t p = static_cast<size_t>(t * m_cachePointsPerSecond);
        if ( p < m_cachedMinL.size() ) {
            m_viewMinL[i]     = m_cachedMinL[p];
            m_maxEnvelopeL[i] = m_cachedMaxL[p];
            m_viewMinR[i]     = m_cachedMinR[p];
            m_maxEnvelopeR[i] = m_cachedMaxR[p];
        }
    }
}

}  // namespace MMM::UI
