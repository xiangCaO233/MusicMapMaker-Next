#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <utility>

#include <fmt/core.h>

namespace MMM::UI
{
namespace
{
/// @brief 使用音轨控制器内容字体测量单行文本宽度。
float measureTrackControllerText(const char* text)
{
    if ( !text ) return 0.0f;

    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) {
        font = ImGui::GetFont();
    }
    return font
        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text, nullptr)
        .x;
}

/// @brief 使用 UI 快照中的字体测量单行文本宽度。
float measureTrackControllerText(const char* text, ImFont* font, float fontSize)
{
    if ( !text || !font ) return 0.0f;

    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 测量一组音轨控制器文本中的最大单行宽度。
template<size_t N>
float measureTrackControllerTextList(const std::array<const char*, N>& labels)
{
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        maxWidth = std::max(maxWidth, measureTrackControllerText(label));
    }
    return maxWidth;
}

/// @brief 使用 UI 快照字体测量一组文本中的最大单行宽度。
template<size_t N>
float measureTrackControllerTextList(const std::array<const char*, N>& labels,
                                     ImFont* font, float fontSize)
{
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        maxWidth = std::max(maxWidth,
                            measureTrackControllerText(label, font, fontSize));
    }
    return maxWidth;
}

/// @brief 捕获音轨控制器同步测量所需的当前帧快照。
UiFrameSnapshot captureAudioTrackControllerUiFrameSnapshot(float dpiScale)
{
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    auto&       skinCfg    = Config::SkinManager::instance();
    const auto& style      = ImGui::GetStyle();

    UiFrameSnapshot snapshot;
    snapshot.dpiScale               = std::max(1.0f, dpiScale);
    snapshot.framePadding           = style.FramePadding;
    snapshot.frameHeight            = ImGui::GetFrameHeight();
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    snapshot.contentFont            = skinCfg.getFont("content");
    snapshot.menuFont               = skinCfg.getFont("menu");
    snapshot.fallbackFont           = ImGui::GetFont();
    snapshot.fontSize               = ImGui::GetFontSize();
    snapshot.translationVersion     = skinCfg.getTranslator().getVersion();
    snapshot.language               = settings.language;
    snapshot.preferredAsciiFont     = settings.preferredAsciiFont;
    snapshot.preferredCjkFont       = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier     = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier      = settings.uiScaleMultiplier;
    snapshot.windowPadding          = aesthetics.windowPadding;
    snapshot.itemSpacing            = aesthetics.itemSpacing;
    return snapshot;
}
}  // namespace

CLayHBox& AudioTrackControllerUI::getRow(size_t index)
{
    while ( m_rows.size() <= index ) m_rows.emplace_back();
    auto& row = m_rows[index];
    row.clear();
    return row;
}

CLayVBox& AudioTrackControllerUI::getSection(size_t index)
{
    while ( m_sections.size() <= index ) m_sections.emplace_back();
    auto& sec = m_sections[index];
    sec.clear();
    return sec;
}

float AudioTrackControllerUI::measureLabelWidth(const char* label)
{
    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) font = ImGui::GetFont();
    float  fontSize = font->LegacySize * font->Scale;
    ImVec2 sz       = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    return sz.x;
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param trackType 当前音轨类型。
/// @param trackName 当前窗口标题。
/// @return 完全匹配时返回 true。
bool AudioTrackControllerUI::layoutMetricsMatch(const LayoutMetricsCache& cache,
                                                const UiFrameSnapshot& snapshot,
                                                TrackType          trackType,
                                                const std::string& trackName)
{
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };

    return cache.valid && cache.trackType == trackType &&
           cache.trackName == trackName &&
           floatEqual(cache.dpiScale, snapshot.dpiScale) &&
           floatEqual(cache.fontSize, snapshot.fontSize) &&
           floatEqual(cache.framePadding.x, snapshot.framePadding.x) &&
           floatEqual(cache.framePadding.y, snapshot.framePadding.y) &&
           floatEqual(cache.frameHeight, snapshot.frameHeight) &&
           floatEqual(cache.frameHeightWithSpacing,
                      snapshot.frameHeightWithSpacing) &&
           cache.language == snapshot.language &&
           cache.translationVersion == snapshot.translationVersion &&
           cache.preferredAsciiFont == snapshot.preferredAsciiFont &&
           cache.preferredCjkFont == snapshot.preferredCjkFont &&
           floatEqual(cache.fontSizeMultiplier, snapshot.fontSizeMultiplier) &&
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier) &&
           floatEqual(cache.windowPadding, snapshot.windowPadding) &&
           floatEqual(cache.itemSpacing, snapshot.itemSpacing);
}

