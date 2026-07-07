#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "ui/imgui/manager/AudioManagerView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <imgui_internal.h>
#include <nfd.h>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 音频资源表格列编号。
enum AudioTableColumn : int {
    /// @brief 音频资源 ID 列。
    AudioTableColumnId = 0,

    /// @brief 音频资源类型列。
    AudioTableColumnType = 1,

    /// @brief 音频资源路径列。
    AudioTableColumnPath = 2
};

/// @brief 使用独立交互音量的音效 key 前缀。
constexpr const char* INTERACTION_SFX_KEY_PREFIX = "ui.";

/// @brief 判断皮肤音效是否属于界面交互音效。
/// @param key 音效 ID。
/// @return 属于交互音效时返回 true。
bool isInteractionSfxKey(const std::string& key)
{
    return key.rfind(INTERACTION_SFX_KEY_PREFIX, 0) == 0;
}

/// @brief 按音频管理器实际字体计算不可折行文本宽度。
float measureAudioManagerText(const char* text)
{
    if ( !text ) return 0.0f;

    auto&   skinCfg = Config::SkinManager::instance();
    ImFont* font    = skinCfg.getFont("filemanager");
    if ( font ) {
        return font
            ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text, nullptr)
            .x;
    }
    return ImGui::CalcTextSize(text).x;
}

/// @brief 使用 UI 快照中的文件管理器字体计算不可折行文本宽度。
float measureAudioManagerText(const char* text, ImFont* font, float fontSize)
{
    if ( !text || !font ) return 0.0f;

    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 向当前最小高度累加一行列表内容和列表间距。
void addAudioManagerListRow(float& height, size_t& rowCount, float rowHeight,
                            float rowSpacing)
{
    if ( rowCount > 0 ) {
        height += rowSpacing;
    }
    height += rowHeight;
    rowCount++;
}

/// @brief 将 ASCII 字符串转换为小写，用于表格排序。
/// @param value 输入字符串。
/// @return 小写后的字符串。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            if ( ch >= 'A' && ch <= 'Z' ) {
                return static_cast<char>(ch - 'A' + 'a');
            }
            return static_cast<char>(ch);
        });
    return value;
}

/// @brief 比较两个 ASCII 字符串的大小，忽略大小写。
/// @param lhs 左侧字符串。
/// @param rhs 右侧字符串。
/// @return 小于返回负值，等于返回 0，大于返回正值。
int compareAsciiText(const std::string& lhs, const std::string& rhs)
{
    return toLowerAscii(lhs).compare(toLowerAscii(rhs));
}

/// @brief 绘制可裁剪、超宽自动滚动的表格单元格文本。
/// @param text 需要绘制的文本。
/// @param cursorPos 单元格起始屏幕坐标。
/// @param width 单元格可用宽度。
/// @param height 行高。
void renderScrollingTableText(const std::string& text, ImVec2 cursorPos,
                              float width, float height)
{
    const float  textWidth = std::max(0.0f, width);
    const ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());
    const float  textH     = ImGui::GetFontSize();
    const float  offsetY   = (height - textH) * 0.5f;

    float offset = 0.0f;
    if ( textSize.x > textWidth ) {
        const float scrollRange = textSize.x - textWidth + 40.0f;
        const float time        = static_cast<float>(ImGui::GetTime());
        float       t           = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset                  = t * scrollRange;
    }

    const ImVec2 textStartPos = cursorPos;
    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textWidth, textStartPos.y + height),
        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

/// @brief 捕获音频管理器同步测量所需的当前帧快照。
UiFrameSnapshot captureAudioManagerUiFrameSnapshot(float dpiScale)
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
    snapshot.fileManagerFont        = skinCfg.getFont("filemanager");
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

