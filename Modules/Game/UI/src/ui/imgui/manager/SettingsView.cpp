#include "ui/imgui/manager/SettingsView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 获取设置分类短标签。
const char* getCategoryShortLabel(Event::SettingsTab tab)
{
    switch ( tab ) {
    case Event::SettingsTab::Software:
        return TR_CACHE("ui.settings.software.short").data();
    case Event::SettingsTab::Visual:
        return TR_CACHE("ui.settings.visual.short").data();
    case Event::SettingsTab::Project:
        return TR_CACHE("ui.settings.project.short").data();
    case Event::SettingsTab::Beatmap:
        return TR_CACHE("ui.settings.beatmap.short").data();
    case Event::SettingsTab::Editor:
        return TR_CACHE("ui.settings.editor.short").data();
    case Event::SettingsTab::Shortcut:
        return TR_CACHE("ui.settings.shortcut.short").data();
    case Event::SettingsTab::Debug:
        return TR_CACHE("ui.settings.debug.short").data();
    }
    return "";
}

/// @brief 使用指定字体测量单行文本宽度。
float measureSettingsText(const char* text, ImFont* font, float fontSize)
{
    if ( !text ) return 0.0f;
    if ( !font ) return 0.0f;

    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 无异常解析皮肤布局中的浮点值。
float parseLayoutFloat(const std::string& value, float fallback)
{
    float parsed = fallback;
    auto  result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if ( result.ec != std::errc() ) {
        return fallback;
    }
    return parsed;
}

/// @brief 测量一组设置文本中的最大单行宽度。
template<size_t N>
float measureSettingsTextList(const std::array<const char*, N>& labels,
                              ImFont* font, float fontSize)
{
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        maxWidth =
            std::max(maxWidth, measureSettingsText(label, font, fontSize));
    }
    return maxWidth;
}

