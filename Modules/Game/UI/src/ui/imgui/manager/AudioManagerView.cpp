#include "ui/imgui/manager/AudioManagerView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <nfd.h>
#include <utility>

namespace MMM::UI
{
namespace
{
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
    input.showPermanentSFX   = m_showPermanentSFX;
    input.showMainTracks     = m_showMainTracks;
    input.showProjectSFX     = m_showProjectSFX;

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
        cache.input.showGlobalSettings == input.showGlobalSettings &&
        cache.input.showPermanentSFX == input.showPermanentSFX &&
        cache.input.showMainTracks == input.showMainTracks &&
        cache.input.showProjectSFX == input.showProjectSFX;

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
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier);
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

    const float scale       = std::max(1.0f, snapshot.dpiScale);
    const float frameH      = snapshot.frameHeight;
    const float frameWithSp = snapshot.frameHeightWithSpacing;
    const float itemSpacing = snapshot.itemSpacing;
    const float rowSpacingY = 4.0f;
    const float labelPad    = std::floor(12.0f * scale);
    const float footerPadX  = std::floor(16.0f * scale) * 2.0f;
    const float muteButtonW = std::floor(32.0f * scale);
    const float rootPadX    = std::floor(12.0f * scale) * 2.0f;
    const float rootPadY    = std::floor(12.0f * scale) * 2.0f;
    ImFont*     font        = snapshot.fileManagerFont
                                  ? snapshot.fileManagerFont
                                  : (snapshot.contentFont ? snapshot.contentFont
                                                          : snapshot.fallbackFont);

    const std::array<const char*, 3> controlLabels{
        TR("ui.audio_manager.global_volume").data(),
        TR("ui.audio_manager.bgm_gain").data(),
        TR("ui.audio_manager.sfx_gain").data()
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
        TR("ui.audio_manager.global_settings").data(),
        TR("ui.audio_manager.permanent_sfx").data(),
        TR("ui.audio_manager.audio_tracks").data(),
        TR("ui.audio_manager.project_sfx").data()
    };

    float headerWidth = 0.0f;
    for ( const char* header : headers ) {
        headerWidth = std::max(
            headerWidth,
            frameH + itemSpacing +
                measureAudioManagerText(header, font, snapshot.fontSize));
    }

    const float controlRowWidth = footerPadX + labelWidth + itemSpacing +
                                  muteButtonW + itemSpacing + sliderMinW;
    float minWidth =
        std::ceil(rootPadX + std::max({ controlRowWidth, headerWidth }));

    float  listHeight = 0.0f;
    size_t listRows   = 0;
    if ( input.permanentSfxCount > 0 ) {
        addAudioManagerListRow(listHeight, listRows, frameH, rowSpacingY);
        if ( input.showPermanentSFX ) {
            for ( size_t i = 0; i < input.permanentSfxCount; ++i ) {
                addAudioManagerListRow(
                    listHeight, listRows, 28.0f * scale, rowSpacingY);
            }
        }
    }

    if ( input.hasProject ) {
        addAudioManagerListRow(listHeight, listRows, frameH, rowSpacingY);
        if ( input.showMainTracks ) {
            for ( size_t i = 0; i < input.mainTrackCount; ++i ) {
                addAudioManagerListRow(
                    listHeight, listRows, 28.0f * scale, rowSpacingY);
            }
            if ( input.effectTrackCount > 0 ) {
                addAudioManagerListRow(
                    listHeight, listRows, frameH, rowSpacingY);
                if ( input.showProjectSFX ) {
                    for ( size_t i = 0; i < input.effectTrackCount; ++i ) {
                        addAudioManagerListRow(
                            listHeight, listRows, 28.0f * scale, rowSpacingY);
                    }
                }
            }
        }
    } else {
        addAudioManagerListRow(listHeight, listRows, 20.0f, rowSpacingY);
        addAudioManagerListRow(listHeight, listRows, 30.0f, rowSpacingY);
    }

    float footerH = frameWithSp;
    if ( input.showGlobalSettings ) {
        footerH += 3.0f * 32.0f + 3.0f * 2.0f + 16.0f;
    }
    footerH += 32.0f + 8.0f;

    float minHeight      = std::ceil(rootPadY + listHeight + footerH);
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
    float   maxLabelW       = getLayoutMetrics(dpiScale).footerLabelWidth;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    size_t rowIndex     = 0;
    size_t subHBoxIndex = 0;

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
            .setSpacing(8)
            .setAlignment(Alignment::Center());