/// @brief 捕获当前音频管理器布局输入。
/// @return 当前项目、皮肤音效数量和展开状态。
AudioManagerView::LayoutInputSnapshot
AudioManagerView::captureLayoutInput() const
{
    LayoutInputSnapshot input;
    auto&               engine  = Logic::EditorEngine::instance();
    auto*               project = engine.getCurrentProject();
    input.hasProject            = project != nullptr;
    input.permanentSfxCount =
        Config::SkinManager::instance().getData().audioPaths.size();
    input.showGlobalSettings = m_showGlobalSettings;

    if ( project ) {
        for ( const auto& audio : project->m_audioResources ) {
            if ( audio.m_type == AudioTrackType::Main ) {
                input.mainTrackCount++;
            } else {
                input.effectTrackCount++;
            }
        }
    }
    return input;
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param input 当前布局输入。
/// @return 完全匹配时返回 true。
bool AudioManagerView::layoutMetricsMatch(const LayoutMetricsCache&  cache,
                                          const UiFrameSnapshot&     snapshot,
                                          const LayoutInputSnapshot& input)
{
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };
    const bool inputMatch =
        cache.input.hasProject == input.hasProject &&
        cache.input.permanentSfxCount == input.permanentSfxCount &&
        cache.input.mainTrackCount == input.mainTrackCount &&
        cache.input.effectTrackCount == input.effectTrackCount &&
        cache.input.showGlobalSettings == input.showGlobalSettings;

    return cache.valid && inputMatch &&
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

/// @brief 构造音频管理器布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param input 当前布局输入。
/// @return 音频管理器布局测量结果。
AudioManagerView::LayoutMetricsCache AudioManagerView::buildLayoutMetrics(
    const UiFrameSnapshot& snapshot, const LayoutInputSnapshot& input)
{
    LayoutMetricsCache cache;
    cache.valid                  = true;
    cache.input                  = input;
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

    const float scale       = std::max(1.0f, snapshot.dpiScale);
    const float frameH      = snapshot.frameHeight;
    const float frameWithSp = snapshot.frameHeightWithSpacing;
    const float itemSpacing = std::floor(snapshot.itemSpacing * scale);
    const float rowSpacingY =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    const float labelPad   = std::floor(12.0f * scale);
    const float rootPad    = std::floor(4.0f * scale);
    const float footerPadX = rootPad;
    const float sectionSpacing =
        std::ceil(std::max(12.0f * scale, itemSpacing));
    const float footerSpacing =
        std::ceil(std::max(2.0f * scale, itemSpacing * 0.25f));
    const float controlColGap = std::ceil(std::max(8.0f * scale, itemSpacing));
    const float labelColGap =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    const float rowPaddingY = snapshot.framePadding.y * 2.0f;
    const float controlRowH = std::ceil(
        std::max({ frameH, snapshot.fontSize + rowPaddingY, 32.0f * scale }));
    const float muteButtonSize  = std::ceil(std::max(frameH, 30.0f * scale));
    const float audioItemHeight = std::ceil(
        std::max({ frameH, snapshot.fontSize + rowPaddingY, 28.0f * scale }));
    const float hintSpacerH = std::ceil(std::max(20.0f * scale, frameH * 0.5f));
    const float hintRowH    = std::ceil(std::max(30.0f * scale, frameH));
    const float importButtonH = std::ceil(std::max(32.0f * scale, frameH));
    const float importButtonGap =
        std::ceil(std::max(8.0f * scale, itemSpacing));
    cache.rootPadding        = rootPad;
    cache.sectionSpacing     = sectionSpacing;
    cache.listRowSpacing     = rowSpacingY;
    cache.audioItemHeight    = audioItemHeight;
    cache.hintSpacerHeight   = hintSpacerH;
    cache.hintRowHeight      = hintRowH;
    cache.footerPaddingX     = footerPadX;
    cache.footerSpacing      = footerSpacing;
    cache.footerHeaderHeight = frameWithSp;
    cache.controlRowHeight   = controlRowH;
    cache.controlColumnGap   = controlColGap;
    cache.labelColumnGap     = labelColGap;
    cache.muteButtonSize     = muteButtonSize;
    cache.importButtonHeight = importButtonH;
    cache.importButtonGap    = importButtonGap;
    ImFont* font = snapshot.fileManagerFont
                       ? snapshot.fileManagerFont
                       : (snapshot.contentFont ? snapshot.contentFont
                                               : snapshot.fallbackFont);

    const std::array<const char*, 5> controlLabels{
        TR("ui.audio_manager.output_device").data(),
        TR("ui.audio_manager.global_volume").data(),
        TR("ui.audio_manager.bgm_gain").data(),
        TR("ui.audio_manager.sfx_gain").data(),
        TR("ui.audio_manager.interaction_sfx_volume").data()
    };

    float labelWidth = 0.0f;
    for ( const char* label : controlLabels ) {
        labelWidth =
            std::max(labelWidth,
                     measureAudioManagerText(label, font, snapshot.fontSize));
    }
    labelWidth += labelPad;
    cache.footerLabelWidth = labelWidth;

    const float sliderValueW =
        std::max(measureAudioManagerText("0.00", font, snapshot.fontSize),
                 measureAudioManagerText("100%", font, snapshot.fontSize));
    const float sliderMinW = sliderValueW + snapshot.framePadding.x * 4.0f +
                             std::floor(48.0f * scale);

    const std::array<const char*, 4> headers{
        TR("ui.audio_manager.column_id").data(),
        TR("ui.audio_manager.column_type").data(),
        TR("ui.audio_manager.column_path").data(),
        TR("ui.audio_manager.global_settings").data(),
    };

    float headerWidth = 0.0f;
    for ( const char* header : headers ) {
        headerWidth = std::max(
            headerWidth,
            frameH + itemSpacing +
                measureAudioManagerText(header, font, snapshot.fontSize));
    }

    const float controlRowWidth = footerPadX * 2.0f + labelWidth +
                                  controlColGap + muteButtonSize +
                                  controlColGap + sliderMinW;
    float       minWidth =
        std::ceil(rootPad * 2.0f + std::max({ controlRowWidth, headerWidth }));

    float        listHeight = 0.0f;
    size_t       listRows   = 0;
    const float  headerRowH = cache.footerHeaderHeight;
    const size_t audioResourceCount =
        input.permanentSfxCount + input.mainTrackCount + input.effectTrackCount;
    if ( input.hasProject || audioResourceCount > 0 ) {
        const size_t visibleRows = std::max<size_t>(audioResourceCount, 1);
        addAudioManagerListRow(
            listHeight,
            listRows,
            headerRowH + static_cast<float>(visibleRows) * audioItemHeight,
            rowSpacingY);
    } else {
        addAudioManagerListRow(listHeight, listRows, hintSpacerH, rowSpacingY);
        addAudioManagerListRow(listHeight, listRows, hintRowH, rowSpacingY);
    }

    float footerH = cache.footerHeaderHeight;
    if ( input.showGlobalSettings ) {
        footerH += 5.0f * controlRowH + 5.0f * footerSpacing;
    }
    footerH += importButtonH + importButtonGap;
    cache.globalControlsHeight = footerH - (importButtonH + importButtonGap);
    cache.footerHeight         = footerH;

    float minHeight      = std::ceil(rootPad * 2.0f + listHeight + footerH);
    cache.minContentSize = ImVec2(minWidth, minHeight);
    return cache;
}

/// @brief 获取音频管理器布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和资源数量匹配的布局测量结果。
const AudioManagerView::LayoutMetricsCache& AudioManagerView::getLayoutMetrics(
    float dpiScale) const
{
    const LayoutInputSnapshot input = captureLayoutInput();
    const UiFrameSnapshot     snapshot =
        captureAudioManagerUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(m_layoutMetricsCache, snapshot, input) ) {
        m_layoutMetricsCache = buildLayoutMetrics(snapshot, input);
    }
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧是否需要准备音频管理器布局数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要后台准备时返回 true。
bool AudioManagerView::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    m_prepareLayoutInput = captureLayoutInput();
    return !layoutMetricsMatch(
        m_layoutMetricsCache, snapshot, m_prepareLayoutInput);
}

