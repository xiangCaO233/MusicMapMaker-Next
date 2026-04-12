#include "ui/imgui/manager/SettingsView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::UI
{

SettingsView::SettingsView(const std::string& subViewName)
    : ISubView(subViewName)
{
}

void SettingsView::onUpdate(LayoutContext& layoutContext,
                            UIManager*     sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float sidebarWidth = std::stof(skinCfg.getLayoutConfig("side_bar.width"));

    CLayHBox rootHBox;
    rootHBox.setPadding(0, 0, 0, 0).setSpacing(0);

    // 左侧图标侧边栏 (宽度与主侧边栏一致)
    rootHBox.addElement(
        "SettingsCategoryList",
        Sizing::Fixed(sidebarWidth),
        Sizing::Grow(),
        [this, sidebarWidth, &skinCfg](Clay_BoundingBox r, bool isHovered) {
            ImGui::BeginChild(
                "SettingsCategories", { r.width, r.height }, false);


            // 强制 Flat 风格：无圆角、无边框、无间距
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            auto DrawCategoryIcon = [&](SettingsTab tab,
                                        const char* iconStr,
                                        const char* tooltip) {
                bool isActive = (m_currentTab == tab);

                if ( isActive ) {
                    // 激活态：深灰色背景（VS Code 风格，与 SideBarUI 一致）
                    auto c1 = skinCfg.getColor("ui.button.sidebar_active");
                    auto c2 = skinCfg.getColor("ui.button.normal");
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(c2.r, c2.g, c2.b, c2.a));
                } else {
                    // 非激活态：全透明，仅悬停反馈
                    auto c1 = skinCfg.getColor("ui.button.transparent");
                    auto c2 = skinCfg.getColor("ui.button.transparent_hovered");
                    auto c3 = skinCfg.getColor("ui.button.transparent_active");
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(c2.r, c2.g, c2.b, c2.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(c3.r, c3.g, c3.b, c3.a));
                }

                // 应用皮肤配置的图标颜色
                Config::Color iconColor = skinCfg.getColor("icon");
                ImVec4        iconVec4(
                    iconColor.r, iconColor.g, iconColor.b, iconColor.a);
                if ( !isActive ) {
                    iconVec4.w *= 0.7f;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

                // 应用设置面板内部的字体图标尺寸
                ImFont* settingIconFont = skinCfg.getFont("setting_internal");
                if ( settingIconFont ) ImGui::PushFont(settingIconFont);

                std::string btnId = std::string(iconStr) + "##setting_tab_" +
                                    std::to_string((int)tab);
                if ( ImGui::Button(btnId.c_str(),
                                   { sidebarWidth, sidebarWidth }) ) {
                    m_currentTab = tab;
                }

                if ( settingIconFont ) ImGui::PopFont();

                if ( ImGui::IsItemHovered() ) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tooltip);
                    ImGui::EndTooltip();
                }

                ImGui::PopStyleColor(4);
            };

            DrawCategoryIcon(SettingsTab::Software,
                             ICON_MMM_DESKTOP,
                             TR_CACHE("ui.settings.software").data());
            DrawCategoryIcon(SettingsTab::Visual,
                             ICON_MMM_EYE,
                             TR_CACHE("ui.settings.visual").data());
            DrawCategoryIcon(SettingsTab::Project,
                             ICON_MMM_FOLDER,
                             TR_CACHE("ui.settings.project").data());
            DrawCategoryIcon(SettingsTab::Editor,
                             ICON_MMM_PEN,
                             TR_CACHE("ui.settings.editor").data());

            ImGui::PopStyleVar(4);
            ImGui::EndChild();
        });

    // 中间分割线
    rootHBox.addElement("SettingsSeparator",
                        Sizing::Fixed(1.0f),
                        Sizing::Grow(),
                        [](Clay_BoundingBox r, bool) {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddLine(
                                { p.x + 0.0f, p.y },
                                { p.x + 0.0f, p.y + r.height },
                                IM_COL32(80, 80, 80, 255));
                        });

    // 右侧内容区
    rootHBox.addElement(
        "SettingsContentArea",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, &skinCfg](Clay_BoundingBox r, bool isHovered) {
            ImGui::BeginChild("SettingsContent", { r.width, r.height }, false);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

            // 应用设置内容字体
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);

            switch ( m_currentTab ) {
            case SettingsTab::Software: drawSoftwareSettings(); break;
            case SettingsTab::Visual: drawVisualSettings(); break;
            case SettingsTab::Project: drawProjectSettings(); break;
            case SettingsTab::Editor: drawEditorSettings(); break;
            }

            if ( contentFont ) ImGui::PopFont();

            ImGui::PopStyleVar();
            ImGui::EndChild();
        });

    rootHBox.render(layoutContext);
}