/// @brief 构造音轨控制器布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param trackType 当前音轨类型。
/// @param trackName 当前窗口标题。
/// @return 音轨控制器布局测量结果。
AudioTrackControllerUI::LayoutMetricsCache
AudioTrackControllerUI::buildLayoutMetrics(const UiFrameSnapshot& snapshot,
                                           TrackType              trackType,
                                           const std::string&     trackName)
{
    LayoutMetricsCache cache;
    cache.valid                  = true;
    cache.trackType              = trackType;
    cache.trackName              = trackName;
    cache.dpiScale               = snapshot.dpiScale;
    cache.fontSize               = snapshot.fontSize;
    cache.framePadding           = snapshot.framePadding;
    cache.frameHeight            = snapshot.frameHeight;
    cache.frameHeightWithSpacing = snapshot.frameHeightWithSpacing;
    cache.language               = snapshot.language;
    cache.translationVersion     = snapshot.translationVersion;
    cache.preferredAsciiFont     = snapshot.preferredAsciiFont;
    cache.preferredCjkFont       = snapshot.preferredCjkFont;
    cache.fontSizeMultiplier     = snapshot.fontSizeMultiplier;
    cache.uiScaleMultiplier      = snapshot.uiScaleMultiplier;
    cache.windowPadding          = snapshot.windowPadding;
    cache.itemSpacing            = snapshot.itemSpacing;

    const float scale        = std::max(1.0f, snapshot.dpiScale);
    const float itemSpacing  = std::floor(snapshot.itemSpacing * scale);
    const float labelPadding = std::ceil(std::max(8.0f * scale, itemSpacing));
    const float contentPadding =
        std::ceil(std::max(4.0f * scale, snapshot.framePadding.x));
    const float contentSpacing =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    const float rowPaddingX =
        std::ceil(std::max(8.0f * scale, snapshot.framePadding.x * 2.0f));
    const float rowPaddingY =
        std::ceil(std::max(4.0f * scale, snapshot.framePadding.y));
    const float rowSpacing    = std::ceil(std::max(8.0f * scale, itemSpacing));
    const float presetSpacing = std::ceil(std::max(4.0f * scale, itemSpacing));
    const float rowHeight =
        std::ceil(std::max(snapshot.frameHeight + rowPaddingY * 2.0f,
                           snapshot.fontSize + snapshot.framePadding.y * 2.0f));
    const float buttonHeight =
        std::ceil(std::max(snapshot.frameHeight,
                           snapshot.fontSize + snapshot.framePadding.y * 2.0f));
    const float muteButtonW = std::ceil(std::max(30.0f * scale, buttonHeight));
    ImFont*     font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    cache.contentPadding  = contentPadding;
    cache.contentSpacing  = contentSpacing;
    cache.rowPaddingX     = rowPaddingX;
    cache.rowPaddingY     = rowPaddingY;
    cache.rowSpacing      = rowSpacing;
    cache.rowHeight       = rowHeight;
    cache.buttonHeight    = buttonHeight;
    cache.muteButtonWidth = muteButtonW;
    cache.presetSpacing   = presetSpacing;

    const std::array<const char*, 8> allLabels{
        TR_CACHE("ui.audio_manager.volume").data(),
        TR_CACHE("ui.audio_manager.speed_control").data(),
        TR_CACHE("ui.audio_manager.speed_presets").data(),
        TR_CACHE("ui.audio_manager.speed_value").data(),
        TR_CACHE("ui.audio_manager.stretch_quality").data(),
        TR_CACHE("ui.audio_manager.pitch_presets").data(),
        TR_CACHE("ui.audio_manager.pitch_value").data(),
        TR_CACHE("ui.audio_manager.play_preview").data()
    };
    cache.labelWidth =
        measureTrackControllerTextList(allLabels, font, snapshot.fontSize) +
        labelPadding;

    const float sliderMinW =
        measureTrackControllerText("0.0000", font, snapshot.fontSize) +
        snapshot.framePadding.x * 4.0f + std::floor(48.0f * scale);
    const float lrPaddingX =
        std::ceil(std::max(3.0f * scale, snapshot.framePadding.x * 0.5f));
    const float lrButtonW =
        std::max({ measureTrackControllerText("L", font, snapshot.fontSize),
                   measureTrackControllerText("R", font, snapshot.fontSize),
                   measureTrackControllerText("LL", font, snapshot.fontSize),
                   measureTrackControllerText("RR", font, snapshot.fontSize),
                   24.0f * scale }) +
        lrPaddingX * 2.0f;
    cache.channelButtonWidth = std::ceil(lrButtonW);

    float widgetWidth = muteButtonW + rowSpacing + sliderMinW;
    if ( trackType == TrackType::Main ) {
        widgetWidth += cache.channelButtonWidth * 4.0f + presetSpacing * 4.0f;

        const std::array<const char*, 4> speedPresets{
            TR_CACHE("ui.audio_manager.speed_025x").data(),
            TR_CACHE("ui.audio_manager.speed_050x").data(),
            TR_CACHE("ui.audio_manager.speed_075x").data(),
            TR_CACHE("ui.audio_manager.speed_100x").data()
        };
        const std::array<const char*, 4> pitchPresets{
            TR_CACHE("ui.audio_manager.pitch_n24").data(),
            TR_CACHE("ui.audio_manager.pitch_n12").data(),
            TR_CACHE("ui.audio_manager.pitch_n5").data(),
            TR_CACHE("ui.audio_manager.pitch_0").data()
        };
        const float speedButtonsW = measureTrackControllerTextList(
                                        speedPresets, font, snapshot.fontSize) +
                                    snapshot.framePadding.x * 2.0f;
        const float pitchButtonsW = measureTrackControllerTextList(
                                        pitchPresets, font, snapshot.fontSize) +
                                    snapshot.framePadding.x * 2.0f;
        const float analysisButtonsW =
            measureTrackControllerText(
                TR("ui.audio_manager.open_waveform").data(),
                font,
                snapshot.fontSize) +
            measureTrackControllerText(
                TR("ui.audio_manager.open_spectrum").data(),
                font,
                snapshot.fontSize) +
            snapshot.framePadding.x * 4.0f + rowSpacing;
        widgetWidth = std::max({ widgetWidth,
                                 speedButtonsW,
                                 pitchButtonsW,
                                 analysisButtonsW,
                                 200.0f * scale });
    } else {
        const float playButtonW =
            std::max(80.0f * scale,
                     measureTrackControllerText(
                         TR("ui.audio_manager.resume_preview").data(),
                         font,
                         snapshot.fontSize) +
                         snapshot.framePadding.x * 2.0f);
        const float pauseButtonW =
            std::max(80.0f * scale,
                     measureTrackControllerText(
                         TR("ui.audio_manager.pause_preview").data(),
                         font,
                         snapshot.fontSize) +
                         snapshot.framePadding.x * 2.0f);
        const float progressW =
            measureTrackControllerText(
                "000.00s / 000.00s", font, snapshot.fontSize) +
            snapshot.framePadding.x * 2.0f;
        widgetWidth = std::max(
            widgetWidth,
            playButtonW + pauseButtonW + progressW + rowSpacing * 2.0f);
    }

    const float rowDecorations = rowPaddingX * 2.0f + rowSpacing;
    const float contentWidth =
        cache.labelWidth + widgetWidth + rowDecorations + contentPadding * 2.0f;
    const size_t rowCount = trackType == TrackType::Main ? 8U : 2U;
    float contentH = 2.0f * scale + contentPadding * 2.0f +
                     rowCount * rowHeight +
                     (rowCount > 0 ? (rowCount - 1) * contentSpacing : 0.0f);

    if ( trackType == TrackType::Main ) {
        contentH += snapshot.frameHeightWithSpacing * 3.0f;
    }

    const float titleWidth =
        measureTrackControllerText(trackName.c_str(), font, snapshot.fontSize) +
        snapshot.frameHeight * 2.0f;
    const float minWidth  = std::ceil(std::max(contentWidth, titleWidth) +
                                      snapshot.windowPadding * 2.0f);
    const float minHeight = std::ceil(contentH + snapshot.windowPadding * 2.0f +
                                      snapshot.frameHeightWithSpacing);
    cache.minWindowSize   = ImVec2(minWidth, minHeight);

    if ( trackType == TrackType::Main ) {
        cache.minWindowSizeWithEq =
            ImVec2(minWidth,
                   std::ceil(minHeight + 150.0f * scale + 220.0f * scale +
                             snapshot.frameHeightWithSpacing * 2.0f));
    } else {
        cache.minWindowSizeWithEq = cache.minWindowSize;
    }
    return cache;
}