        // --- A. 左侧容器: (FixW) [ 标签 + 弹簧 ] ---
        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(4).setAlignment(Alignment::Center());

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
        rightBox.setSpacing(8).setAlignment(Alignment::Center());

        // 1. 静音按钮
        rightBox.addElement(
            std::string(id) + "_mute",
            Sizing::Fixed(32),
            Sizing::Fixed(30),
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
                    { r.x, r.y + (r.height - 30) * 0.5f });
                if ( muted ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                Utils::pushFixedButtonStyleVars();
                if ( ImGui::Button((std::string(icon) + "##Btn" + id).c_str(),
                                   ImVec2(32, 30)) ) {
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
            Sizing::Fixed(30),
            [&, id, volume, minVal, maxVal, format, tooltip, onVolumeChange](
                Clay_BoundingBox r, bool isHovered) {
                float frameH = ImGui::GetFrameHeight();
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - frameH) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                float val = volume;
                if ( ImGui::SliderFloat((std::string("##Slider") + id).c_str(),
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
                         Sizing::Fixed(32));
    };

    // 渲染音轨列表项的辅助函数
    auto renderAudioItem = [&](const AudioResource& audio,
                               bool                 isPermanentEffect = false) {
        listVBox.addElement(
            "Audio_" + audio.m_id + "_" + audio.m_path,
            Sizing::Grow(),
            Sizing::Fixed(28 * dpiScale),
            [=, &engine, this](Clay_BoundingBox r, bool isHovered) {
                ImGui::Indent();
                std::string labelStr = audio.m_id + " - " + audio.m_path;
                float       availW   = ImGui::GetContentRegionAvail().x;

                Utils::renderScrollingSelectable(
                    audio.m_id, labelStr, availW, 28 * dpiScale, [&]() {
                        // 点击弹出控制器
                        AudioTrackControllerUI::TrackType type =
                            (audio.m_type == AudioTrackType::Main)
                                ? AudioTrackControllerUI::TrackType::Main
                                : AudioTrackControllerUI::TrackType::Effect;
                        sourceManager->openAudioTrackController(
                            audio.m_id, audio.m_id, type);
                    });

                static int s_audioLogCounter = 0;
                if ( !isPermanentEffect ) {
                    bool hovered = ImGui::IsItemHovered();
                    bool rclicked =
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                    if ( (hovered || rclicked) && s_audioLogCounter < 10 ) {
                        XINFO(
                            "AudioItem [{}]: hovered={} rclicked={} "
                            "isPermanent={} pos=({},{}) size=({},{}) "
                            "mouse=({},{})",
                            audio.m_id,
                            hovered,
                            rclicked,
                            isPermanentEffect,
                            r.x,
                            r.y,
                            r.width,
                            r.height,
                            ImGui::GetMousePos().x,
                            ImGui::GetMousePos().y);
                        s_audioLogCounter++;
                    }
                    if ( hovered && rclicked ) {
                        XINFO("AudioItem RIGHT-CLICK TRIGGERED: {}",
                              audio.m_id);
                        m_manageTrackId   = audio.m_id;
                        m_manageTrackType = audio.m_type;
                        m_openManageModal = true;
                    }
                }

                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s (Type: %s)",
                                      audio.m_path.c_str(),
                                      audio.m_type == AudioTrackType::Main
                                          ? "Main"
                                          : "Effect");
                }
                ImGui::Unindent();
            });
    };

    // 1. 特效音轨常驻显示 (皮肤音效)
    auto& skinData = Config::SkinManager::instance().getData();
    if ( !skinData.audioPaths.empty() ) {
        listVBox.addElement("PermanentSFXHeader",
                            Sizing::Grow(),
                            Sizing::Fixed(ImGui::GetFrameHeight()),
                            [&](Clay_BoundingBox r, bool isHovered) {
                                Utils::renderCollapsingHeader(
                                    TR("ui.audio_manager.permanent_sfx").data(),
                                    &m_showPermanentSFX,
                                    r);
                            });

        if ( m_showPermanentSFX ) {
            for ( const auto& [key, path] : skinData.audioPaths ) {
                AudioResource res;
                res.m_id            = key;
                res.m_path          = Config::pathToUtf8(path);
                res.m_type          = AudioTrackType::Effect;
                res.m_config.volume = audioManager.getSFXPoolVolume(key);
                res.m_config.muted  = audioManager.getSFXPoolMute(key);

                renderAudioItem(res, true);
            }
        }
    }

