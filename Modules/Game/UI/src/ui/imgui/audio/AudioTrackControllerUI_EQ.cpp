#include "ui/imgui/audio/AudioTrackControllerUI.h"

#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "implot.h"
#include "mmm/project/AudioResource.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>

#include <ice/config/config.hpp>
#include <ice/core/effect/filter/BiquadFilter.hpp>

namespace MMM::UI
{
namespace
{

constexpr std::array<float, 10> TEN_BAND_FREQUENCIES{
    31.25F,  62.5F,   125.0F,  250.0F,  500.0F,
    1000.0F, 2000.0F, 4000.0F, 8000.0F, 16000.0F,
};
constexpr std::array<float, 15> FIFTEEN_BAND_FREQUENCIES{
    25.0F,   40.0F,   63.0F,   100.0F,  160.0F,  250.0F,   400.0F,   630.0F,
    1000.0F, 1600.0F, 2500.0F, 4000.0F, 6300.0F, 10000.0F, 16000.0F,
};

/// @brief 获取持久化 EQ 预设对应的中心频率。
/// @param preset 持久化预设编号。
/// @return 预设无效或关闭时返回空视图。
[[nodiscard]] std::span<const float> equalizerFrequencies(int preset) noexcept
{
    if ( preset == static_cast<int>(Audio::EQPreset::TenBand) ) {
        return TEN_BAND_FREQUENCIES;
    }
    if ( preset == static_cast<int>(Audio::EQPreset::FifteenBand) ) {
        return FIFTEEN_BAND_FREQUENCIES;
    }
    return {};
}

/// @brief 补齐项目音频资源 EQ 各频段的持久化参数。
/// @param config 待规范化资源配置。
/// @param bandCount 当前预设的频段数。
/// @return 配置被补齐时返回 true。
bool ensureEqualizerBandStorage(AudioTrackConfig& config, std::size_t bandCount)
{
    bool changed = false;
    if ( config.eqBandGains.size() != bandCount ) {
        config.eqBandGains.resize(bandCount, 0.0F);
        changed = true;
    }
    if ( config.eqBandQs.size() != bandCount ) {
        config.eqBandQs.resize(bandCount,
                               static_cast<float>(std::numbers::sqrt2));
        changed = true;
    }
    return changed;
}

/// @brief 构造当前资源配置的无状态 EQ 滤波器，仅用于 UI 曲线预览。
/// @param config 项目音频资源配置。
/// @param frequencies 当前预设的频率表。
/// @param filters 接收滤波器系数的固定容量数组。
void prepareEqualizerPreviewFilters(
    const AudioTrackConfig& config, std::span<const float> frequencies,
    std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()>& filters)
{
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return;

    for ( std::size_t index = 0; index < frequencies.size(); ++index ) {
        const float  configuredGain = config.eqBandGains[index];
        const float  configuredQ    = config.eqBandQs[index];
        const double gain =
            std::isfinite(configuredGain)
                ? std::clamp(static_cast<double>(configuredGain), -24.0, 24.0)
                : 0.0;
        const double q =
            std::isfinite(configuredQ) && configuredQ > 0.0F
                ? std::clamp(static_cast<double>(configuredQ), 0.1, 10.0)
                : std::numbers::sqrt2;
        filters[index].set_peaking(sampleRate, frequencies[index], q, gain);
    }
}

/// @brief 计算固定滤波器组在指定频率处的总响应。
/// @param filters 已准备系数的滤波器。
/// @param bandCount 有效滤波器数量。
/// @param frequency 目标频率，单位 Hz。
/// @return 总响应增益，单位 dB。
[[nodiscard]] double equalizerResponseDb(
    const std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()>&
                filters,
    std::size_t bandCount, double frequency) noexcept
{
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;

    double magnitude = 1.0;
    for ( std::size_t index = 0; index < bandCount; ++index ) {
        magnitude *=
            filters[index].get_magnitude_response(frequency, sampleRate);
    }
    if ( magnitude <= 1.0e-6 ) return -120.0;
    return 20.0 * std::log10(magnitude);
}

}  // namespace

void AudioTrackControllerUI::renderEQSection(AudioTrackConfig& config,
                                             bool&             changed)
{
    ImGui::Separator();

    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float comboWidth   = 200.0F;

    const char* title      = TR("ui.audio_manager.eq_control").data();
    const float titleWidth = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((contentWidth - titleWidth) * 0.5F);
    ImGui::Text("%s", title);

    const char* presets[] = { TR("ui.audio_manager.eq_none").data(),
                              TR("ui.audio_manager.eq_10_band").data(),
                              TR("ui.audio_manager.eq_15_band").data() };
    const bool  validEnabledPreset =
        config.eqEnabled &&
        (config.eqPreset == static_cast<int>(Audio::EQPreset::TenBand) ||
         config.eqPreset == static_cast<int>(Audio::EQPreset::FifteenBand));
    int presetIndex = validEnabledPreset ? config.eqPreset : 0;
    if ( config.eqEnabled && !validEnabledPreset ) {
        config.eqEnabled = false;
        config.eqPreset  = static_cast<int>(Audio::EQPreset::None);
        changed          = true;
    }
    m_currentPreset = static_cast<Audio::EQPreset>(presetIndex);

    ImGui::SetCursorPosX((contentWidth - comboWidth) * 0.5F);
    ImGui::SetNextItemWidth(comboWidth);
    if ( ::MMM::UI::FeedbackCombo(
             "##EQPreset", &presetIndex, presets, IM_ARRAYSIZE(presets)) ) {
        config.eqEnabled = presetIndex != 0;
        config.eqPreset  = presetIndex;
        m_currentPreset  = static_cast<Audio::EQPreset>(presetIndex);
        if ( config.eqEnabled ) {
            const auto frequencies = equalizerFrequencies(config.eqPreset);
            (void)ensureEqualizerBandStorage(config, frequencies.size());
        }
        changed = true;
    }

    if ( !config.eqEnabled ) return;

    const auto frequencies = equalizerFrequencies(config.eqPreset);
    if ( frequencies.empty() ) {
        config.eqEnabled = false;
        config.eqPreset  = static_cast<int>(Audio::EQPreset::None);
        m_currentPreset  = Audio::EQPreset::None;
        changed          = true;
        return;
    }
    changed |= ensureEqualizerBandStorage(config, frequencies.size());
    const std::size_t bandCount = frequencies.size();

    std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()> filters;
    prepareEqualizerPreviewFilters(config, frequencies, filters);

    if ( ImPlot::BeginPlot("##EQCurve",
                           ImVec2(-1, 150),
                           ImPlotFlags_NoLegend | ImPlotFlags_NoMenus) ) {
        ImPlot::SetupAxis(ImAxis_X1, TR("ui.audio_manager.freq_hz").data());
        ImPlot::SetupAxis(ImAxis_Y1, TR("ui.audio_manager.gain_db").data());
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, 20, 20000);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -24, 24);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.0f dB");

