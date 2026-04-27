#include "ui/imgui/audio/AudioSpectrumView.h"
#include "audio/AudioManager.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fftw3.h>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>

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

static std::shared_ptr<BufferSourceNodeProxy> g_spectrumBufferSource =
    std::make_shared<BufferSourceNodeProxy>();

AudioSpectrumView::AudioSpectrumView(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    m_processBuffer = std::make_unique<ice::AudioBuffer>();
    m_rawBuffer     = std::make_unique<ice::AudioBuffer>();
    buildColormapLUT();
}

AudioSpectrumView::~AudioSpectrumView()
{
    if ( m_fftPlan ) fftw_destroy_plan(m_fftPlan);
    if ( m_fftIn ) fftw_free(m_fftIn);
    if ( m_fftOut ) fftw_free(m_fftOut);
}

void AudioSpectrumView::buildColormapLUT()
{
    // 预构建 Hot 色图的 256 级查找表 (从 ImPlot)
    // 必须在 ImPlot context 有效时调用
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
        m_layoutCtx, windowTitle, true, ImGuiWindowFlags_None);

    if ( !track ) {
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    if ( m_isCalculating ) {
        ImGui::OpenPopup("ProcessingSpectrum");
        if ( ImGui::BeginPopupModal("ProcessingSpectrum",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoTitleBar) ) {
            ImGui::Text(
                "Generating Spectrum Heatmap... This may take a few seconds.");
            ImGui::EndPopup();
        }
        return;
    }

    double currentTime = audioManager.getCurrentTime();
    double totalTime   = audioManager.getTotalTime();

    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat(
        TR("ui.waveform.zoom").data(), &m_zoom, 0.1f, 5.0f, "%.1fs");
    ImGui::SameLine();
    if ( ImGui::Button("Sync Effects") ) {
        fullRecalculate();
    }

    syncEQ();

    // --- 脏检查：仅在视图窗口显著变化时重新光栅化 ---
    double viewStart = currentTime - m_zoom;
    double viewEnd   = currentTime + m_zoom;

    if ( std::abs(viewStart - m_lastViewStart) > VIEW_CHANGE_THRESHOLD ||
         std::abs(viewEnd - m_lastViewEnd) > VIEW_CHANGE_THRESHOLD ) {
        updateSpectrum(currentTime, totalTime);
        m_lastViewStart = viewStart;
        m_lastViewEnd   = viewEnd;
    }

    // --- 渲染纹理：每通道一张 ImGui::Image ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  halfH = avail.y * 0.5f - 4.0f;
    ImVec2 imgSize(avail.x, halfH);

    // L 通道
    ImGui::Text("L (Hz)");
    if ( m_textureL ) {
        ImGui::Image(m_textureL->getImTextureID(), imgSize);
    } else {
        ImGui::Dummy(imgSize);
    }

    // R 通道
    ImGui::Text("R (Hz)");
    if ( m_textureR ) {
        ImGui::Image(m_textureR->getImTextureID(), imgSize);
    } else {
        ImGui::Dummy(imgSize);
    }
}

bool AudioSpectrumView::needReload()
{
    return m_textureDirty;
}

void AudioSpectrumView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    cmdPool,
                                       vk::Queue&          queue)
{
    if ( !m_textureDirty || m_texW <= 0 || m_texH <= 0 ) return;

    // 销毁旧纹理
    m_textureL.reset();
    m_textureR.reset();

    // 从 CPU 像素缓冲创建 GPU 纹理
    m_textureL =
        std::make_unique<Graphic::VKTexture>(m_pixelBufferL.data(),
                                             static_cast<uint32_t>(m_texW),
                                             static_cast<uint32_t>(m_texH),
                                             physicalDevice,
                                             logicalDevice,
                                             cmdPool,
                                             queue);

    m_textureR =
        std::make_unique<Graphic::VKTexture>(m_pixelBufferR.data(),
                                             static_cast<uint32_t>(m_texW),
                                             static_cast<uint32_t>(m_texH),
                                             physicalDevice,
                                             logicalDevice,
                                             cmdPool,
                                             queue);

    m_textureDirty = false;
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
        m_previewEQ->set_inputnode(g_spectrumBufferSource);
    }

    for ( size_t i = 0; i < m_previewEQ->get_band_count(); ++i ) {
        m_previewEQ->set_band_gain_db(i,
                                      audioManager.getMainTrackEQBandGain(i));
        m_previewEQ->set_band_q_factor(i, audioManager.getMainTrackEQBandQ(i));
    }
}

