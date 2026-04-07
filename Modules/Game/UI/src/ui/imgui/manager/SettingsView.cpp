#include "ui/imgui/manager/SettingsView.h"
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
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                } else {
                    // 非激活态：全透明，仅悬停反馈
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
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

    // 1. 语言选择 (示例，目前只有 zh_cn 和 en_us)
    const char* langs[]     = { "简体中文 (zh_cn)", "English (en_us)" };
    static int  currentLang = 0;  // TODO: 从配置中获取
    if ( ImGui::Combo(TR_CACHE("ui.settings.software.language").data(),
                      &currentLang,
                      langs,
                      IM_ARRAYSIZE(langs)) ) {
        // TODO: 切换语言逻辑
        changed = true;
    }

    // 2. 文件选择器样式
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

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.background").data());
    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.bg_darken").data(),
                           &visual.background.darken_ratio,
                           0.0f,
                           1.0f);

    changed |=
        ImGui::SliderFloat(TR_CACHE("ui.settings.visual.timeline_zoom").data(),
                           &visual.timelineZoom,
                           0.1f,
                           5.0f,
                           "%.2fx");

    changed |=
        ImGui::Checkbox(TR_CACHE("ui.settings.visual.linear_scroll").data(),
                        &visual.enableLinearScrollMapping);

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
        ImGui::TextColored(ImVec4(1, 0, 0, 1),
                           "%s",
                           TR_CACHE("ui.settings.project.no_project").data());
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

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