        constexpr std::size_t                  CURVE_SAMPLE_COUNT = 128U;
        std::array<double, CURVE_SAMPLE_COUNT> curveX{};
        std::array<double, CURVE_SAMPLE_COUNT> curveY{};
        for ( std::size_t index = 0; index < CURVE_SAMPLE_COUNT; ++index ) {
            const double ratio = static_cast<double>(index) /
                                 static_cast<double>(CURVE_SAMPLE_COUNT - 1U);
            const double frequency = 20.0 * std::pow(1000.0, ratio);
            curveX[index]          = frequency;
            curveY[index] = equalizerResponseDb(filters, bandCount, frequency);
        }

        ImPlot::PlotLine(TR("ui.audio_manager.response").data(),
                         curveX.data(),
                         curveY.data(),
                         static_cast<int>(curveX.size()),
                         ImPlotSpec(ImPlotProp_LineColor,
                                    ImVec4(0.2F, 0.8F, 1.0F, 1.0F),
                                    ImPlotProp_LineWeight,
                                    2.0F));

        std::array<double, FIFTEEN_BAND_FREQUENCIES.size()> plotX{};
        std::array<double, FIFTEEN_BAND_FREQUENCIES.size()> plotY{};
        for ( std::size_t index = 0; index < bandCount; ++index ) {
            plotX[index] = frequencies[index];
            plotY[index] = config.eqBandGains[index];
        }
        ImPlot::PlotScatter(TR("ui.audio_manager.bands").data(),
                            plotX.data(),
                            plotY.data(),
                            static_cast<int>(bandCount),
                            ImPlotSpec(ImPlotProp_Marker, ImPlotMarker_Circle));