/// @brief 获取音轨控制器布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和音轨类型匹配的布局测量结果。
const AudioTrackControllerUI::LayoutMetricsCache&
AudioTrackControllerUI::getLayoutMetrics(float dpiScale) const
{
    UiFrameSnapshot snapshot =
        captureAudioTrackControllerUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(
             m_layoutMetricsCache, snapshot, m_type, m_trackName) ) {
        m_layoutMetricsCache =
            buildLayoutMetrics(snapshot, m_type, m_trackName);
    }
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧音轨控制器是否需要准备布局测量数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要刷新布局缓存时返回 true。
bool AudioTrackControllerUI::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    return m_isOpen && !layoutMetricsMatch(
                           m_layoutMetricsCache, snapshot, m_type, m_trackName);
}

/// @brief 在 UI 主线程准备音轨控制器布局测量数据。
/// @param snapshot 当前帧 UI 快照。
void AudioTrackControllerUI::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    m_preparedLayoutMetricsCache =
        buildLayoutMetrics(snapshot, m_type, m_trackName);
    m_hasPreparedLayoutMetrics = true;
}

/// @brief 将准备好的布局测量数据切换给主线程使用。
void AudioTrackControllerUI::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedLayoutMetrics ) {
        return;
    }

    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 计算音轨控制器当前音轨类型所需的最小整窗尺寸。