/// @brief 估算当前设置页标签列中的最大宽度。
float measureSettingsTabLabelWidth(Event::SettingsTab     tab,
                                   const UiFrameSnapshot& snapshot)
{
    ImFont* font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    switch ( tab ) {
    case Event::SettingsTab::Software: {
        const std::array<const char*, 30> labels{
            TR_CACHE("ui.settings.software.language").data(),
            TR_CACHE("ui.settings.software.framelimit").data(),
            TR_CACHE("ui.settings.software.auto_upload_pgo_profiles").data(),
            "皮肤",
            TR_CACHE("ui.settings.software.theme").data(),
            TR_CACHE("ui.settings.software.font.ascii").data(),
            TR_CACHE("ui.settings.software.font.cjk").data(),
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            TR_CACHE("ui.settings.software.font.multiplier").data(),
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            TR_CACHE("ui.settings.software.cursor_size").data(),
            TR_CACHE("ui.settings.software.trail_size").data(),
            TR_CACHE("ui.settings.software.trail_life").data(),
            TR_CACHE("ui.settings.software.smoke_size").data(),
            TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
            TR_CACHE("ui.settings.software.smoke_life").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.frame_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_gap").data(),
            TR_CACHE("ui.settings.software.aesthetics.item_spacing").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_padding").data(),
            TR_CACHE("ui.settings.software.aesthetics.animation_transition")
                .data(),
            TR_CACHE("ui.settings.software.picker_style").data(),
            TR_CACHE("ui.settings.software.save_format").data(),
            TR_CACHE("ui.settings.software.time_format").data(),
            TR_CACHE("ui.settings.software.recent_limit").data(),
            TR_CACHE("ui.settings.software.sync_mode").data(),
            TR_CACHE("ui.settings.software.sync_factor").data(),
            TR_CACHE("ui.settings.software.sync_buffer").data(),
            TR_CACHE("ui.settings.software.sync_interval").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Visual: {
        const std::array<const char*, 31> labels{
            TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            TR_CACHE("ui.settings.visual.note_scale_x").data(),
            TR_CACHE("ui.settings.visual.note_scale_y").data(),
            TR_CACHE("ui.settings.visual.note_fill_mode").data(),
            TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            TR_CACHE("ui.settings.visual.bg_darken").data(),
            TR_CACHE("ui.settings.visual.background_spectrum.enabled").data(),
            TR_CACHE("ui.settings.visual.background_spectrum.band_count")
                .data(),
            TR_CACHE("ui.settings.visual.background_spectrum.width_ratio")
                .data(),
            TR_CACHE("ui.settings.visual.background_spectrum.height_ratio")
                .data(),
            TR_CACHE("ui.settings.visual.background_spectrum.baseline_ratio")
                .data(),
            TR_CACHE("ui.settings.visual.background_spectrum.opacity").data(),
            TR_CACHE(
                "ui.settings.visual.background_spectrum.include_hit_effects")
                .data(),
            TR_CACHE("ui.settings.visual.preview_ratio").data(),
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            TR_CACHE("ui.settings.visual.scroll_animation_duration").data(),
            TR_CACHE("ui.settings.visual.linear_scroll").data(),
            TR_CACHE("ui.settings.visual.snap_threshold").data(),
            TR_CACHE("ui.settings.visual.spectrum_detail").data(),
            TR_CACHE("ui.settings.visual.visual_offset").data(),
            TR_CACHE("ui.settings.visual.waveform_visual_offset").data(),
            TR_CACHE("ui.settings.visual.spectrum_visual_offset").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Project: {
        const std::array<const char*, 3> labels{
            TR_CACHE("ui.settings.project.info").data(),
            TR_CACHE("ui.settings.project.path").data(),
            TR_CACHE("ui.settings.project.no_project").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Beatmap: {
        const std::array<const char*, 17> labels{
            TR_CACHE("ui.settings.beatmap.name").data(),
            TR_CACHE("ui.settings.beatmap.title").data(),
            TR_CACHE("ui.settings.beatmap.title_unicode").data(),
            TR_CACHE("ui.settings.beatmap.artist").data(),
            TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
            TR_CACHE("ui.settings.beatmap.mapper").data(),
            TR_CACHE("ui.settings.beatmap.version").data(),
            TR_CACHE("ui.settings.beatmap.path").data(),
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            TR_CACHE("ui.settings.beatmap.video_start").data(),
            TR_CACHE("ui.settings.beatmap.bg_offset").data(),
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            TR_CACHE("ui.settings.beatmap.length").data(),
            TR_CACHE("ui.settings.beatmap.audio").data(),
            TR_CACHE("ui.settings.beatmap.cover").data(),
            TR_CACHE("ui.settings.beatmap.background").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Editor: {
        const std::array<const char*, 17> labels{
            TR_CACHE("ui.settings.editor.reverse_scroll").data(),
            TR_CACHE("ui.settings.editor.scroll_snap").data(),
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            TR_CACHE("ui.settings.editor.remove_objects_on_polyline_path")
                .data(),
            TR_CACHE("ui.settings.editor.select_pasted_objects").data(),
            TR_CACHE("ui.settings.editor.copy_paste_time_basis").data(),
            TR_CACHE("ui.settings.editor.timeline_selection_includes_bpm")
                .data(),
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            TR_CACHE("ui.settings.editor.selection").data(),
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            TR_CACHE("ui.settings.editor.selection.rounding").data(),
            TR_CACHE("ui.settings.editor.sfx_strategy").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
            TR_CACHE("ui.settings.editor.sfx_stereo_hit_effects").data(),
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Shortcut: {
        const std::array<const char*, 17> labels{
            TR_CACHE("ui.settings.shortcut.tool_move").data(),
            TR_CACHE("ui.settings.shortcut.tool_marquee").data(),
            TR_CACHE("ui.settings.shortcut.tool_draw").data(),
            TR_CACHE("ui.settings.shortcut.tool_color_brush").data(),
            TR_CACHE("ui.settings.shortcut.tool_color_eraser").data(),
            TR_CACHE("ui.settings.shortcut.mirror").data(),
            TR_CACHE("ui.settings.shortcut.mirror_paste").data(),
            TR_CACHE("ui.settings.shortcut.delete_selected").data(),
            TR_CACHE("ui.settings.shortcut.toggle_reverse_scroll").data(),
            TR_CACHE("ui.settings.shortcut.toggle_scroll_snap").data(),
            TR_CACHE("ui.settings.shortcut.toggle_snap_floor").data(),
            TR_CACHE("ui.settings.shortcut.toggle_scroll_timing_mapping")
                .data(),
            TR_CACHE("ui.settings.shortcut.toggle_beat_lines").data(),
            TR_CACHE("ui.settings.shortcut.toggle_stop_playback_on_scroll")
                .data(),
            TR_CACHE("ui.settings.shortcut.toggle_hit_sfx").data(),
            TR_CACHE("ui.settings.shortcut.toggle_hit_effects").data(),
            TR_CACHE("ui.settings.shortcut.toggle_sync_same_main_audio").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Debug: {
        const std::array<const char*, 2> labels{
            TR_CACHE("ui.settings.debug.draw_hitboxes").data(),
            TR_CACHE("ui.settings.debug.render_profile_logging").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    }
    return 0.0f;
}

/// @brief 估算当前设置页右侧控件中不可再换行内容的宽度。
float measureSettingsTabWidgetWidth(Event::SettingsTab     tab,
                                    const UiFrameSnapshot& snapshot)
{
    const float scale      = std::max(1.0f, snapshot.dpiScale);
    const float framePad   = snapshot.framePadding.x * 2.0f;
    const float comboArrow = snapshot.frameHeight;
    ImFont*     font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    float minWidth = measureSettingsText("0.0000", font, snapshot.fontSize) +
                     framePad + std::floor(48.0f * scale);

    auto addOptions = [&](auto&& labels) {
        minWidth =
            std::max(minWidth,
                     measureSettingsTextList(labels, font, snapshot.fontSize) +
                         framePad + comboArrow);
    };

    switch ( tab ) {
    case Event::SettingsTab::Software: {
        addOptions(std::array<const char*, 2>{ "简体中文 (zh_cn)",
                                               "English (en_us)" });
        addOptions(std::array<const char*, 6>{
            TR_CACHE("ui.settings.software.framelimit.vsync").data(),
            TR_CACHE("ui.settings.software.framelimit.2x").data(),
            TR_CACHE("ui.settings.software.framelimit.4x").data(),
            TR_CACHE("ui.settings.software.framelimit.8x").data(),
            TR_CACHE("ui.settings.software.framelimit.unlimited").data(),
            TR_CACHE("ui.settings.software.font.default").data() });
        break;
    }
    case Event::SettingsTab::Visual: {
        addOptions(std::array<const char*, 4>{
            TR_CACHE("ui.settings.visual.fill_mode.stretch").data(),
            TR_CACHE("ui.settings.visual.fill_mode.aspect_fit").data(),
            TR_CACHE("ui.settings.visual.fill_mode.aspect_fill").data(),
            TR_CACHE("ui.settings.visual.fill_mode.center").data() });
        break;
    }
    case Event::SettingsTab::Beatmap: {
        addOptions(std::array<const char*, 2>{
            TR_CACHE("ui.settings.beatmap.cover_type.image").data(),
            TR_CACHE("ui.settings.beatmap.cover_type.video").data() });
        break;
    }
    case Event::SettingsTab::Editor: {
        addOptions(std::array<const char*, 2>{
            TR_CACHE("ui.settings.editor.selection.strict").data(),
            TR_CACHE("ui.settings.editor.selection.intersection").data() });
        break;
    }
    case Event::SettingsTab::Shortcut: {
        const float shortcutWidth =
            measureSettingsText(
                "Ctrl+Shift+RightArrow", font, snapshot.fontSize) +
            measureSettingsText(TR_CACHE("ui.settings.shortcut.record").data(),
                                font,
                                snapshot.fontSize) +
            measureSettingsText(TR_CACHE("ui.settings.shortcut.clear").data(),
                                font,
                                snapshot.fontSize) +
            framePad * 3.0f + std::floor(32.0f * scale);
        minWidth = std::max(minWidth, shortcutWidth);
        break;
    }
    case Event::SettingsTab::Project: break;
    case Event::SettingsTab::Debug: break;
    }

    return std::ceil(minWidth);
}

/// @brief 捕获设置窗口同步测量所需的当前帧快照。
/// @param dpiScale 当前窗口内容缩放。
/// @return 设置窗口布局测量快照。
UiFrameSnapshot captureSettingsUiFrameSnapshot(float dpiScale)
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
    snapshot.sidebarWidthConfig     = skinCfg.getLayoutConfig("side_bar.width");
    return snapshot;
}
}  // namespace

/// @brief 构造设置面板视图并订阅设置页切换事件。
/// @param viewName 视图名称。
SettingsView::SettingsView(const std::string& viewName) : IUIView(viewName)
{
    m_tabSubId =
        Event::EventBus::instance().subscribe<Event::UISettingsTabEvent>(
            [this](const Event::UISettingsTabEvent& e) { open(e.tab); });
}

/// @brief 析构设置面板视图并取消设置页切换事件订阅。
SettingsView::~SettingsView()
{
    if ( m_tabSubId != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UISettingsTabEvent>(
            m_tabSubId);
    }
}

/// @brief 获取或创建指定索引的设置项行布局。
/// @param index 行布局缓存索引。
/// @return 已清空并可复用的横向行布局。
CLayHBox& SettingsView::getRow(size_t index)
{
    if ( index >= m_settingRows.size() ) {
        m_settingRows.emplace_back();
    }
    m_settingRows[index].clear();
    return m_settingRows[index];
}

/// @brief 获取或创建指定索引的设置段落布局。
/// @param index 段落布局缓存索引。
/// @return 已清空并可复用的纵向段落布局。
CLayVBox& SettingsView::getSection(size_t index)
{
    if ( index >= m_sectionBoxes.size() ) {
        m_sectionBoxes.emplace_back();
    }
    m_sectionBoxes[index].clear();
    return m_sectionBoxes[index];
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param tab 当前设置页。
/// @return 完全匹配时返回 true。
bool SettingsView::layoutMetricsMatch(const LayoutMetricsCache& cache,
                                      const UiFrameSnapshot&    snapshot,
                                      Event::SettingsTab        tab)
{
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };

    return cache.valid && cache.tab == tab &&
           floatEqual(cache.dpiScale, snapshot.dpiScale) &&
           cache.language == snapshot.language &&
           cache.preferredAsciiFont == snapshot.preferredAsciiFont &&
           cache.preferredCjkFont == snapshot.preferredCjkFont &&
           floatEqual(cache.fontSizeMultiplier, snapshot.fontSizeMultiplier) &&
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier) &&
           floatEqual(cache.windowPadding, snapshot.windowPadding) &&
           floatEqual(cache.itemSpacing, snapshot.itemSpacing) &&
           cache.sidebarWidthConfig == snapshot.sidebarWidthConfig &&
           cache.translationVersion == snapshot.translationVersion;
}

/// @brief 构造设置窗口布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param tab 需要测量的设置页。
/// @return 设置窗口布局测量结果。
SettingsView::LayoutMetricsCache SettingsView::buildLayoutMetrics(
    const UiFrameSnapshot& snapshot, Event::SettingsTab tab)
{
    LayoutMetricsCache cache;
    cache.valid              = true;
    cache.tab                = tab;
    cache.dpiScale           = snapshot.dpiScale;
    cache.language           = snapshot.language;
    cache.preferredAsciiFont = snapshot.preferredAsciiFont;
    cache.preferredCjkFont   = snapshot.preferredCjkFont;
    cache.fontSizeMultiplier = snapshot.fontSizeMultiplier;
    cache.uiScaleMultiplier  = snapshot.uiScaleMultiplier;
    cache.windowPadding      = snapshot.windowPadding;
    cache.itemSpacing        = snapshot.itemSpacing;
    cache.sidebarWidthConfig = snapshot.sidebarWidthConfig;
    cache.translationVersion = snapshot.translationVersion;

    const float scale = std::max(1.0f, snapshot.dpiScale);
    ImFont*     menuFont =
        snapshot.menuFont ? snapshot.menuFont : snapshot.fallbackFont;
    const float sidebarBaseW =
        parseLayoutFloat(cache.sidebarWidthConfig, 40.0f);
    const float btnSize = std::floor(sidebarBaseW * scale);

    const std::array<const char*, 7> labels{
        getCategoryShortLabel(Event::SettingsTab::Software),
        getCategoryShortLabel(Event::SettingsTab::Visual),
        getCategoryShortLabel(Event::SettingsTab::Project),
        getCategoryShortLabel(Event::SettingsTab::Beatmap),
        getCategoryShortLabel(Event::SettingsTab::Editor),
        getCategoryShortLabel(Event::SettingsTab::Shortcut),
        getCategoryShortLabel(Event::SettingsTab::Debug)
    };
    const float maxLabelWidth =
        measureSettingsTextList(labels, menuFont, snapshot.fontSize);
    const float sepAreaW       = std::floor(12.0f * scale);
    const float labelPadding   = std::floor(12.0f * scale);
    const float vboxPadding    = std::floor(12.0f * scale);
    cache.categorySidebarWidth = std::floor(btnSize + sepAreaW + maxLabelWidth +
                                            labelPadding + vboxPadding);

    const float windowPad = std::floor(snapshot.windowPadding * scale) * 2.0f;
    const float categorySize    = std::floor(sidebarBaseW * scale);
    const float categorySpacing = std::floor(snapshot.itemSpacing * scale);
    const float categoryHeight  = std::floor(8.0f * scale) * 2.0f +
                                  categorySize * 7.0f + categorySpacing * 6.0f;

    cache.tabLabelWidth =
        measureSettingsTabLabelWidth(tab, snapshot) + std::floor(16.0f * scale);
    cache.tabWidgetWidth = measureSettingsTabWidgetWidth(tab, snapshot);
    const float contentDecorations = std::floor(15.0f * scale) * 2.0f +
                                     std::floor(8.0f * scale) * 4.0f +
                                     std::floor(8.0f * scale);
    const float contentWidth =
        cache.tabLabelWidth + cache.tabWidgetWidth + contentDecorations;
    const float titleHeight = snapshot.frameHeightWithSpacing;

    cache.minWindowSize = ImVec2(
        std::ceil(windowPad + cache.categorySidebarWidth + 1.0f + contentWidth),
        std::ceil(windowPad + titleHeight + categoryHeight));
    return cache;
}

/// @brief 获取当前设置页布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和设置页匹配的布局测量结果。
/// @warning UI 热路径：仅在缓存未命中时同步测量；通常由并行准备提前填充。
const SettingsView::LayoutMetricsCache& SettingsView::getLayoutMetrics(
    float dpiScale) const
{
    UiFrameSnapshot snapshot = captureSettingsUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(m_layoutMetricsCache, snapshot, m_currentTab) ) {
        m_layoutMetricsCache = buildLayoutMetrics(snapshot, m_currentTab);
    }
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧设置窗口是否需要准备布局数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要刷新布局缓存时返回 true。
bool SettingsView::needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const
{
    return m_isOpen &&
           !layoutMetricsMatch(m_layoutMetricsCache, snapshot, m_currentTab);
}

/// @brief 在 UI 主线程准备设置窗口布局数据。
/// @param snapshot 当前帧 UI 快照。
void SettingsView::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    m_preparedLayoutMetricsCache = buildLayoutMetrics(snapshot, m_currentTab);
    m_hasPreparedLayoutMetrics   = true;
}

/// @brief 将准备好的布局数据切换给主线程使用。
void SettingsView::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedLayoutMetrics ) {
        return;
    }

    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 获取当前设置页标签列宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 当前设置页标签列宽度。
float SettingsView::getCurrentTabLabelWidth(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).tabLabelWidth;
}

/// @brief 计算设置分类侧栏中不可再换行文本所需的宽度。
float SettingsView::getCategorySidebarWidth(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).categorySidebarWidth;
}

/// @brief 计算设置窗口当前内容所需的最小整窗尺寸。
ImVec2 SettingsView::getMinWindowSize(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).minWindowSize;
}

/// @brief 打开设置窗口并切换到指定设置页。
/// @param tab 需要激活的设置页。
void SettingsView::open(Event::SettingsTab tab)
{
    m_currentTab                    = tab;
    m_isOpen                        = true;
    m_focusRequestFramesRemaining   = FOCUS_REQUEST_FRAME_COUNT;
    m_dockToCenterNextFrame         = true;
    m_availableSkinDirectoriesDirty = true;
    if ( tab != Event::SettingsTab::Shortcut ) {
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }
}

/// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
void SettingsView::requestDockToCenter()
{
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧聚焦设置窗口。
void SettingsView::requestFocus()
{
    m_focusRequestFramesRemaining = FOCUS_REQUEST_FRAME_COUNT;
}

/// @brief 更新并渲染独立设置窗口。
/// @param sourceManager 当前 UI 管理器。
void SettingsView::update(UIManager* sourceManager)
{
    m_sourceManager = sourceManager;

    std::string windowName =
        std::string(TR("title.settings_manager").data()) + "###SettingsWindow";

    ImGuiID dockId = 0;
    if ( m_dockToCenterNextFrame ) {
        dockId = MainDockSpaceUI::getCenterDockId();
    }
    if ( m_focusRequestFramesRemaining > 0 ) {
        ImGui::SetNextWindowFocus();
        --m_focusRequestFramesRemaining;
    }
    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    ImGui::SetNextWindowSizeConstraints(getMinWindowSize(dpiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));

    LayoutContext layoutContext(m_layoutCtx,
                                windowName,
                                false,
                                ImGuiWindowFlags_None,
                                &m_isOpen,
                                dockId,
                                ImGuiCond_Always);
    (void)layoutContext;
    if ( dockId != 0 ) {
        m_dockToCenterNextFrame = false;
    }

    drawContent();
    if ( !m_isOpen ) {
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }
}

/// @brief 绘制设置视图内容。
void SettingsView::drawContent()
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    float sidebarBaseW =
        parseLayoutFloat(skinCfg.getLayoutConfig("side_bar.width"), 40.0f);
    float btnSize = std::floor(sidebarBaseW * dpiScale);

    float sidebarWidth = getCategorySidebarWidth(dpiScale);
    Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);

    // 1. 左侧图标侧边栏 (Clay 布局)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("SettingsCategories", { sidebarWidth, 0 }, false);
    {
        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float rounding = std::floor(aesthetics.frameRounding * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        Utils::pushFixedButtonStyleVars();

        auto DrawCategoryButton = [&](Event::SettingsTab tab,
                                      const char*        iconStr,
                                      const char*        tooltip,
                                      Clay_BoundingBox   rect) {
            ImGui::SetCursorScreenPos({ rect.x, rect.y });

            bool isActive = (m_currentTab == tab);
            if ( isActive ) {
                ImVec4 activeCol =
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
            } else {
                Utils::UIThemeUtils::pushTransparentButtonStyles();
            }

            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if ( !isActive ) iconVec4.w *= 0.7f;
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            std::string btnId = "##setting_tab_" + std::to_string((int)tab);
            if ( ::MMM::UI::FeedbackButton(btnId.c_str(),
                                           { rect.width, rect.height }) ) {
                m_currentTab = tab;
                if ( tab != Event::SettingsTab::Shortcut ) {
                    m_recordingShortcutTarget = ShortcutRecordTarget::None;
                    ShortcutUtils::setShortcutRecordingActive(false);
                }
            }

            float iconAreaW = btnSize;
            float sepX      = rect.x + iconAreaW;

            ImFont* iconFont = skinCfg.getFont("pure_icons");
            if ( !iconFont ) {
                iconFont = skinCfg.getFont("setting_internal");
            }
            if ( iconFont ) ImGui::PushFont(iconFont, iconFont->LegacySize);
            ImFont* drawIconFont = iconFont ? iconFont : ImGui::GetFont();
            ImVec2  iconSize     = ImGui::CalcTextSize(iconStr);
            ImVec2  iconPos = { rect.x + (iconAreaW - iconSize.x) * 0.5f,
                                rect.y + (rect.height - iconSize.y) * 0.5f };
            ImGui::GetWindowDrawList()->AddText(
                drawIconFont,
                ImGui::GetFontSize(),
                iconPos,
                ImGui::GetColorU32(ImGuiCol_Text),
                iconStr);
            if ( iconFont ) ImGui::PopFont();

            ImVec4 sepCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            sepCol.w *= 0.3f;
            ImGui::GetWindowDrawList()->AddLine(
                { sepX, rect.y + rect.height * 0.25f },
                { sepX, rect.y + rect.height * 0.75f },
                ImGui::GetColorU32(sepCol),
                std::floor(1.0f * dpiScale));

            std::string label    = getCategoryShortLabel(tab);
            ImFont*     menuFont = skinCfg.getFont("menu");
            if ( menuFont ) {
                ImGui::PushFont(menuFont, menuFont->LegacySize);
                ImVec2 labelSize       = ImGui::CalcTextSize(label.c_str());
                float  textLeftPadding = std::floor(8.0f * dpiScale);
                ImVec2 labelPos = { sepX + textLeftPadding,
                                    rect.y +
                                        (rect.height - labelSize.y) * 0.5f };
                ImGui::GetWindowDrawList()->AddText(
                    menuFont,
                    ImGui::GetFontSize(),
                    labelPos,
                    ImGui::GetColorU32(ImGuiCol_Text),
                    label.c_str());
                ImGui::PopFont();
            }

            Utils::renderTooltip(tooltip, Utils::TooltipDir::Right);

            ImGui::PopStyleColor(1);
            if ( isActive )
                ImGui::PopStyleColor(3);
            else
                Utils::UIThemeUtils::popTransparentButtonStyles();
        };

        CLayVBox vbox;
        vbox.setPadding(std::floor(6.0f * dpiScale),
                        std::floor(6.0f * dpiScale),
                        std::floor(8.0f * dpiScale),
                        std::floor(8.0f * dpiScale))
            .setSpacing(std::floor(aesthetics.itemSpacing * dpiScale));

        vbox.addElement("SoftwareTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Software,
                                ICON_MMM_DESKTOP,
                                TR_CACHE("ui.settings.software").data(),
                                rect);
                        });

        vbox.addElement("VisualTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Visual,
                                ICON_MMM_EYE,
                                TR_CACHE("ui.settings.visual").data(),
                                rect);
                        });

        vbox.addElement("ProjectTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Project,
                                ICON_MMM_FOLDER,
                                TR_CACHE("ui.settings.project").data(),
                                rect);
                        });

        vbox.addElement("BeatmapTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Beatmap,
                                ICON_MMM_FILE,
                                TR_CACHE("ui.settings.beatmap").data(),
                                rect);
                        });

        vbox.addElement("EditorTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Editor,
                                ICON_MMM_PEN,
                                TR_CACHE("ui.settings.editor").data(),
                                rect);
                        });

        vbox.addElement("ShortcutTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Shortcut,
                                ICON_MMM_KEYBOARD,
                                TR_CACHE("ui.settings.shortcut").data(),
                                rect);
                        });

        vbox.addElement("DebugTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Debug,
                                ICON_MMM_BUG,
                                TR_CACHE("ui.settings.debug").data(),
                                rect);
                        });

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        vbox.renderInCurrent(
            startPos, { sidebarWidth, ImGui::GetContentRegionAvail().y });

        Utils::popFixedButtonStyleVars();
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);  // WindowPadding, ChildRounding

    ImGui::SameLine(0, 0);

    // 2. 中间分割线
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  h = ImGui::GetContentRegionAvail().y;
        ImGui::GetWindowDrawList()->AddLine(
            { p.x, p.y }, { p.x, p.y + h }, IM_COL32(80, 80, 80, 255));
        ImGui::Dummy({ 1.0f, h });
    }

    ImGui::SameLine(0, 0);

    // 3. 右侧内容区 (标准 ImGui，内部调用 Clay)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 25.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 15.0f));

        if ( ImGui::BeginChild("SettingsContent", { 0, 0 }, false) ) {
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) {
                ImGui::PushFont(contentFont, contentFont->LegacySize);
            }

            switch ( m_currentTab ) {
            case Event::SettingsTab::Software: drawSoftwareSettings(); break;
            case Event::SettingsTab::Visual: drawVisualSettings(); break;
            case Event::SettingsTab::Project: drawProjectSettings(); break;
            case Event::SettingsTab::Beatmap: drawBeatmapSettings(); break;
            case Event::SettingsTab::Editor: drawEditorSettings(); break;
            case Event::SettingsTab::Shortcut: drawShortcutSettings(); break;
            case Event::SettingsTab::Debug: drawDebugSettings(); break;
            }

            ImGui::Dummy(ImVec2(0, 50));
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
}

}  // namespace MMM::UI