        ImPlot::EndPlot();
    }

    ImGui::Separator();

    const float sliderWidth   = ImGui::GetFrameHeight();
    const float eqSpacing     = 8.0F;
    float       maxLabelWidth = 0.0F;
    for ( const float frequency : frequencies ) {
        char label[32]{};
        if ( frequency >= 1000.0F ) {
            std::snprintf(label, sizeof(label), "%.1fk", frequency / 1000.0F);
        } else {
            std::snprintf(label, sizeof(label), "%.0f", frequency);
        }
        maxLabelWidth = std::max(maxLabelWidth, ImGui::CalcTextSize(label).x);
    }

    const float colWidth = std::max(sliderWidth, maxLabelWidth);
    const float totalWidth =
        static_cast<float>(bandCount) * (colWidth + eqSpacing) - eqSpacing;
    const float availWidth           = ImGui::GetContentRegionAvail().x;
    const float totalWidthWithBuffer = totalWidth + 2.0F;
    const float childWidth = std::min(totalWidthWithBuffer, availWidth);
    if ( childWidth < availWidth ) {
        ImGui::SetCursorPosX((availWidth - childWidth) * 0.5F);
    }

    const ImGuiWindowFlags childFlags =
        totalWidthWithBuffer > availWidth ? ImGuiWindowFlags_HorizontalScrollbar
                                          : ImGuiWindowFlags_None;
    const float footerHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float availHeight = ImGui::GetContentRegionAvail().y;
    const float childHeight = std::max(220.0F, availHeight - footerHeight);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool opened = ImGui::BeginChild("EQSliders",
                                          ImVec2(childWidth, childHeight),
                                          ImGuiChildFlags_None,
                                          childFlags);
    ImGui::PopStyleVar();

    if ( opened ) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(eqSpacing, 4));

        constexpr float START_X            = 1.0F;
        const float     startY             = ImGui::GetCursorPosY();
        const float     totalSlidersHeight = childHeight - 48.0F;
        const float     gainSliderHeight =
            std::max(60.0F, totalSlidersHeight * 0.6F);
        const float qSliderHeight = std::max(40.0F, totalSlidersHeight * 0.4F);

        for ( std::size_t index = 0; index < bandCount; ++index ) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetCursorPos(ImVec2(
                START_X + static_cast<float>(index) * (colWidth + eqSpacing),
                startY + 3.0F));
            ImGui::BeginGroup();

            float&      gain = config.eqBandGains[index];
            float&      q    = config.eqBandQs[index];
            char        label[32]{};
            const float frequency = frequencies[index];
            if ( frequency >= 1000.0F ) {
                std::snprintf(
                    label, sizeof(label), "%.1fk", frequency / 1000.0F);
            } else {
                std::snprintf(label, sizeof(label), "%.0f", frequency);
            }

            const float textWidth = ImGui::CalcTextSize(label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - textWidth) * 0.5F);
            ImGui::TextUnformatted(label);

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - sliderWidth) * 0.5F);
            if ( ::MMM::UI::FeedbackVSliderFloat(
                     "##Gain",
                     ImVec2(sliderWidth, gainSliderHeight),
                     &gain,
                     -24.0F,
                     24.0F,
                     "") ) {
                changed = true;
            }
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s",
                    TR_FMT("ui.audio_manager.eq_tooltip", label, gain).c_str());
            }

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - sliderWidth) * 0.5F);
            if ( ::MMM::UI::FeedbackVSliderFloat(
                     "##Q",
                     ImVec2(sliderWidth, qSliderHeight),
                     &q,
                     0.1F,
                     10.0F,
                     "") ) {
                changed = true;
            }
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s",
                    TR_FMT("ui.audio_manager.q_factor_tooltip", q).c_str());
            }

            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    const char* resetLabel  = TR("ui.audio_manager.reset_eq").data();
    const float buttonWidth = ImGui::CalcTextSize(resetLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonWidth) *
                         0.5F);
    if ( ::MMM::UI::FeedbackButton(resetLabel) ) {
        std::fill(config.eqBandGains.begin(), config.eqBandGains.end(), 0.0F);
        std::fill(config.eqBandQs.begin(),
                  config.eqBandQs.end(),
                  static_cast<float>(std::numbers::sqrt2));
        changed = true;
    }
}

}  // namespace MMM::UI
