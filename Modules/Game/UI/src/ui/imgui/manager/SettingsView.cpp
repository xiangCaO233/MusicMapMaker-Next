#include "ui/imgui/manager/SettingsView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <cmath>

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
    }
    return "";
}

/// @brief 使用指定字体测量单行文本宽度。
float measureSettingsText(const char* text, const char* fontName)
{
    if ( !text ) return 0.0f;

    auto&   skinCfg = Config::SkinManager::instance();
    ImFont* font    = skinCfg.getFont(fontName);
    if ( !font ) {
        font = ImGui::GetFont();
    }
    return font
        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text, nullptr)
        .x;
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
                              const char*                       fontName)
{
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        maxWidth = std::max(maxWidth, measureSettingsText(label, fontName));
    }
    return maxWidth;
}

/// @brief 估算当前设置页标签列中的最大宽度。
float measureSettingsTabLabelWidth(Event::SettingsTab tab)
{
    switch ( tab ) {
    case Event::SettingsTab::Software: {
        const std::array<const char*, 27> labels{
            TR_CACHE("ui.settings.software.language").data(),
            TR_CACHE("ui.settings.software.framelimit").data(),
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
            TR_CACHE("ui.settings.software.picker_style").data(),
            TR_CACHE("ui.settings.software.save_format").data(),
            TR_CACHE("ui.settings.software.time_format").data(),
            TR_CACHE("ui.settings.software.recent_limit").data(),
            TR_CACHE("ui.settings.software.sync_mode").data(),
            TR_CACHE("ui.settings.software.sync_factor").data(),
            TR_CACHE("ui.settings.software.sync_buffer").data(),
            TR_CACHE("ui.settings.software.sync_interval").data()
        };
        return measureSettingsTextList(labels, "content");
    }
    case Event::SettingsTab::Visual: {
        const std::array<const char*, 28> labels{
            TR_CACHE("ui.settings.visual.layout_left").data(),
            TR_CACHE("ui.settings.visual.layout_top").data(),
            TR_CACHE("ui.settings.visual.layout_right").data(),
            TR_CACHE("ui.settings.visual.layout_bottom").data(),
            TR_CACHE("ui.settings.visual.layout_box_width").data(),
            TR_CACHE("ui.settings.visual.judgeline_pos").data(),
            TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            TR_CACHE("ui.settings.visual.note_scale_x").data(),
            TR_CACHE("ui.settings.visual.note_scale_y").data(),
            TR_CACHE("ui.settings.visual.note_fill_mode").data(),
            TR_CACHE("ui.settings.visual.debug_draw_hitboxes").data(),
            TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            TR_CACHE("ui.settings.visual.bg_darken").data(),
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
            TR_CACHE("ui.settings.visual.linear_scroll").data(),
            TR_CACHE("ui.settings.visual.snap_threshold").data(),
            TR_CACHE("ui.settings.visual.spectrum_detail").data(),
            TR_CACHE("ui.settings.visual.visual_offset").data()
        };
        return measureSettingsTextList(labels, "content");
    }
    case Event::SettingsTab::Project: {
        const std::array<const char*, 3> labels{
            TR_CACHE("ui.settings.project.info").data(),
            TR_CACHE("ui.settings.project.path").data(),
            TR_CACHE("ui.settings.project.no_project").data()
        };
        return measureSettingsTextList(labels, "content");
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
        return measureSettingsTextList(labels, "content");
    }
    case Event::SettingsTab::Editor: {
        const std::array<const char*, 14> labels{
            TR_CACHE("ui.settings.editor.reverse_scroll").data(),
            TR_CACHE("ui.settings.editor.scroll_snap").data(),
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            TR_CACHE("ui.settings.editor.remove_objects_on_polyline_path")
                .data(),
            TR_CACHE("ui.settings.editor.select_pasted_objects").data(),
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            TR_CACHE("ui.settings.editor.selection").data(),
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            TR_CACHE("ui.settings.editor.selection.rounding").data(),
            TR_CACHE("ui.settings.editor.sfx_strategy").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data()
        };
        return measureSettingsTextList(labels, "content");
    }
    }
    return 0.0f;
}