void SettingsView::drawSoftwareSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    ImGui::SeparatorText(TR_CACHE("ui.settings.software.general").data());

    // 1. 语言选择
    const char* langs[]     = { "简体中文 (zh_cn)", "English (en_us)" };
    const char* langIDs[]   = { "zh_cn", "en_us" };
    int         currentLang = (settings.language == "en_us") ? 1 : 0;

    if ( ImGui::Combo(TR_CACHE("ui.settings.software.language").data(),
                      &currentLang,
                      langs,
                      IM_ARRAYSIZE(langs)) ) {
        settings.language = langIDs[currentLang];
        // 实时切换语言
        Config::SkinManager::instance().getTranslator().switchLang(
            settings.language);
        changed = true;
    }

    // 1.5 垂直同步
    changed |= ImGui::Checkbox(TR_CACHE("ui.settings.software.vsync").data(),
                               &settings.vsync);

    // 2. 光标样式 (移至此处)
    ImGui::Text("%s", TR_CACHE("ui.settings.editor.cursor_style").data());
    ImGui::SameLine();
    int cursorStyle = (int)settings.cursorStyle;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.editor.cursor_software").data(),
             cursorStyle == (int)Config::CursorStyle::Software) ) {
        settings.cursorStyle = Config::CursorStyle::Software;
        changed              = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(TR_CACHE("ui.settings.editor.cursor_system").data(),
                            cursorStyle == (int)Config::CursorStyle::System) ) {
        settings.cursorStyle = Config::CursorStyle::System;
        changed              = true;
    }

    // 3. 文件选择器样式
    int pickerStyle = (int)settings.filePickerStyle;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_native").data(),
             pickerStyle == (int)Config::FilePickerStyle::Native) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Native;
        changed                  = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_unified").data(),
             pickerStyle == (int)Config::FilePickerStyle::Unified) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Unified;
        changed                  = true;
    }

    // 4. 最近项目上限
    if ( ImGui::SliderInt(TR_CACHE("ui.settings.software.recent_limit").data(),
                          &settings.recentProjectsLimit,
                          1,
                          50) ) {
        changed = true;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.software.sync").data());
    // 3. 同步模式
    int         syncMode    = (int)settings.syncConfig.mode;
    const char* syncModes[] = { "None", "Integral", "WaterTank" };
    if ( ImGui::Combo(TR_CACHE("ui.settings.software.sync_mode").data(),
                      &syncMode,
                      syncModes,
                      IM_ARRAYSIZE(syncModes)) ) {
        settings.syncConfig.mode = (Config::SyncMode)syncMode;
        changed                  = true;
    }

    if ( settings.syncConfig.mode == Config::SyncMode::Integral ) {
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.sync_factor").data(),
            &settings.syncConfig.integralFactor,
            0.0f,
            1.0f);
    } else if ( settings.syncConfig.mode == Config::SyncMode::WaterTank ) {
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.sync_buffer").data(),
            &settings.syncConfig.waterTankBuffer,
            0.0f,
            0.5f);
    }

    changed |=
        ImGui::DragScalar(TR_CACHE("ui.settings.software.sync_interval").data(),
                          ImGuiDataType_Double,
                          &settings.syncConfig.syncInterval,
                          0.1f,
                          nullptr,
                          nullptr,
                          "%.1f s");

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