    if ( project ) {
        // 2. 显示主音轨列表
        listVBox.addElement("AudioTracksHeader",
                            Sizing::Grow(),
                            Sizing::Fixed(ImGui::GetFrameHeight()),
                            [&](Clay_BoundingBox r, bool isHovered) {
                                Utils::renderCollapsingHeader(
                                    TR("ui.audio_manager.audio_tracks").data(),
                                    &m_showMainTracks,
                                    r);
                            });

        if ( m_showMainTracks ) {
            std::vector<AudioResource> mainTracks;
            std::vector<AudioResource> effectTracks;
            for ( const auto& audio : project->m_audioResources ) {
                if ( audio.m_type == AudioTrackType::Main ) {
                    mainTracks.push_back(audio);
                } else {
                    effectTracks.push_back(audio);
                }
            }

            for ( const auto& audio : mainTracks ) {
                renderAudioItem(audio);
            }

            // 3. 显示项目特效音轨
            if ( !effectTracks.empty() ) {
                listVBox.addElement(
                    "ProjectSFXHeader",
                    Sizing::Grow(),
                    Sizing::Fixed(ImGui::GetFrameHeight()),
                    [&](Clay_BoundingBox r, bool isHovered) {
                        Utils::renderCollapsingHeader(
                            TR("ui.audio_manager.project_sfx").data(),
                            &m_showProjectSFX,
                            r);
                    });

                if ( m_showProjectSFX ) {
                    for ( const auto& audio : effectTracks ) {
                        renderAudioItem(audio);
                    }
                }
            }
        }
    } else {
        // 未打开项目时的提示
        listVBox.addElement("InitialHintSpacer",
                            Sizing::Grow(),
                            Sizing::Fixed(20),
                            [](Clay_BoundingBox, bool) {});
        listVBox.addElement("InitialHint",
                            Sizing::Grow(),
                            Sizing::Fixed(30),
                            [=](Clay_BoundingBox r, bool isHovered) {
                                ImGui::Indent();
                                ImGui::TextDisabled(
                                    "%s",
                                    TR("ui.audio_manager.initial_hint").data());
                                ImGui::Unindent();
                            });
    }

    // 底部全局控制 - 始终显示
    CLayVBox footerVBox;
    // 使用对称的水平内边距，移除手动 Indent，确保左右居中对齐
    footerVBox.setPadding(16, 16, 0, 0).setSpacing(2);

    footerVBox.addElement("FooterHeader",
                          Sizing::Grow(),
                          Sizing::Fixed(ImGui::GetFrameHeight()),
                          [&](Clay_BoundingBox r, bool isHovered) {
                              Utils::renderCollapsingHeader(
                                  TR("ui.audio_manager.global_settings").data(),
                                  &m_showGlobalSettings,
                                  r);
                          });

    if ( m_showGlobalSettings ) {
        footerVBox.addSpring();  // 顶部弹簧，实现垂直居中

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

        footerVBox.addSpring();  // 底部弹簧
    }

    // --- 执行分段渲染 ---
    // 1. 动态计算页脚高度
    float footerH = ImGui::GetFrameHeightWithSpacing();
    if ( m_showGlobalSettings ) {
        // 3个32px的项目 + 间距 + 上下预留的缓冲空间
        footerH += 3 * 32.0f + 3 * 2.0f + 16.0f;
    }
    // 3. 底部导入按钮 (32px + 间距)
    footerH += 32.0f + 8.0f;

    // 2. 渲染顶部列表区域 (自动占据剩余空间)
    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addElement(
            "listContentArea",
            Sizing::Grow(),
            Sizing::Grow(),
            [&listVBox, &layoutContext](Clay_BoundingBox r, bool isHovered) {
                ImGui::BeginChild("AudioListChild",
                                  { r.width, r.height },
                                  false,
                                  ImGuiWindowFlags_None);

                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = { r.width, 10000.0f };

                listVBox.render(layoutContext);

                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            });

    ImVec2 totalSize = rootVBox.renderInCurrent(
        layoutContext.m_startPos,
        { layoutContext.m_avail.x, layoutContext.m_avail.y - footerH });

    // 3. 底部全局控制区域 (独立渲染)
    ImVec2 footerPos = { layoutContext.m_startPos.x,
                         layoutContext.m_startPos.y + totalSize.y };