/// @brief 在线程池中准备音频管理器布局测量数据。
/// @param snapshot 当前帧 UI 快照。
void AudioManagerView::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    m_preparedLayoutMetricsCache =
        buildLayoutMetrics(snapshot, m_prepareLayoutInput);
    m_hasPreparedLayoutMetrics = true;
}

/// @brief 将后台准备好的布局测量数据切换给主线程使用。
void AudioManagerView::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedLayoutMetrics ) {
        return;
    }

    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 获取音频管理器中不可再换行控件所需的最小内容尺寸。
ImVec2 AudioManagerView::getMinContentSize(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).minContentSize;
}

// 内部绘制逻辑 (Clay/ImGui)
void AudioManagerView::onUpdate(LayoutContext& layoutContext,
                                UIManager*     sourceManager)
{
    auto& engine       = Logic::EditorEngine::instance();
    auto* project      = engine.getCurrentProject();
    auto& skinCfg      = Config::SkinManager::instance();
    auto& audioManager = Audio::AudioManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    const auto& layoutMetrics  = getLayoutMetrics(dpiScale);
    const float maxLabelW      = layoutMetrics.footerLabelWidth;
    auto        toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    const auto playbackBackend = audioManager.getPlaybackBackend();
    if ( m_outputDevicesDirty ||
         m_cachedOutputDeviceBackend != playbackBackend ) {
        m_cachedOutputDevices       = audioManager.listOutputDevices();
        m_cachedOutputDeviceBackend = playbackBackend;
        m_outputDevicesDirty        = false;
    }

    size_t rowIndex     = 0;
    size_t subHBoxIndex = 0;

    CLayVBox rootVBox;

    // 渲染一行音频控制项 (Label + Mute + Slider)
    auto addControlRow = [&](CLayVBox&   parent,
                             const char* id,
                             const char* label,
                             float       labelWidth,
                             float       volume,
                             bool        muted,
                             float       levelL,
                             float       levelR,
                             float       minVal,
                             float       maxVal,
                             const char* tooltip,
                             const char* format,
                             auto        onVolumeChange,
                             auto        onMuteChange) {
        // 创建水平行布局 (主容器)
        CLayHBox& row = this->getRow(rowIndex++);
        row.clear();
        row.setPadding(0, 0, 0, 0)
            .setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        // --- A. 左侧容器: (FixW) [ 标签 + 弹簧 ] ---
        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(toLayoutPixels(layoutMetrics.labelColumnGap))
            .setAlignment(Alignment::Center());

        // 1. 标签
        leftBox.addElement(std::string(id) + "_lbl",
                           Sizing::Fixed(ImGui::CalcTextSize(label).x),
                           Sizing::Grow(),
                           [label](Clay_BoundingBox r, bool) {
                               float textH  = ImGui::CalcTextSize(label).y;
                               float offset = (r.height - textH) * 0.5f;
                               ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                               ImGui::Text("%s", label);
                           });

        // 2. 弹簧 (填充 FixW 剩余空间)
        leftBox.addSpring();

        // 将左侧容器加入主行
        row.addLayout((std::string(id) + "_left").c_str(),
                      leftBox,
                      Sizing::Fixed(labelWidth),
                      Sizing::Grow());

        // --- B. 右侧容器: (GrowW) [ 按钮 + 滑条 ] ---
        CLayHBox& rightBox = this->getSubHBox(subHBoxIndex++);
        rightBox.clear();
        rightBox.setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        // 1. 静音按钮
        rightBox.addElement(
            std::string(id) + "_mute",
            Sizing::Fixed(layoutMetrics.muteButtonSize),
            Sizing::Fixed(layoutMetrics.muteButtonSize),
            [&, id, muted, volume, tooltip, onMuteChange](Clay_BoundingBox r,
                                                          bool isHovered) {
                const char* icon = ICON_MMM_VOLUME_MUTE;
                if ( !muted ) {
                    if ( volume <= 0.33f )
                        icon = ICON_MMM_VOLUME_OFF;
                    else if ( volume <= 0.66f )
                        icon = ICON_MMM_VOLUME_LOW;
                    else
                        icon = ICON_MMM_VOLUME_HIGH;
                }

                ImGui::SetCursorScreenPos(
                    { r.x,
                      r.y + (r.height - layoutMetrics.muteButtonSize) * 0.5f });
                if ( muted ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                Utils::pushFixedButtonStyleVars();
                if ( ::MMM::UI::FeedbackButton(
                         (std::string(icon) + "##Btn" + id).c_str(),
                         ImVec2(layoutMetrics.muteButtonSize,
                                layoutMetrics.muteButtonSize)) ) {
                    onMuteChange(!muted);
                }
                Utils::popFixedButtonStyleVars();
                if ( muted ) {
                    ImGui::PopStyleColor();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s (%s)",
                                      tooltip,
                                      muted
                                          ? TR("ui.audio_manager.unmute").data()
                                          : TR("ui.audio_manager.mute").data());
                }
            });

        // 2. 拉条 (自动延伸)
        rightBox.addElement(
            std::string(id) + "_slider",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.controlRowHeight),
            [&, id, volume, minVal, maxVal, format, tooltip, onVolumeChange](
                Clay_BoundingBox r, bool isHovered) {
                float frameH = ImGui::GetFrameHeight();
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - frameH) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                float val = volume;
                if ( ::MMM::UI::FeedbackSliderFloat(
                         (std::string("##Slider") + id).c_str(),
                         &val,
                         minVal,
                         maxVal,
                         format) ) {
                    onVolumeChange(val);
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", tooltip);
                }
            });

        // 将右侧容器加入主行
        row.addLayout((std::string(id) + "_right").c_str(),
                      rightBox,
                      Sizing::Grow(),
                      Sizing::Grow());

        parent.addLayout((std::string(id) + "_row").c_str(),
                         row,
                         Sizing::Grow(),
                         Sizing::Fixed(layoutMetrics.controlRowHeight));
    };

    auto addDeviceComboRow = [&](CLayVBox&   parent,
                                 const char* id,
                                 const char* label,
                                 float       labelWidth) {
        CLayHBox& row = this->getRow(rowIndex++);
        row.clear();
        row.setPadding(0, 0, 0, 0)
            .setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(toLayoutPixels(layoutMetrics.labelColumnGap))
            .setAlignment(Alignment::Center());
        leftBox.addElement(std::string(id) + "_lbl",
                           Sizing::Fixed(ImGui::CalcTextSize(label).x),
                           Sizing::Grow(),
                           [label](Clay_BoundingBox r, bool) {
                               const float textH = ImGui::CalcTextSize(label).y;
                               ImGui::SetCursorScreenPos(
                                   { r.x, r.y + (r.height - textH) * 0.5f });
                               ImGui::Text("%s", label);
                           });
        leftBox.addSpring();

        row.addLayout((std::string(id) + "_left").c_str(),
                      leftBox,
                      Sizing::Fixed(labelWidth),
                      Sizing::Grow());

        CLayHBox& rightBox = this->getSubHBox(subHBoxIndex++);
        rightBox.clear();
        rightBox.setAlignment(Alignment::Center());
        rightBox.addElement(
            std::string(id) + "_combo",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.controlRowHeight),
            [&, id](Clay_BoundingBox r, bool) {
                const std::string defaultDeviceLabel =
                    TR("ui.audio_manager.output_device_default").data();
                const std::string currentDeviceName =
                    audioManager.getOutputDeviceName();
                const std::string previewName = currentDeviceName.empty()
                                                    ? defaultDeviceLabel
                                                    : currentDeviceName;

                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo(
                         (std::string("##Combo") + id).c_str(),
                         previewName.c_str()) ) {
                    for ( const auto& device : m_cachedOutputDevices ) {
                        const std::string optionLabel =
                            device.isDefault ? defaultDeviceLabel : device.name;
                        const bool selected = currentDeviceName == device.name;
                        if ( ::MMM::UI::FeedbackSelectable(optionLabel.c_str(),
                                                           selected) ) {
                            if ( audioManager.setOutputDeviceName(
                                     device.name) ) {
                                m_outputDevicesDirty = true;
                            }
                        }
                        if ( selected ) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s",
                        TR("ui.audio_manager.output_device_tooltip").data());
                }
            });

        row.addLayout((std::string(id) + "_right").c_str(),
                      rightBox,
                      Sizing::Grow(),
                      Sizing::Grow());
        parent.addLayout((std::string(id) + "_row").c_str(),
                         row,
                         Sizing::Grow(),
                         Sizing::Fixed(layoutMetrics.controlRowHeight));
    };

    const auto& skinData = Config::SkinManager::instance().getData();

    auto audioTableTypeLabel = [](AudioTableRowKind kind) -> std::string {
        switch ( kind ) {
        case AudioTableRowKind::InteractionSfx:
            return TR("ui.audio_manager.type_interaction_sfx").data();
        case AudioTableRowKind::PermanentSfx:
            return TR("ui.audio_manager.type_permanent_sfx").data();
        case AudioTableRowKind::MainTrack:
            return TR("ui.audio_manager.type_main_track").data();
        case AudioTableRowKind::ProjectSfx:
            return TR("ui.audio_manager.type_project_sfx").data();
        }
        return TR("ui.file_manager.value_unknown").data();
    };

    auto audioTableTypeSortRank = [](AudioTableRowKind kind) {
        switch ( kind ) {
        case AudioTableRowKind::InteractionSfx: return 0;
        case AudioTableRowKind::PermanentSfx: return 1;
        case AudioTableRowKind::MainTrack: return 2;
        case AudioTableRowKind::ProjectSfx: return 3;
        }
        return 4;
    };

    auto rebuildAudioTableRows = [&]() {
        m_audioTableRows.clear();
        const size_t projectAudioCount =
            project ? project->m_audioResources.size() : 0;
        m_audioTableRows.reserve(skinData.audioPaths.size() +
                                 projectAudioCount);

        for ( const auto& [key, path] : skinData.audioPaths ) {
            AudioTableRow row;
            row.m_id   = key;
            row.m_path = Config::pathToUtf8(path);
            row.m_type = AudioTrackType::Effect;
            row.m_kind = isInteractionSfxKey(key)
                             ? AudioTableRowKind::InteractionSfx
                             : AudioTableRowKind::PermanentSfx;
            m_audioTableRows.push_back(std::move(row));
        }

        if ( project ) {
            for ( const auto& audio : project->m_audioResources ) {
                AudioTableRow row;
                row.m_id   = audio.m_id;
                row.m_path = audio.m_path;
                row.m_type = audio.m_type;
                row.m_kind = audio.m_type == AudioTrackType::Main
                                 ? AudioTableRowKind::MainTrack
                                 : AudioTableRowKind::ProjectSfx;
                m_audioTableRows.push_back(std::move(row));
            }
        }

        std::stable_sort(
            m_audioTableRows.begin(),
            m_audioTableRows.end(),
            [&](const AudioTableRow& lhs, const AudioTableRow& rhs) {
                int compareResult = 0;
                switch ( m_audioTableSortKey ) {
                case AudioTableSortKey::Id:
                    compareResult = compareAsciiText(lhs.m_id, rhs.m_id);
                    break;
                case AudioTableSortKey::Type:
                    compareResult = audioTableTypeSortRank(lhs.m_kind) -
                                    audioTableTypeSortRank(rhs.m_kind);
                    break;
                case AudioTableSortKey::Path:
                    compareResult = compareAsciiText(lhs.m_path, rhs.m_path);
                    break;
                }

                if ( compareResult == 0 ) {
                    compareResult = compareAsciiText(lhs.m_id, rhs.m_id);
                }
                if ( compareResult == 0 ) {
                    compareResult = compareAsciiText(lhs.m_path, rhs.m_path);
                }
                if ( m_audioTableSortDirection == SortDirection::Descending ) {
                    compareResult = -compareResult;
                }
                return compareResult < 0;
            });

        m_cachedPermanentSfxCount  = skinData.audioPaths.size();
        m_cachedProjectAudioCount  = projectAudioCount;
        m_cachedAudioTableProject  = project;
        m_audioTableSortCacheDirty = false;
    };

    auto syncAudioTableSortSpecs = [&]() {
        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if ( !sortSpecs || sortSpecs->SpecsCount <= 0 ||
             !sortSpecs->SpecsDirty ) {
            return;
        }

        const ImGuiTableColumnSortSpecs& primarySpec = sortSpecs->Specs[0];
        AudioTableSortKey                newSortKey  = AudioTableSortKey::Id;
        if ( primarySpec.ColumnIndex == AudioTableColumnType ) {
            newSortKey = AudioTableSortKey::Type;
        } else if ( primarySpec.ColumnIndex == AudioTableColumnPath ) {
            newSortKey = AudioTableSortKey::Path;
        }

        const SortDirection newDirection =
            primarySpec.SortDirection == ImGuiSortDirection_Descending
                ? SortDirection::Descending
                : SortDirection::Ascending;
        if ( newSortKey != m_audioTableSortKey ||
             newDirection != m_audioTableSortDirection ) {
            m_audioTableSortKey        = newSortKey;
            m_audioTableSortDirection  = newDirection;
            m_audioTableSortCacheDirty = true;
        }
        sortSpecs->SpecsDirty = false;
    };

    auto resetAudioTableSort = [&]() {
        if ( m_audioTableSortKey != AudioTableSortKey::Id ||
             m_audioTableSortDirection != SortDirection::Ascending ) {
            m_audioTableSortKey        = AudioTableSortKey::Id;
            m_audioTableSortDirection  = SortDirection::Ascending;
            m_audioTableSortCacheDirty = true;
        }
    };

    auto renderAudioTableHeaderContextMenu = [&]() {
        ImGuiTable* table = ImGui::GetCurrentTable();
        if ( !table ) return;

        ImGuiStyle&  style = ImGui::GetStyle();
        const ImVec2 popupPadding(std::max(style.WindowPadding.x, 8.0f),
                                  std::max(style.WindowPadding.y, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                                   std::max(style.ItemSpacing.y, 4.0f)));
        const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
        if ( !popupOpen ) {
            ImGui::PopStyleVar(2);
            return;
        }

        const int contextColumn =
            table->ContextPopupColumn >= 0 &&
                    table->ContextPopupColumn < table->ColumnsCount
                ? table->ContextPopupColumn
                : -1;
        if ( contextColumn >= 0 &&
             (ImGui::TableGetColumnFlags(contextColumn) &
              ImGuiTableColumnFlags_IsEnabled) != 0 &&
             ::MMM::UI::FeedbackMenuItem(
                 TR("ui.audio_manager.table_menu.size_column_fit").data()) ) {
            ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
        }

        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.audio_manager.table_menu.size_all_default").data()) ) {
            ImGui::TableSetColumnWidthAutoAll(table);
        }

        if ( ::MMM::UI::FeedbackBeginMenu(
                 TR("ui.audio_manager.table_menu.reset").data()) ) {
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_all").data()) ) {
                ImGui::TableResetSettings(table);
                resetAudioTableSort();
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_columns").data()) ) {
                ImGui::TableSetColumnWidthAutoAll(table);
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.show_all_columns")
                         .data()) ) {
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    ImGui::TableSetColumnEnabled(column, true);
                }
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_sort").data()) ) {
                resetAudioTableSort();
            }
            ::MMM::UI::FeedbackEndMenu();
        }

        ImGui::Separator();

        const std::array<const char*, 3> columnLabels{
            TR("ui.audio_manager.column_id").data(),
            TR("ui.audio_manager.column_type").data(),
            TR("ui.audio_manager.column_path").data()
        };
        int enabledColumnCount = 0;
        for ( int column = 0; column < table->ColumnsCount; ++column ) {
            if ( (ImGui::TableGetColumnFlags(column) &
                  ImGuiTableColumnFlags_IsEnabled) != 0 ) {
                enabledColumnCount++;
            }
        }
        for ( int column = 0; column < table->ColumnsCount; ++column ) {
            const bool enabled   = (ImGui::TableGetColumnFlags(column) &
                                    ImGuiTableColumnFlags_IsEnabled) != 0;
            const bool canToggle = !enabled || enabledColumnCount > 1;
            if ( ::MMM::UI::FeedbackMenuItem(
                     columnLabels[column], nullptr, enabled, canToggle) ) {
                ImGui::TableSetColumnEnabled(column, !enabled);
            }
        }

        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
    };

    auto renderAudioResourcesTable = [&](Clay_BoundingBox r, bool) {
        ImGui::SetCursorScreenPos({ r.x, r.y });
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if ( ImGui::BeginTable(
                 "AudioManagerTable", 3, tableFlags, { r.width, r.height }) ) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_id").data(),
                ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_type").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 116.0f * ImGui::GetFontSize() / 17.0f));
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_path").data(),
                ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                table->DisableDefaultContextMenu = true;
            }
            ImGui::TableHeadersRow();
            syncAudioTableSortSpecs();
            renderAudioTableHeaderContextMenu();

            const size_t projectAudioCount =
                project ? project->m_audioResources.size() : 0;
            if ( m_audioTableSortCacheDirty ||
                 m_cachedPermanentSfxCount != skinData.audioPaths.size() ||
                 m_cachedProjectAudioCount != projectAudioCount ||
                 m_cachedAudioTableProject != project ) {
                rebuildAudioTableRows();
            }

            const float      rowHeight = layoutMetrics.audioItemHeight;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_audioTableRows.size()), rowHeight);
            while ( clipper.Step() ) {
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                      ++row ) {
                    const auto& rowData =
                        m_audioTableRows[static_cast<size_t>(row)];
                    const std::string typeText =
                        audioTableTypeLabel(rowData.m_kind);

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    ImGui::TableNextColumn();

                    const ImVec2 idCellPos   = ImGui::GetCursorScreenPos();
                    const float  idCellWidth = ImGui::GetContentRegionAvail().x;
                    const std::string rowId = fmt::format("##AudioRow_{}_{}_{}",
                                                          rowData.m_id,
                                                          rowData.m_path,
                                                          row);
                    const bool        clicked = ::MMM::UI::FeedbackSelectable(
                        rowId.c_str(),
                        false,
                        ImGuiSelectableFlags_SpanAllColumns,
                        { 0.0f, rowHeight });
                    const bool hovered = ImGui::IsItemHovered();
                    if ( clicked ) {
                        const auto controllerType =
                            rowData.m_type == AudioTrackType::Main
                                ? AudioTrackControllerUI::TrackType::Main
                                : AudioTrackControllerUI::TrackType::Effect;
                        sourceManager->openAudioTrackController(
                            rowData.m_id, rowData.m_id, controllerType);
                    }

                    if ( hovered &&
                         (rowData.m_kind == AudioTableRowKind::MainTrack ||
                          rowData.m_kind == AudioTableRowKind::ProjectSfx) &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                        m_manageTrackId   = rowData.m_id;
                        m_manageTrackType = rowData.m_type;
                        m_openManageModal = true;
                    }
                    if ( hovered ) {
                        ImGui::SetTooltip(
                            "%s\n%s: %s",
                            rowData.m_path.c_str(),
                            TR("ui.audio_manager.column_type").data(),
                            typeText.c_str());
                    }

                    const std::string idText =
                        std::string(ICON_MMM_MUSIC) + "  " + rowData.m_id;
                    renderScrollingTableText(
                        idText, idCellPos, idCellWidth, rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(typeText,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(rowData.m_path,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);
                }
            }
            ImGui::EndTable();
        }
    };

    auto renderInitialHint = [&](Clay_BoundingBox r, bool) {
        const char* text     = TR("ui.audio_manager.initial_hint").data();
        const auto  textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorScreenPos(
            { r.x + std::max(0.0f, r.width - textSize.x) * 0.5f,
              r.y + std::max(0.0f, r.height - textSize.y) * 0.5f });
        ImGui::TextDisabled("%s", text);
    };

    // 底部全局控制 - 始终显示
    CLayVBox footerVBox;
    // 使用对称的水平内边距，移除手动 Indent，确保左右居中对齐
    footerVBox
        .setPadding(toLayoutPixels(layoutMetrics.footerPaddingX),
                    toLayoutPixels(layoutMetrics.footerPaddingX),
                    0,
                    0)
        .setSpacing(toLayoutPixels(layoutMetrics.footerSpacing));

    footerVBox.addElement("FooterHeader",
                          Sizing::Grow(),
                          Sizing::Fixed(layoutMetrics.footerHeaderHeight),
                          [&](Clay_BoundingBox r, bool isHovered) {
                              Utils::renderCollapsingHeader(
                                  TR("ui.audio_manager.global_settings").data(),
                                  &m_showGlobalSettings,
                                  r);
                          });

    if ( m_showGlobalSettings ) {
        addDeviceComboRow(footerVBox,
                          "OutputDevice",
                          TR("ui.audio_manager.output_device").data(),
                          maxLabelW);

        addControlRow(
            footerVBox,
            "Global",
            TR("ui.audio_manager.global_volume").data(),
            maxLabelW,
            audioManager.getGlobalVolume(),
            audioManager.isGlobalMuted(),
            audioManager.getOutputLevelL(),
            audioManager.getOutputLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.global_volume").data(),
            "%.2f",
            [&](float v) { audioManager.setGlobalVolume(v); },
            [&](bool m) { audioManager.setGlobalMute(m); });

        addControlRow(
            footerVBox,
            "BGMGain",
            TR("ui.audio_manager.bgm_gain").data(),
            maxLabelW,
            audioManager.getBGMGain(),
            audioManager.isBGMGainMuted(),
            audioManager.getMainTrackLevelL(),
            audioManager.getMainTrackLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.bgm_gain").data(),
            "%.2f",
            [&](float v) { audioManager.setBGMGain(v); },
            [&](bool m) { audioManager.setBGMGainMute(m); });

        addControlRow(
            footerVBox,
            "SFXGain",
            TR("ui.audio_manager.sfx_gain").data(),
            maxLabelW,
            audioManager.getSFXGain(),
            audioManager.isSFXGainMuted(),
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            TR("ui.audio_manager.sfx_gain").data(),
            "%.2f",
            [&](float v) { audioManager.setSFXGain(v); },
            [&](bool m) { audioManager.setSFXGainMute(m); });

        addControlRow(
            footerVBox,
            "InteractionSFXGain",
            TR("ui.audio_manager.interaction_sfx_volume").data(),
            maxLabelW,
            audioManager.getInteractionSFXGain(),
            audioManager.isInteractionSFXGainMuted(),
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            TR("ui.audio_manager.interaction_sfx_volume").data(),
            "%.2f",
            [&](float v) { audioManager.setInteractionSFXGain(v); },
            [&](bool m) { audioManager.setInteractionSFXGainMute(m); });
    }

    // --- 执行分段渲染 ---
    // 1. 动态计算页脚高度
    float footerH = layoutMetrics.footerHeight;

    // 2. 渲染顶部列表区域 (自动占据剩余空间)
    rootVBox
        .setPadding(toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding))
        .setSpacing(toLayoutPixels(layoutMetrics.sectionSpacing))
        .addElement("listContentArea",
                    Sizing::Grow(),
                    Sizing::Grow(),
                    [&](Clay_BoundingBox r, bool isHovered) {
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                            ImVec2(0.0f, 0.0f));
                        ImGui::BeginChild("AudioListChild",
                                          { r.width, r.height },
                                          false,
                                          ImGuiWindowFlags_None);
                        const ImVec2 childPos = ImGui::GetCursorScreenPos();
                        const ImVec2 childAvail =
                            ImGui::GetContentRegionAvail();
                        Clay_BoundingBox childBox{ .x      = childPos.x,
                                                   .y      = childPos.y,
                                                   .width  = childAvail.x,
                                                   .height = childAvail.y };
                        if ( project || !skinData.audioPaths.empty() ) {
                            renderAudioResourcesTable(childBox, isHovered);
                        } else {
                            renderInitialHint(childBox, isHovered);
                        }

                        ImGui::EndChild();
                        ImGui::PopStyleVar();
                    });

    ImVec2 totalSize = rootVBox.renderInCurrent(
        layoutContext.m_startPos,
        { layoutContext.m_avail.x,
          std::max(0.0f, layoutContext.m_avail.y - footerH) });

    // 3. 底部全局控制区域 (独立渲染)
    ImVec2 footerPos = { layoutContext.m_startPos.x,
                         layoutContext.m_startPos.y + totalSize.y };

    float controlH = layoutMetrics.globalControlsHeight;
    footerVBox.renderInCurrent(footerPos,
                               { layoutContext.m_avail.x, controlH });

    // 4. 底部加号按钮 (全宽)
    CLayHBox bottomBtnHBox;
    bottomBtnHBox
        .setPadding(toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    0,
                    0)
        .setAlignment(Alignment::Center())
        .addElement(
            "Audio_ImportNew",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.importButtonHeight),
            [&engine](Clay_BoundingBox r, bool isHovered) {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                Utils::pushFixedButtonStyleVars();

                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                bgCol.w *= 0.5f;
                float rounding = ImGui::GetStyle().FrameRounding;

                if ( isHovered ) bgCol.w *= 1.5f;

                dl->AddRectFilled({ r.x, r.y },
                                  { r.x + r.width, r.y + r.height },
                                  ImGui::ColorConvertFloat4ToU32(bgCol),
                                  rounding);

                if ( ::MMM::UI::FeedbackButton(
                         fmt::format("{}##ImportAudio", ICON_MMM_PLUS).c_str(),
                         ImVec2(r.width, r.height)) ) {
                    auto& settings = engine.getEditorConfig().settings;
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = {
                            { "Audio Files", "mp3,ogg,wav,flac,opus,aac,m4a" }
                        };
                        nfdresult_t result =
                            NFD_OpenDialogU8(&outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            Event::EventBus::instance().publish(
                                Event::AudioImportTriggerEvent{ outPath });
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        IGFD::FileDialogConfig fdConfig;
                        fdConfig.path     = settings.lastFilePickerPath;
                        fdConfig.fileName = "";
                        fdConfig.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AudioImportPicker",
                            TR("ui.audio_manager.import_audio").data(),
                            ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a",
                            fdConfig);
                    }
                }

                Utils::popFixedButtonStyleVars();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s", TR("ui.audio_manager.import_audio").data());
                }
            });

    ImVec2 btnPos = { footerPos.x,
                      footerPos.y + controlH + layoutMetrics.importButtonGap };
    bottomBtnHBox.renderInCurrent(
        btnPos, { layoutContext.m_avail.x, layoutMetrics.importButtonHeight });

    // --- 5. 音轨管理窗口 ---
    bool showManageModal = !m_manageTrackId.empty();
    if ( showManageModal ) {
        std::string windowTitle =
            fmt::format("{} {}",
                        TR("ui.audio_manager.manage_title").data(),
                        m_manageTrackId);
        if ( m_openManageModal ) {
            m_openManageModal = false;
        }
        Utils::CenteredModalPopupScope manageWindowScope(dpiScale);
        if ( manageWindowScope.beginWindow(windowTitle.c_str(),
                                           &showManageModal,
                                           ImGuiWindowFlags_NoCollapse,
                                           { 420 * dpiScale, 0.0f }) ) {
            if ( !showManageModal ) {
                m_manageTrackId = "";
            }

            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox    modalLayout;
            const auto& modalStyle = ImGui::GetStyle();
            float       padding =
                std::max(16.0f * dpiScale, modalStyle.WindowPadding.x);
            const float modalGap =
                std::max(12.0f * dpiScale, modalStyle.ItemSpacing.y);
            const float rowGap =
                std::max(8.0f * dpiScale, modalStyle.ItemSpacing.x);
            const float modalButtonH =
                std::max(32.0f * dpiScale, ImGui::GetFrameHeight());
            const float modalComboH =
                std::max(28.0f * dpiScale, ImGui::GetFrameHeight());
            const float typeLabelW =
                std::max(100.0f * dpiScale,
                         measureAudioManagerText(
                             TR("ui.audio_manager.track_type").data()) +
                             modalStyle.FramePadding.x * 2.0f);
            const float removeButtonW =
                std::max(140.0f * dpiScale,
                         measureAudioManagerText(
                             TR("ui.audio_manager.remove_track").data()) +
                             modalStyle.FramePadding.x * 2.0f);
            const float cancelButtonW = std::max(
                100.0f * dpiScale,
                measureAudioManagerText(TR("ui.common.cancel").data()) +
                    modalStyle.FramePadding.x * 2.0f);
            const uint16_t modalPaddingPx = toLayoutPixels(padding);
            const uint16_t modalGapPx     = toLayoutPixels(modalGap);
            const uint16_t rowGapPx       = toLayoutPixels(rowGap);
            modalLayout.setPadding(
                modalPaddingPx, modalPaddingPx, modalPaddingPx, modalPaddingPx);
            modalLayout.setSpacing(modalGapPx);

            // 1. 标题与分隔线 (移除了冗余的 Text，仅保留分隔线)
            modalLayout.addElement(
                "ModalSep1",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 2. 配置项 (音轨类型)
            CLayHBox typeRow;
            typeRow.setAlignment(Alignment::Center());
            typeRow.setSpacing(rowGapPx);
            typeRow.addElement(
                "TypeLabel",
                Sizing::Fixed(typeLabelW),
                Sizing::Grow(),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s:",
                                TR("ui.audio_manager.track_type").data());
                });
            typeRow.addElement(
                "TypeCombo",
                Sizing::Grow(),
                Sizing::Fixed(modalComboH),
                [=, this, &engine](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    int currentType =
                        (m_manageTrackType == AudioTrackType::Main ? 0 : 1);
                    const char* typeNames[] = { "Main", "Effect" };
                    ImGui::SetNextItemWidth(r.width);
                    if ( ::MMM::UI::FeedbackCombo(
                             "##TrackType", &currentType, typeNames, 2) ) {
                        m_manageTrackType =
                            (currentType == 0 ? AudioTrackType::Main
                                              : AudioTrackType::Effect);
                        m_audioTableSortCacheDirty = true;
                        engine.pushCommand(Logic::CmdUpdateAudioResource{
                            m_manageTrackId, m_manageTrackType });
                        ImGui::CloseCurrentPopup();
                    }
                });
            modalLayout.addLayout("TypeRow",
                                  typeRow,
                                  Sizing::Grow(),
                                  Sizing::Fixed(modalButtonH));

            modalLayout.addElement(
                "ModalSep2",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 3. 操作按钮
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(modalGapPx);
            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(removeButtonW),
                Sizing::Fixed(modalButtonH),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.audio_manager.remove_track").data(),
                             { r.width, r.height }) ) {
                        ImGui::OpenPopup("RemoveTrackConfirm");
                    }
                });
            btnRow.addElement("CancelBtn",
                              Sizing::Fixed(cancelButtonW),
                              Sizing::Fixed(modalButtonH),
                              [=, this](Clay_BoundingBox r, bool) {
                                  ImGui::SetCursorScreenPos({ r.x, r.y });
                                  if ( ::MMM::UI::FeedbackButton(
                                           TR("ui.common.cancel").data(),
                                           { r.width, r.height }) ) {
                                      m_manageTrackId = "";
                                  }
                              });
            modalLayout.addLayout(
                "BtnRow", btnRow, Sizing::Grow(), Sizing::Fixed(modalButtonH));

            // 渲染布局
            // 注意：在模态框中使用 renderInCurrent 来适配 ImGui 的自动大小计算
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // 二次确认弹窗
            {
                Utils::CenteredModalPopupScope removeModalScope(dpiScale);
                if ( removeModalScope.begin("RemoveTrackConfirm") ) {
                    ImGui::Text("%s",
                                TR("ui.audio_manager.remove_confirm").data());
                    ImGui::Spacing();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.confirm").data(),
                             { 100 * dpiScale, 0 }) ) {
                        m_audioTableSortCacheDirty = true;
                        engine.pushCommand(
                            Logic::CmdRemoveAudioResource{ m_manageTrackId });
                        m_manageTrackId = "";
                    }
                    ImGui::SameLine();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { 100 * dpiScale, 0 }) ) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}
}  // namespace MMM::UI