ImVec2 AudioTrackControllerUI::getMinWindowSize(float dpiScale) const
{
    const auto& cache = getLayoutMetrics(dpiScale);
    if ( m_type == TrackType::Main &&
         Audio::AudioManager::instance().isMainTrackEQEnabled() ) {
        return cache.minWindowSizeWithEq;
    }
    return cache.minWindowSize;
}

void AudioTrackControllerUI::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                            const char* label, float labelWidth,
                                            CLayBox::DrawFunc widget,
                                            float             heightOverride)
{
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };

    auto& row = getRow(rowIndex++);
    row.setPadding(toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingY),
                   toLayoutPixels(layoutMetrics.rowPaddingY))
        .setSpacing(toLayoutPixels(layoutMetrics.rowSpacing))
        .setAlignment(Alignment::Center());

    std::string labelId = "AT_R" + std::to_string(rowIndex) + "_L_" + label;

    // A. Left Box: 【说明标签，弹簧】
    auto& leftBox = getRow(rowIndex++);
    leftBox.clear();
    leftBox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .setAlignment(Alignment::Center());

    // 1. 说明标签 (采用 Fit 自动匹配内容宽度)
    leftBox.addElement(labelId + "_lbl",
                       Sizing::Fit(),
                       Sizing::Grow(),
                       [label](Clay_BoundingBox r, bool) {
                           float textH  = ImGui::CalcTextSize(label).y;
                           float offset = (r.height - textH) * 0.5f;
                           ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                           ImGui::Text("%s", label);
                       });

    // 2. 弹簧 spacer
    leftBox.addElement(
        labelId + "_lbl_spring", Sizing::Grow(), Sizing::Grow(), nullptr);

    // 将 Left Box 作为一个具有固定宽度的子 HBox 加入主行
    row.addLayout((labelId + "_left").c_str(),
                  leftBox,
                  Sizing::Fixed(labelWidth),
                  Sizing::Grow());

    // B. Right Box: 【控件或标签】直接 Grow()
    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) { widget(r, h); });

    float rowH = heightOverride > 0.0f
                     ? std::max(heightOverride, layoutMetrics.rowHeight)
                     : layoutMetrics.rowHeight;
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