void SettingsView::drawVisualSettings()
{
    auto& visual  = Config::AppConfig::instance().getVisualConfig();
    bool  changed = false;

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.layout").data());
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_left").data(),
                           &visual.trackLayout.left,
                           0.0f,
                           1.0f);
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_top").data(),
                           &visual.trackLayout.top,
                           0.0f,
                           1.0f);
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_right").data(),
                           &visual.trackLayout.right,
                           0.0f,
                           1.0f);
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_bottom").data(),
                           &visual.trackLayout.bottom,
                           0.0f,
                           1.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.layout_box_width").data(),
        &visual.trackBoxLineWidth,
        1.0f,
        10.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.judgeline").data());
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.judgeline_pos").data(),
                           &visual.judgeline_pos,
                           0.0f,
                           1.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.judgeline_width").data(),
        &visual.judgelineWidth,
        1.0f,
        10.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.note").data());
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.note_scale_x").data(),
                           &visual.noteScaleX,
                           0.5f,
                           3.0f);
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.note_scale_y").data(),
                           &visual.noteScaleY,
                           0.5f,
                           3.0f);

    int         noteFillMode = (int)visual.noteFillMode;
    const char* fillModes[]  = {
        "Stretch", "AspectFit", "AspectFill", "Center"
    };
    if ( ImGui::Combo(TR_CACHE("ui.settings.visual.note_fill_mode").data(),
                      &noteFillMode,
                      fillModes,
                      IM_ARRAYSIZE(fillModes)) ) {
        visual.noteFillMode = (Config::BackgroundFillMode)noteFillMode;
        changed             = true;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.background").data());
    int bgFillMode = (int)visual.background.fillMode;
    if ( ImGui::Combo(TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
                      &bgFillMode,
                      fillModes,
                      IM_ARRAYSIZE(fillModes)) ) {
        visual.background.fillMode = (Config::BackgroundFillMode)bgFillMode;
        changed                    = true;
    }
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.bg_opaque").data(),
                           &visual.background.opaque_ratio,
                           0.0f,
                           1.0f);
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.bg_darken").data(),
                           &visual.background.darken_ratio,
                           0.0f,
                           1.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.preview").data());
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.preview_ratio").data(),
                           &visual.previewConfig.areaRatio,
                           1.0f,
                           10.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity").data(),
        &visual.previewConfig.edgeScrollSensitivity,
        0.0f,
        5.0f,
        "%.2f");
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.preview_margin_left").data(),
        &visual.previewConfig.margin.left,
        0.0f,
        20.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.preview_margin_top").data(),
        &visual.previewConfig.margin.top,
        0.0f,
        20.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.preview_margin_right").data(),
        &visual.previewConfig.margin.right,
        0.0f,
        20.0f);
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
        &visual.previewConfig.margin.bottom,
        0.0f,
        20.0f);

    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.timeline_zoom").data(),
                           &visual.timelineZoom,
                           0.1f,
                           5.0f,
                           "%.2fx");

    changed |=
        ImGui::Checkbox(TR_CACHE("ui.settings.visual.linear_scroll").data(),
                        &visual.enableLinearScrollMapping);

    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.snap_threshold").data(),
                           &visual.snapThreshold,
                           0.0f,
                           20.0f,
                           "%.1f px");

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.offset").data());
    if ( ImGui::DragFloat(TR_CACHE("ui.settings.visual.visual_offset").data(),
                          &visual.visualOffset,
                          0.001f,
                          -0.5f,
                          0.5f,
                          "%.3f s") ) {
        changed = true;
    }

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

void SettingsView::drawProjectSettings()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    if ( !project ) {
        auto dangerCol = Config::SkinManager::instance().getColor("ui.danger");
        ImGui::TextColored(
            ImVec4(dangerCol.r, dangerCol.g, dangerCol.b, dangerCol.a),
            "%s",
            TR("ui.settings.project.no_project").data());
        return;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.project.info").data());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.name").data(),
                project->m_metadata.m_title.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.artist").data(),
                project->m_metadata.m_artist.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.mapper").data(),
                project->m_metadata.m_mapper.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.path").data(),
                project->m_projectRoot.string().c_str());

    // TODO: 允许修改项目元数据
}

void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    ImGui::SeparatorText(TR_CACHE("ui.settings.editor.behavior").data());
    changed |=
        ImGui::Checkbox(TR_CACHE("ui.settings.editor.reverse_scroll").data(),
                        &settings.reverseScroll);
    changed |=
        ImGui::Checkbox(TR_CACHE("ui.settings.editor.scroll_snap").data(),
                        &settings.scrollSnap);

    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
        &settings.scrollSpeedMultiplier,
        1.0f,
        10.0f,
        "%.1f");

    int beatDivisor = settings.beatDivisor;
    if ( ImGui::SliderInt(TR_CACHE("ui.settings.editor.beat_divisor").data(),
                          &beatDivisor,
                          1,
                          64) ) {
        settings.beatDivisor = beatDivisor;
        changed              = true;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.editor.sfx").data());

    int         strategy     = (int)settings.sfxConfig.polylineStrategy;
    const char* strategies[] = {
        "Exact", "InternalAsNormal", "OnlyTailExact", "AllAsNormal"
    };
    if ( ImGui::Combo(TR_CACHE("ui.settings.editor.sfx_strategy").data(),
                      &strategy,
                      strategies,
                      IM_ARRAYSIZE(strategies)) ) {
        settings.sfxConfig.polylineStrategy =
            (Config::PolylineSfxStrategy)strategy;
        changed = true;
    }

    changed |=
        ImGui::Checkbox(TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
                        &settings.sfxConfig.enableFlickWidthVolumeScaling);
    if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
            &settings.sfxConfig.flickWidthVolumeMultiplier,
            0.0f,
            10.0f);
    }

    bool syncSpeedChanged =
        ImGui::Checkbox(TR_CACHE("ui.settings.editor.sfx_sync_speed").data(),
                        &settings.sfxConfig.hitSfxSyncSpeed);

    if ( syncSpeedChanged ) {
        changed = true;
        Audio::AudioManager::instance().updateSFXSyncSpeedRouting(
            settings.sfxConfig.hitSfxSyncSpeed);
    }

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
