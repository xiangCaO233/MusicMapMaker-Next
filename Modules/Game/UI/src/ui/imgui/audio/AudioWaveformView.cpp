#include "ui/imgui/audio/AudioWaveformView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
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
    LayoutContext layoutContext(m_layoutCtx, windowTitle, true,
                                ImGuiWindowFlags_None, &m_isOpen);

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
        if ( ImPlot::BeginPlot(
                 id,
                 ImVec2(-1, channelH),
                 ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect) ) {
            double viewStart = currentTime - m_zoom;
            double viewEnd   = currentTime + m_zoom;

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
            ImPlot::EndPlot();
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
