#include "audio/AudioManager.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "implot.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include <cmath>
#include <vector>

namespace MMM::UI
{

void AudioTrackControllerUI::renderEQSection(bool& changed)
{
    auto& audio = Audio::AudioManager::instance();

    ImGui::Separator();
    ImGui::Text("%s", TR("ui.audio_manager.eq_control").data());

    const char* presets[] = { TR("ui.audio_manager.eq_none").data(),
                              TR("ui.audio_manager.eq_10_band").data(),
                              TR("ui.audio_manager.eq_15_band").data() };
    int         presetIdx = static_cast<int>(m_currentPreset);
    if ( ImGui::Combo(
             "##EQPreset", &presetIdx, presets, IM_ARRAYSIZE(presets)) ) {
        m_currentPreset = static_cast<Audio::EQPreset>(presetIdx);
        audio.createMainTrackEQ(m_currentPreset);
        changed = true;
    }

    if ( audio.isMainTrackEQEnabled() ) {
        size_t bandCount = audio.getMainTrackEQBandCount();

        // 绘制增益预览图
        if ( ImPlot::BeginPlot("##EQCurve",
                               ImVec2(-1, 150),
                               ImPlotFlags_NoLegend | ImPlotFlags_NoMenus) ) {
            ImPlot::SetupAxis(ImAxis_X1, TR("ui.audio_manager.freq_hz").data());
            ImPlot::SetupAxis(ImAxis_Y1, TR("ui.audio_manager.gain_db").data());
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisLimits(ImAxis_X1, 20, 20000);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -24, 24);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "%.0f dB");

            // 绘制平滑的真幅频响应曲线
            const int                  sampleCount = 128;
            static std::vector<double> curveX(sampleCount);
            static std::vector<double> curveY(sampleCount);

            for ( int j = 0; j < sampleCount; ++j ) {
                // 对数空间采样 20Hz - 20kHz
                double t    = (double)j / (double)(sampleCount - 1);
                double freq = 20.0 * std::pow(1000.0, t);  // 20 * 10^(3*t)
                curveX[j]   = freq;
                curveY[j]   = (double)audio.getMainTrackEQResponse((float)freq);
            }

            ImPlot::PlotLine(TR("ui.audio_manager.response").data(),
                             curveX.data(),
                             curveY.data(),
                             sampleCount,
                             ImPlotSpec(ImPlotProp_LineColor,
                                        ImVec4(0.2f, 0.8f, 1.0f, 1.0f),
                                        ImPlotProp_LineWeight,
                                        2.0f));

            // 绘制各频段控制点
            std::vector<double> plotX(bandCount);
            std::vector<double> plotY(bandCount);
            for ( size_t i = 0; i < bandCount; ++i ) {
                plotX[i] = audio.getMainTrackEQBandFrequency(i);
                plotY[i] = audio.getMainTrackEQBandGain(i);
            }

            ImPlot::PlotScatter(
                TR("ui.audio_manager.bands").data(),
                plotX.data(),
                plotY.data(),
                (int)bandCount,
                ImPlotSpec(ImPlotProp_Marker, ImPlotMarker_Circle));

            ImPlot::EndPlot();
        }

        // 绘制滑块组
        ImGui::Separator();

        float sliderWidth = ImGui::GetFrameHeight();
        float eqSpacing   = 8.0f;  // 增加间距以提高可读性

        // 启用水平滚动条，高度增加以容纳滚动条
        ImGui::BeginChild("EQSliders",
                          ImVec2(0, 220),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(eqSpacing, 4));

        float startY = ImGui::GetCursorPosY();
        for ( size_t i = 0; i < bandCount; ++i ) {
            ImGui::PushID((int)i);

            ImGui::SetCursorPosY(i == 0 ? startY + 3 : startY);

            ImGui::BeginGroup();

            float gain = audio.getMainTrackEQBandGain(i);
            float q    = audio.getMainTrackEQBandQ(i);
            char  label[32];
            float freq = audio.getMainTrackEQBandFrequency(i);
            if ( freq >= 1000.0f )
                snprintf(label, sizeof(label), "%.1fk", freq / 1000.0f);
            else
                snprintf(label, sizeof(label), "%.0f", freq);

            // 1. 频率标签 (居中对齐)
            float textWidth = ImGui::CalcTextSize(label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (sliderWidth - textWidth) * 0.5f);
            ImGui::TextUnformatted(label);

            // 2. 增益滑块 (固定宽度)
            if ( ImGui::VSliderFloat("##Gain",
                                     ImVec2(sliderWidth, 110),
                                     &gain,
                                     -24.0f,
                                     24.0f,
                                     "") ) {
                audio.setMainTrackEQBandGain(i, gain);
                changed = true;
            }
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    TR("ui.audio_manager.eq_tooltip").data(), label, gain);
            }

            // 3. Q 值滑块 (改为竖直布局，交互更稳定)
            if ( ImGui::VSliderFloat(
                     "##Q", ImVec2(sliderWidth, 72), &q, 0.1f, 10.0f, "") ) {
                audio.setMainTrackEQBandQ(i, q);
                changed = true;
            }
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    TR("ui.audio_manager.q_factor_tooltip").data(), q);
            }

            ImGui::EndGroup();
            ImGui::PopID();

            if ( i < bandCount - 1 ) {
                ImGui::SameLine();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();

        if ( ImGui::Button(TR("ui.audio_manager.reset_eq").data()) ) {
            for ( size_t i = 0; i < bandCount; ++i ) {
                audio.setMainTrackEQBandGain(i, 0.0f);
                audio.setMainTrackEQBandQ(i, 1.414f);
            }
            changed = true;
        }
    }
}

}  // namespace MMM::UI