void AudioTrackControllerUI::buildVolumeSection(CLayVBox& parent,
                                                size_t&   rowIndex,
                                                float labelWidth, float& volume,
                                                bool& muted, bool& changed)
{
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    const float btnWidth   = layoutMetrics.muteButtonWidth;
    const float btnHeight  = layoutMetrics.buttonHeight;
    const float lrPaddingX = std::ceil(std::max(
        3.0f * layoutMetrics.dpiScale, layoutMetrics.framePadding.x * 0.5f));
    const float lrGap      = layoutMetrics.presetSpacing;
    const float lrButtonW  = layoutMetrics.channelButtonWidth;
    const float rowSpacing = layoutMetrics.rowSpacing;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.volume").data(),
        labelWidth,
        [this,
         &volume,
         &muted,
         &changed,
         btnWidth,
         btnHeight,
         lrPaddingX,
         lrGap,
         lrButtonW,
         rowSpacing](Clay_BoundingBox r, bool) {
            auto& audio = Audio::AudioManager::instance();

            float offset = (r.height - btnHeight) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });

            const char* icon = ICON_MMM_VOLUME_MUTE;
            if ( !muted ) {
                if ( volume <= 0.33f )
                    icon = ICON_MMM_VOLUME_OFF;
                else if ( volume <= 0.66f )
                    icon = ICON_MMM_VOLUME_LOW;
                else
                    icon = ICON_MMM_VOLUME_HIGH;
            }

            bool pushedTextColor = false;
            if ( muted ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
                pushedTextColor = true;
            }

            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            Utils::pushFixedButtonStyleVars();
            if ( ::MMM::UI::FeedbackButton(icon,
                                           ImVec2(btnWidth, btnHeight)) ) {
                muted   = !muted;
                changed = true;
            }
            Utils::popFixedButtonStyleVars();
            ImGui::PopStyleColor();

            if ( pushedTextColor ) {
                ImGui::PopStyleColor();
            }

            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip("%s",
                                  muted ? TR("ui.audio_manager.unmute").data()
                                        : TR("ui.audio_manager.mute").data());
            }

            ImGui::SameLine(0, rowSpacing);

            float lrWidth     = (m_type == TrackType::Main)
                                    ? (lrButtonW * 4.0f + lrGap * 4.0f)
                                    : 0.0f;
            float sliderWidth = r.width - btnWidth - rowSpacing - lrWidth;
            sliderWidth       = std::max(sliderWidth, 40.0f);
            ImGui::SetNextItemWidth(sliderWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##Volume", &volume, 0.0f, 1.0f, "%.2f") ) {
                changed = true;
                if ( muted && volume > 0.0f ) {
                    muted = false;
                }
            }

            if ( m_type == TrackType::Main ) {
                auto         channelMode = audio.getMainMixerChannelMode();
                const ImVec4 copyModeColor{ 0.45f, 1.0f, 0.48f, 1.0f };
                auto drawChannelButton = [&](const char*             id,
                                             Audio::MixerChannelMode mode,
                                             const char*             tooltip,
                                             const ImVec4& activeColor) {
                    const bool active = channelMode == mode;
                    ImGui::SameLine(0, lrGap);
                    if ( active ) {
                        ImGui::PushStyleColor(ImGuiCol_Text, activeColor);
                    }
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_FramePadding,
                        ImVec2(lrPaddingX, style.FramePadding.y));
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                        ImVec2(0.5f, 0.5f));
                    if ( ::MMM::UI::FeedbackButton(
                             id, ImVec2(lrButtonW, btnHeight)) ) {
                        channelMode =
                            active ? Audio::MixerChannelMode::Stereo : mode;
                        audio.setMainMixerChannelMode(channelMode);
                    }
                    ImGui::PopStyleVar(2);
                    if ( active ) {
                        ImGui::PopStyleColor();
                    }
                    if ( ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip("%s", tooltip);
                    }
                };

                drawChannelButton("L##MainMixerMuteL",
                                  Audio::MixerChannelMode::MuteLeft,
                                  TR("ui.audio_manager.mute_l").data(),
                                  Utils::UIThemeUtils::getDangerColor());
                drawChannelButton("R##MainMixerMuteR",
                                  Audio::MixerChannelMode::MuteRight,
                                  TR("ui.audio_manager.mute_r").data(),
                                  Utils::UIThemeUtils::getDangerColor());
                drawChannelButton("LL##MainMixerCopyL",
                                  Audio::MixerChannelMode::CopyLeftToRight,
                                  "LL",
                                  copyModeColor);
                drawChannelButton("RR##MainMixerCopyR",
                                  Audio::MixerChannelMode::CopyRightToLeft,
                                  "RR",
                                  copyModeColor);
            }
        });
}