void AudioSpectrumView::fullRecalculate()
{
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();
    if ( !track ) return;

    m_isCalculating   = true;
    double totalTime  = audioManager.getTotalTime();
    double sampleRate = ice::ICEConfig::internal_format.samplerate;
    m_cachedNumTotalSegments =
        static_cast<int>(totalTime * m_cacheSegmentsPerSecond) + 1;

    m_cachedHeatmapL.assign(m_numFrequencyBins * m_cachedNumTotalSegments,
                            -100.0);
    m_cachedHeatmapR.assign(m_numFrequencyBins * m_cachedNumTotalSegments,
                            -100.0);

    const int fftSize = 2048;
    prepareFFT(fftSize);
    syncEQ();

    const size_t hopSize =
        static_cast<size_t>(sampleRate / m_cacheSegmentsPerSecond);
    m_rawBuffer->resize(ice::ICEConfig::internal_format, fftSize);
    m_processBuffer->resize(ice::ICEConfig::internal_format, fftSize);

    auto processHeatmap = [&](int chIdx, std::vector<double>& heatmap) {
        for ( int t = 0; t < m_cachedNumTotalSegments; ++t ) {
            size_t startFrame = t * hopSize;
            if ( startFrame + fftSize > track->num_frames() ) break;

            track->read(*m_rawBuffer, startFrame, fftSize);
            if ( m_previewEQ ) {
                g_spectrumBufferSource->setBuffer(m_rawBuffer.get());
                m_processBuffer->resize(ice::ICEConfig::internal_format,
                                        fftSize);
                m_previewEQ->process(*m_processBuffer);
            } else {
                for ( uint16_t ch = 0; ch < m_rawBuffer->num_channels(); ++ch )
                    std::memcpy(m_processBuffer->raw_ptrs()[ch],
                                m_rawBuffer->raw_ptrs()[ch],
                                fftSize * sizeof(float));
            }

            float* chanData = m_processBuffer->raw_ptrs()[chIdx];
            for ( int i = 0; i < fftSize; ++i )
                m_fftIn[i] = chanData[i] * m_window[i];

            fftw_execute(m_fftPlan);

            for ( int b = 0; b < m_numFrequencyBins; ++b ) {
                double freqStart =
                    20.0 * std::pow(1000.0, (double)b / m_numFrequencyBins);
                double freqEnd =
                    20.0 *
                    std::pow(1000.0, (double)(b + 1) / m_numFrequencyBins);
                int bStart = static_cast<int>(freqStart * fftSize / sampleRate);
                int bEnd =
                    std::min(static_cast<int>(freqEnd * fftSize / sampleRate),
                             fftSize / 2);

                double maxMag = 0;
                for ( int i = bStart; i <= bEnd; ++i ) {
                    double magSq = m_fftOut[i][0] * m_fftOut[i][0] +
                                   m_fftOut[i][1] * m_fftOut[i][1];
                    if ( magSq > maxMag ) maxMag = magSq;
                }
                double db = (maxMag > 1e-9)
                                ? 20.0 * std::log10(std::sqrt(maxMag) / fftSize)
                                : -100.0;
                heatmap[b * m_cachedNumTotalSegments + t] =
                    std::clamp(db, -100.0, 0.0);
            }
        }
    };

    processHeatmap(0, m_cachedHeatmapL);
    if ( m_processBuffer->num_channels() > 1 )
        processHeatmap(1, m_cachedHeatmapR);
    else
        m_cachedHeatmapR = m_cachedHeatmapL;

    // 强制重新光栅化
    m_lastViewStart = -1e9;
    m_lastViewEnd   = -1e9;

    m_isCalculating = false;
    XINFO("Spectrum heatmap recalculated: {} bins x {} segments @ {}seg/s",
          m_numFrequencyBins,
          m_cachedNumTotalSegments,
          m_cacheSegmentsPerSecond);
}

void AudioSpectrumView::updateSpectrum(double currentTime, double totalTime)
{
    if ( m_cachedHeatmapL.empty() ) return;

    double viewStart = currentTime - m_zoom;
    double viewEnd   = currentTime + m_zoom;

    // 纹理尺寸：宽度取可视窗口中的缓存段数，高度取频率 bin 数
    int newTexW =
        static_cast<int>((viewEnd - viewStart) * m_cacheSegmentsPerSecond);
    newTexW     = std::clamp(newTexW, 1, 4096);
    int newTexH = m_numFrequencyBins;

    m_texW = newTexW;
    m_texH = newTexH;

    // 分配像素缓冲
    size_t pixelCount = static_cast<size_t>(m_texW) * m_texH * 4;
    m_pixelBufferL.resize(pixelCount);
    m_pixelBufferR.resize(pixelCount);

    // 光栅化：将缓存数据映射到像素
    // 纹理坐标：行 0 = 最高频率 (顶部)，行 texH-1 = 最低频率 (底部)
    const double scaleMin = -80.0;
    const double scaleMax = -10.0;
    const double range    = scaleMax - scaleMin;

    for ( int py = 0; py < m_texH; ++py ) {
        // 频率 bin: 行 0 → 最高频率 bin (m_numFrequencyBins-1)
        int b = m_numFrequencyBins - 1 - py;

        for ( int px = 0; px < m_texW; ++px ) {
            double time    = viewStart + (static_cast<double>(px) / m_texW) *
                                             (viewEnd - viewStart);
            int    cachedT = static_cast<int>(time * m_cacheSegmentsPerSecond);

            double valL = -100.0;
            double valR = -100.0;

            if ( cachedT >= 0 && cachedT < m_cachedNumTotalSegments ) {
                valL = m_cachedHeatmapL[b * m_cachedNumTotalSegments + cachedT];
                valR = m_cachedHeatmapR[b * m_cachedNumTotalSegments + cachedT];
            }

            // 映射 dB → [0, 255] 色图索引
            auto dbToIdx = [&](double db) -> int {
                float t = static_cast<float>(
                    std::clamp((db - scaleMin) / range, 0.0, 1.0));
                return static_cast<int>(t * 255.0f);
            };

            size_t offset = (static_cast<size_t>(py) * m_texW + px) * 4;

            auto& colL                 = m_colormapLUT[dbToIdx(valL)];
            m_pixelBufferL[offset + 0] = colL[0];
            m_pixelBufferL[offset + 1] = colL[1];
            m_pixelBufferL[offset + 2] = colL[2];
            m_pixelBufferL[offset + 3] = colL[3];

            auto& colR                 = m_colormapLUT[dbToIdx(valR)];
            m_pixelBufferR[offset + 0] = colR[0];
            m_pixelBufferR[offset + 1] = colR[1];
            m_pixelBufferR[offset + 2] = colR[2];
            m_pixelBufferR[offset + 3] = colR[3];
        }
    }

    m_textureDirty = true;
}

}  // namespace MMM::UI