/// @brief 估算当前设置页右侧控件中不可再换行内容的宽度。
float measureSettingsTabWidgetWidth(Event::SettingsTab tab, float dpiScale)
{
    const float scale      = std::max(1.0f, dpiScale);
    const float framePad   = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float comboArrow = ImGui::GetFrameHeight();
    float       minWidth = measureSettingsText("0.0000", "content") + framePad +
                           std::floor(48.0f * scale);

    auto addOptions = [&](auto&& labels) {
        minWidth = std::max(
            minWidth,
            measureSettingsTextList(labels, "content") + framePad + comboArrow);
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
    case Event::SettingsTab::Project: break;
    }

    return std::ceil(minWidth);
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

/// @brief 计算设置分类侧栏中不可再换行文本所需的宽度。
float SettingsView::getCategorySidebarWidth(float dpiScale) const
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    const float          scale   = std::max(1.0f, dpiScale);
    const float          sidebarBaseW =
        parseLayoutFloat(skinCfg.getLayoutConfig("side_bar.width"), 40.0f);
    const float btnSize = std::floor(sidebarBaseW * scale);

    const std::array<const char*, 5> labels{
        getCategoryShortLabel(Event::SettingsTab::Software),
        getCategoryShortLabel(Event::SettingsTab::Visual),
        getCategoryShortLabel(Event::SettingsTab::Project),
        getCategoryShortLabel(Event::SettingsTab::Beatmap),
        getCategoryShortLabel(Event::SettingsTab::Editor)
    };
    const float maxLabelWidth = measureSettingsTextList(labels, "menu");
    const float sepAreaW      = std::floor(12.0f * scale);
    const float labelPadding  = std::floor(12.0f * scale);
    const float vboxPadding   = std::floor(12.0f * scale);

    return std::floor(btnSize + sepAreaW + maxLabelWidth + labelPadding +
                      vboxPadding);
}

/// @brief 计算设置窗口当前内容所需的最小整窗尺寸。
ImVec2 SettingsView::getMinWindowSize(float dpiScale) const
{
    const float scale = std::max(1.0f, dpiScale);
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float windowPad = std::floor(aesthetics.windowPadding * scale) * 2.0f;
    const float sidebarWidth = getCategorySidebarWidth(scale);
    const float categorySize = std::floor(
        parseLayoutFloat(
            Config::SkinManager::instance().getLayoutConfig("side_bar.width"),
            40.0f) *
        scale);
    const float categorySpacing = std::floor(aesthetics.itemSpacing * scale);
    const float categoryHeight  = std::floor(8.0f * scale) * 2.0f +
                                  categorySize * 5.0f + categorySpacing * 4.0f;

    const float labelWidth =
        measureSettingsTabLabelWidth(m_currentTab) + std::floor(16.0f * scale);
    const float widgetWidth =
        measureSettingsTabWidgetWidth(m_currentTab, scale);
    const float contentDecorations = std::floor(15.0f * scale) * 2.0f +
                                     std::floor(8.0f * scale) * 4.0f +
                                     std::floor(8.0f * scale);
    const float contentWidth = labelWidth + widgetWidth + contentDecorations;
    const float titleHeight  = ImGui::GetFrameHeightWithSpacing();

    return ImVec2(std::ceil(windowPad + sidebarWidth + 1.0f + contentWidth),
                  std::ceil(windowPad + titleHeight + categoryHeight));
}

/// @brief 打开设置窗口并切换到指定设置页。
/// @param tab 需要激活的设置页。
void SettingsView::open(Event::SettingsTab tab)
{
    m_currentTab            = tab;
    m_isOpen                = true;
    m_focusNextFrame        = true;
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
void SettingsView::requestDockToCenter()
{
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧聚焦设置窗口。
void SettingsView::requestFocus()
{
    m_focusNextFrame = true;
}

/// @brief 更新并渲染独立设置窗口。
/// @param sourceManager 当前 UI 管理器。
void SettingsView::update(UIManager* sourceManager)
{
    (void)sourceManager;

    std::string windowName =
        std::string(TR("title.settings_manager").data()) + "###SettingsWindow";

    ImGuiID dockId = 0;
    if ( m_dockToCenterNextFrame ) {
        dockId = MainDockSpaceUI::getCenterDockId();
    }
    if ( m_focusNextFrame ) {
        ImGui::SetNextWindowFocus();
        m_focusNextFrame = false;
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
            if ( ImGui::Button(btnId.c_str(), { rect.width, rect.height }) ) {
                m_currentTab = tab;
            }

            float iconAreaW = btnSize;
            float sepX      = rect.x + iconAreaW;

            ImFont* iconFont = skinCfg.getFont("pure_icons");
            if ( !iconFont ) {
                iconFont = skinCfg.getFont("setting_internal");
            }
            if ( iconFont ) ImGui::PushFont(iconFont);
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
                ImGui::PushFont(menuFont);
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
            if ( contentFont ) ImGui::PushFont(contentFont);

            switch ( m_currentTab ) {
            case Event::SettingsTab::Software: drawSoftwareSettings(); break;
            case Event::SettingsTab::Visual: drawVisualSettings(); break;
            case Event::SettingsTab::Project: drawProjectSettings(); break;
            case Event::SettingsTab::Beatmap: drawBeatmapSettings(); break;
            case Event::SettingsTab::Editor: drawEditorSettings(); break;
            }

            ImGui::Dummy(ImVec2(0, 50));
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
}

}  // namespace MMM::UI