void AudioTrackControllerUI::buildSpeedAndPitchSection(
    CLayVBox& parent, size_t& rowIndex, float labelWidth, float availWidgetW,
    float& speed, float& pitch, bool& changed)
{
    auto&       audio = Audio::AudioManager::instance();
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    const float rowPadY     = layoutMetrics.rowPaddingY;
    const float widgetH     = layoutMetrics.buttonHeight;
    const float spacing     = layoutMetrics.presetSpacing;
    const float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
    availWidgetW            = std::max(availWidgetW, widgetH * 4.0f);

    // 动态检测并自动折行说明标签
    auto trim = [](const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if ( first == std::string::npos ) return std::string();
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    };

    float       actualSpeed = (float)audio.getActualPlaybackSpeed();
    std::string rawStr      = TR("ui.audio_manager.speed_info").data();
    size_t      pipePos     = rawStr.find('|');
    bool        hasPipe     = (pipePos != std::string::npos);
    std::string leftFmt  = hasPipe ? trim(rawStr.substr(0, pipePos)) : rawStr;
    std::string rightFmt = hasPipe ? trim(rawStr.substr(pipePos + 1)) : "";

    char leftBuf[128]  = { 0 };
    char rightBuf[128] = { 0 };
    char fullBuf[256]  = { 0 };

    snprintf(leftBuf, sizeof(leftBuf), leftFmt.c_str(), speed);
    if ( hasPipe ) {
        snprintf(rightBuf, sizeof(rightBuf), rightFmt.c_str(), actualSpeed);
        snprintf(fullBuf, sizeof(fullBuf), "%s | %s", leftBuf, rightBuf);
    } else {
        snprintf(fullBuf, sizeof(fullBuf), "%s", leftBuf);
    }

    std::string leftStr  = leftBuf;
    std::string rightStr = rightBuf;
    std::string fullStr  = fullBuf;

    float textW      = ImGui::CalcTextSize(fullStr.c_str()).x;
    bool  labelWraps = (textW > availWidgetW);
    float labelH = labelWraps ? (2.0f * widgetH + lineSpacing + rowPadY * 2.0f)
                              : layoutMetrics.rowHeight;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_control").data(),
        labelWidth,
        [leftStr, rightStr, fullStr, labelWraps, rowPadY, widgetH, lineSpacing](
            Clay_BoundingBox r, bool) {
            ImGui::AlignTextToFramePadding();

            if ( labelWraps ) {
                // 第一行期望值
                ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });
                ImGui::TextUnformatted(leftStr.c_str());

                // 第二行实际值
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + rowPadY + widgetH + lineSpacing });
                ImGui::TextUnformatted(rightStr.c_str());
            } else {
                float offset = (r.height - widgetH) * 0.5f;
                ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                ImGui::TextUnformatted(fullStr.c_str());
            }
        },
        labelH);

    // 动态计算速度预设按钮自动折行的高度与宽度
    std::string speed025 = TR("ui.audio_manager.speed_025x").data();
    std::string speed050 = TR("ui.audio_manager.speed_050x").data();
    std::string speed075 = TR("ui.audio_manager.speed_075x").data();
    std::string speed100 = TR("ui.audio_manager.speed_100x").data();

    std::vector<std::string> speedPresets = {
        speed025, speed050, speed075, speed100
    };
    std::vector<float> targetSpeeds = { 0.25f, 0.5f, 0.75f, 1.0f };

    float currentX   = 0.0f;
    int   speedLines = 1;
    for ( size_t i = 0; i < speedPresets.size(); ++i ) {
        float btnW = ImGui::CalcTextSize(speedPresets[i].c_str()).x +
                     ImGui::GetStyle().FramePadding.x * 2.0f;
        if ( i > 0 ) {
            if ( currentX + spacing + btnW < availWidgetW ) {
                currentX += spacing + btnW;
            } else {
                speedLines++;
                currentX = btnW;
            }
        } else {
            currentX = btnW;
        }
    }
    float speedPresetsH =
        speedLines * widgetH + (speedLines - 1) * lineSpacing + rowPadY * 2.0f;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_presets").data(),
        labelWidth,
        [speedPresets,
         targetSpeeds,
         &speed,
         &changed,
         rowPadY,
         widgetH,
         spacing,
         lineSpacing](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });

            float currentX = 0.0f;
            float currentY = 0.0f;
            for ( size_t i = 0; i < speedPresets.size(); ++i ) {
                float btnW = ImGui::CalcTextSize(speedPresets[i].c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
                if ( i > 0 ) {
                    if ( currentX + spacing + btnW < r.width ) {
                        ImGui::SameLine();
                        currentX += spacing + btnW;
                    } else {
                        currentY += widgetH + lineSpacing;
                        ImGui::SetCursorScreenPos(
                            { r.x, r.y + rowPadY + currentY });
                        currentX = btnW;
                    }
                } else {
                    currentX = btnW;
                }

                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackButton(speedPresets[i].c_str()) ) {
                    speed   = targetSpeeds[i];
                    changed = true;
                }
                ImGui::PopID();
            }
        },
        speedPresetsH);

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_value").data(),
        labelWidth,
        [&speed, &changed](Clay_BoundingBox r, bool) {
            ImGui::SetNextItemWidth(r.width);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##SpeedSlider", &speed, 0.25f, 2.0f, "%.4fx") ) {
                changed = true;
            }
            if ( ImGui::IsItemHovered() ) {
                Utils::renderTooltip(
                    TR("ui.audio_manager.speed_tooltip").data(),
                    Utils::TooltipDir::Right);
            }
        });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.stretch_quality").data(),
        labelWidth,
        [&changed, &audio](Clay_BoundingBox r, bool) {
            const char* qualityNames[] = {
                TR("ui.audio_manager.quality_fast").data(),
                TR("ui.audio_manager.quality_balanced").data(),
                TR("ui.audio_manager.quality_finer").data(),
                TR("ui.audio_manager.quality_best").data()
            };

            int currentQuality = static_cast<int>(audio.getPlaybackQuality());
            ImGui::SetNextItemWidth(r.width);
            if ( ::MMM::UI::FeedbackCombo("##StretchQuality",
                                          &currentQuality,
                                          qualityNames,
                                          IM_ARRAYSIZE(qualityNames)) ) {
                audio.setPlaybackQuality(
                    static_cast<Audio::AudioManager::StretchQuality>(
                        currentQuality));
                changed = true;
            }
        });

    // 动态计算音高预设按钮自动折行的高度与宽度
    std::string pitchN24 = TR("ui.audio_manager.pitch_n24").data();
    std::string pitchN12 = TR("ui.audio_manager.pitch_n12").data();
    std::string pitchN5  = TR("ui.audio_manager.pitch_n5").data();
    std::string pitch0   = TR("ui.audio_manager.pitch_0").data();

    std::vector<std::string> pitchPresets = {
        pitchN24, pitchN12, pitchN5, pitch0
    };
    std::vector<float> targetPitches = { -24.0f, -12.0f, -5.0f, 0.0f };

    currentX       = 0.0f;
    int pitchLines = 1;
    for ( size_t i = 0; i < pitchPresets.size(); ++i ) {
        float btnW = ImGui::CalcTextSize(pitchPresets[i].c_str()).x +
                     ImGui::GetStyle().FramePadding.x * 2.0f;
        if ( i > 0 ) {
            if ( currentX + spacing + btnW < availWidgetW ) {
                currentX += spacing + btnW;
            } else {
                pitchLines++;
                currentX = btnW;
            }
        } else {
            currentX = btnW;
        }
    }
    float pitchPresetsH =
        pitchLines * widgetH + (pitchLines - 1) * lineSpacing + rowPadY * 2.0f;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_presets").data(),
        labelWidth,
        [pitchPresets,
         targetPitches,
         &pitch,
         &changed,
         rowPadY,
         widgetH,
         spacing,
         lineSpacing](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });

            float currentX = 0.0f;
            float currentY = 0.0f;
            for ( size_t i = 0; i < pitchPresets.size(); ++i ) {
                float btnW = ImGui::CalcTextSize(pitchPresets[i].c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
                if ( i > 0 ) {
                    if ( currentX + spacing + btnW < r.width ) {
                        ImGui::SameLine();
                        currentX += spacing + btnW;
                    } else {
                        currentY += widgetH + lineSpacing;
                        ImGui::SetCursorScreenPos(
                            { r.x, r.y + rowPadY + currentY });
                        currentX = btnW;
                    }
                } else {
                    currentX = btnW;
                }

                ImGui::PushID(static_cast<int>(i + 100));
                if ( ::MMM::UI::FeedbackButton(pitchPresets[i].c_str()) ) {
                    pitch   = targetPitches[i];
                    changed = true;
                }
                ImGui::PopID();
            }
        },
        pitchPresetsH);

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_value").data(),
        labelWidth,
        [&pitch, &changed](Clay_BoundingBox r, bool) {
            ImGui::SetNextItemWidth(r.width);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##PitchSlider", &pitch, -24.0f, 24.0f, "%.1f st") ) {
                changed = true;
            }
        });
}