    float controlH = footerH - (32.0f + 8.0f);
    footerVBox.renderInCurrent(footerPos,
                               { layoutContext.m_avail.x, controlH });

    // 4. 底部加号按钮 (全宽)
    CLayHBox bottomBtnHBox;
    bottomBtnHBox.setPadding(12, 12, 0, 0)
        .setAlignment(Alignment::Center())
        .addElement(
            "Audio_ImportNew",
            Sizing::Grow(),
            Sizing::Fixed(32.0f),
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

                if ( ImGui::Button(
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

    ImVec2 btnPos = { footerPos.x, footerPos.y + controlH + 4.0f };
    bottomBtnHBox.renderInCurrent(btnPos, { layoutContext.m_avail.x, 32.0f });

    // --- 5. 音轨管理窗口 ---
    bool showManageModal = !m_manageTrackId.empty();
    if ( showManageModal ) {
        std::string windowTitle =
            fmt::format("{} {}",
                        TR("ui.audio_manager.manage_title").data(),
                        m_manageTrackId);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if ( m_openManageModal ) {
            ImGui::SetNextWindowSize({ 420 * dpiScale, 0 });
            m_openManageModal = false;
        }
        if ( ImGui::Begin(windowTitle.c_str(),
                          &showManageModal,
                          ImGuiWindowFlags_NoCollapse) ) {
            if ( !showManageModal ) {
                m_manageTrackId = "";
            }

            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox modalLayout;
            float    padding = 16 * dpiScale;
            modalLayout.setPadding(padding, padding, padding, padding);
            modalLayout.setSpacing(12 * dpiScale);

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
            typeRow.setSpacing(8 * dpiScale);
            typeRow.addElement(
                "TypeLabel",
                Sizing::Fixed(100 * dpiScale),
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
                Sizing::Fixed(28 * dpiScale),
                [=, this, &engine](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    int currentType =
                        (m_manageTrackType == AudioTrackType::Main ? 0 : 1);
                    const char* typeNames[] = { "Main", "Effect" };
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::Combo(
                             "##TrackType", &currentType, typeNames, 2) ) {
                        m_manageTrackType =
                            (currentType == 0 ? AudioTrackType::Main
                                              : AudioTrackType::Effect);
                        engine.pushCommand(Logic::CmdUpdateAudioResource{
                            m_manageTrackId, m_manageTrackType });
                        ImGui::CloseCurrentPopup();
                    }
                });
            modalLayout.addLayout("TypeRow",
                                  typeRow,
                                  Sizing::Grow(),
                                  Sizing::Fixed(32 * dpiScale));

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
            btnRow.setSpacing(12 * dpiScale);
            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(140 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(
                             TR("ui.audio_manager.remove_track").data(),
                             { r.width, r.height }) ) {
                        ImGui::OpenPopup("RemoveTrackConfirm");
                    }
                });
            btnRow.addElement(
                "CancelBtn",
                Sizing::Fixed(100 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(TR("ui.common.cancel").data(),
                                       { r.width, r.height }) ) {
                        m_manageTrackId = "";
                    }
                });
            modalLayout.addLayout(
                "BtnRow", btnRow, Sizing::Grow(), Sizing::Fixed(32 * dpiScale));

            // 渲染布局
            // 注意：在模态框中使用 renderInCurrent 来适配 ImGui 的自动大小计算
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // 二次确认弹窗
            {
                static bool wasOpen = false;
                bool        isOpen  = ImGui::IsPopupOpen("RemoveTrackConfirm");
                if ( isOpen && !wasOpen ) {
                    ImGui::SetNextWindowPos(
                        ImGui::GetMainViewport()->GetCenter(),
                        ImGuiCond_Always,
                        ImVec2(0.5f, 0.5f));
                }
                wasOpen = isOpen;
            }
            if ( ImGui::BeginPopupModal(
                     "RemoveTrackConfirm", nullptr, ImGuiWindowFlags_None) ) {
                ImGui::Text("%s", TR("ui.audio_manager.remove_confirm").data());
                ImGui::Spacing();
                if ( ImGui::Button(TR("ui.common.confirm").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    engine.pushCommand(
                        Logic::CmdRemoveAudioResource{ m_manageTrackId });
                    m_manageTrackId = "";
                }
                ImGui::SameLine();
                if ( ImGui::Button(TR("ui.common.cancel").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}
}  // namespace MMM::UI
