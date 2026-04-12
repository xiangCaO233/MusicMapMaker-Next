#include "ui/imgui/manager/SettingsView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"

namespace MMM::UI
{

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

    // 2. 光标样式
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

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.beat_line").data());
    changed |= ImGui::SliderFloat(
        TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
        &visual.beatLineAlpha,
        0.0f,
        1.0f);

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

} // namespace MMM::UI