void AudioTrackControllerUI::buildEffectPreviewSection(CLayVBox& parent,
                                                       size_t&   rowIndex,
                                                       float     labelWidth)
{
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    const float buttonHeight = layoutMetrics.buttonHeight;
    const float dpiScale     = layoutMetrics.dpiScale;
    const float gap          = layoutMetrics.rowSpacing;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.play_preview").data(),
        labelWidth,
        [this, buttonHeight, dpiScale, gap](Clay_BoundingBox r, bool) {
            auto& audio = Audio::AudioManager::instance();

            ImGui::SetCursorScreenPos({ r.x, r.y });

            bool        isPaused = audio.isSFXPaused(m_trackId);
            const char* playText =
                isPaused ? TR("ui.audio_manager.resume_preview").data()
                         : TR("ui.audio_manager.play_preview").data();
            const char* pauseText = TR("ui.audio_manager.pause_preview").data();
            const auto& style     = ImGui::GetStyle();
            auto        buttonWidth = [&](const char* text) {
                return std::max(
                    80.0f * dpiScale,
                    ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f);
            };
            const float playButtonW  = buttonWidth(playText);
            const float pauseButtonW = buttonWidth(pauseText);

            if ( ::MMM::UI::FeedbackButton(
                     playText, ImVec2(playButtonW, buttonHeight)) ) {
                if ( isPaused ) {
                    audio.resumeSoundEffect(m_trackId);
                } else {
                    audio.playSoundEffect(m_trackId);
                }
            }

            ImGui::SameLine(0, gap);

            if ( ::MMM::UI::FeedbackButton(
                     pauseText, ImVec2(pauseButtonW, buttonHeight)) ) {
                audio.pauseSoundEffect(m_trackId);
            }

            ImGui::SameLine(0, gap);

            float duration     = (float)audio.getSFXDuration(m_trackId);
            float playbackTime = (float)audio.getSFXPlaybackTime(m_trackId);
            float progress =
                (duration > 0.0f) ? (playbackTime / duration) : 0.0f;
            std::string progressText =
                fmt::format("{:.2f}s / {:.2f}s", playbackTime, duration);

            float remaining = r.width - playButtonW - pauseButtonW - gap * 2.0f;
            remaining       = std::max(0.0f, remaining);
            ImGui::ProgressBar(progress,
                               ImVec2(remaining, buttonHeight),
                               progressText.c_str());
        });
}

void AudioTrackControllerUI::buildAnalysisButtons(CLayVBox&  parent,
                                                  size_t&    rowIndex,
                                                  UIManager* sourceManager)
{
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    const float gap          = layoutMetrics.rowSpacing;
    const float buttonHeight = layoutMetrics.buttonHeight;

    auto& row = getRow(rowIndex++);
    row.setPadding(toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingY),
                   toLayoutPixels(layoutMetrics.rowPaddingY))
        .setSpacing(toLayoutPixels(gap))
        .setAlignment(Alignment::Center());

    std::string rowId = "AT_Analysis_R" + std::to_string(rowIndex);
    row.addElement(
        rowId + "_btns",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, sourceManager, gap, buttonHeight](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            float btnW = std::max(0.0f, (r.width - gap) * 0.5f);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.audio_manager.open_waveform").data(),
                     ImVec2(btnW, buttonHeight)) ) {
                std::string viewName = "AudioWaveform";
                if ( !sourceManager->getView<AudioWaveformView>(viewName) ) {
                    ::MMM::UI::PlayPopupOpenFeedback();
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioWaveformView>(
                            TR("ui.audio_manager.waveform_title").data()));
                }
            }
            ImGui::SameLine(0, gap);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.audio_manager.open_spectrum").data(),
                     ImVec2(btnW, buttonHeight)) ) {
                std::string viewName = "AudioSpectrum";
                if ( !sourceManager->getView<AudioSpectrumView>(viewName) ) {
                    ::MMM::UI::PlayPopupOpenFeedback();
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioSpectrumView>(
                            TR("ui.audio_manager.spectrum_title").data()));
                }
            }
        });

    float rowH = layoutMetrics.rowHeight;
    parent.addLayout(
        (rowId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

}  // namespace MMM::UI
