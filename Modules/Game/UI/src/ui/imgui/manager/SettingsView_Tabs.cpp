#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include <ImGuiFileDialog.h>
#include <filesystem>

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

    // 1.6 UI 主题
    ImGui::Text("%s", TR_CACHE("ui.settings.software.theme").data());
    ImGui::SameLine();
    int         theme    = (int)settings.theme;
    const char* themes[] = { TR_CACHE("ui.settings.software.theme.auto").data(),
                             "DeepDark",
                             "Dark",
                             "Light",
                             "Classic",
                             "Microsoft",
                             "Darcula",
                             "Photoshop",
                             "Unreal",
                             "Gold",
                             "RoundedVisualStudio",
                             "SonicRiders",
                             "DarkRuda",
                             "SoftCherry",
                             "Enemymouse",
                             "DiscordDark",
                             "Comfy",
                             "PurpleComfy",
                             "FutureDark",
                             "CleanDark",
                             "Moonlight",
                             "ComfortableLight",
                             "HazyDark",
                             "Everforest",
                             "Windark",
                             "Rest",
                             "ComfortableDarkCyan",
                             "KazamCherry" };
    ImGui::SetNextItemWidth(200.0f);
    if ( ImGui::Combo("##ThemeCombo", &theme, themes, IM_ARRAYSIZE(themes)) ) {
        settings.theme = (Config::UITheme)theme;
        changed        = true;
    }

    // 1.7 字体选择
    auto& skinMgr    = Config::SkinManager::instance();
    auto& asciiFonts = skinMgr.getAsciiFonts();
    auto& cjkFonts   = skinMgr.getCjkFonts();

    std::string currentAscii = settings.preferredAsciiFont.empty()
                                   ? "Default"
                                   : settings.preferredAsciiFont;
    std::string currentCjk   = settings.preferredCjkFont.empty()
                                   ? "Default"
                                   : settings.preferredCjkFont;

    bool fontChanged = false;

    // ASCII 字体
    if ( currentAscii == "Default" && !asciiFonts.empty() ) {
        currentAscii = asciiFonts.front().first;
    }

    ImGui::Text("%s", TR_CACHE("ui.settings.software.font.ascii").data());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if ( ImGui::BeginCombo("##AsciiFontCombo", currentAscii.c_str()) ) {
        for ( const auto& [name, path] : asciiFonts ) {
            bool        isSelected = (currentAscii == name);
            std::string label      = name + "##" + path.string();
            if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                settings.preferredAsciiFont = name;
                fontChanged                 = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if ( ImGui::Button("...##BrowseAscii") ) {
        IGFD::FileDialogConfig config;
        config.path = ".";
        ImGuiFileDialog::Instance()->OpenDialog(
            "AsciiFontPicker",
            TR_CACHE("ui.settings.software.font.browse").data(),
            ".ttf,.otf",
            config);
    }

    // CJK 字体
    if ( currentCjk == "Default" && !cjkFonts.empty() ) {
        currentCjk = cjkFonts.front().first;
    }

    ImGui::Text("%s", TR_CACHE("ui.settings.software.font.cjk").data());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if ( ImGui::BeginCombo("##CjkFontCombo", currentCjk.c_str()) ) {
        for ( const auto& [name, path] : cjkFonts ) {
            bool        isSelected = (currentCjk == name);
            std::string label      = name + "##" + path.string();
            if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                settings.preferredCjkFont = name;
                fontChanged               = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if ( ImGui::Button("...##BrowseCjk") ) {
        IGFD::FileDialogConfig config;
        config.path = ".";
        ImGuiFileDialog::Instance()->OpenDialog(
            "CjkFontPicker",
            TR_CACHE("ui.settings.software.font.browse").data(),
            ".ttf,.otf",
            config);
    }

    // 处理文件选择器结果
    if ( ImGuiFileDialog::Instance()->Display(
             "AsciiFontPicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            settings.preferredAsciiFont =
                ImGuiFileDialog::Instance()->GetFilePathName();
            fontChanged = true;
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if ( ImGuiFileDialog::Instance()->Display(
             "CjkFontPicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            settings.preferredCjkFont =
                ImGuiFileDialog::Instance()->GetFilePathName();
            fontChanged = true;
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if ( fontChanged ) {
        changed = true;
        // 执行热重载
        if ( auto ctx = Graphic::VKContext::get() ) {
            ctx->get().rebuildFonts();
        }
    }

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

    if ( settings.cursorStyle == Config::CursorStyle::Software ) {
        ImGui::SeparatorText(
            TR_CACHE("ui.settings.software.cursor_params").data());
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.cursor_size").data(),
            &settings.softwareCursorConfig.cursorSize,
            4.0f,
            512.0f,
            "%.1f px");
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.trail_size").data(),
            &settings.softwareCursorConfig.trailSize,
            4.0f,
            512.0f,
            "%.1f px");
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.trail_life").data(),
            &settings.softwareCursorConfig.trailLifeTime,
            0.05f,
            5.0f,
            "%.2f s");
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.smoke_size").data(),
            &settings.softwareCursorConfig.smokeSize,
            4.0f,
            512.0f,
            "%.1f px");

        changed |= ImGui::Checkbox(
            TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
            &settings.softwareCursorConfig.enableBpmSyncSmokeLife);

        if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
            ImGui::BeginDisabled();
        changed |= ImGui::SliderFloat(
            TR_CACHE("ui.settings.software.smoke_life").data(),
            &settings.softwareCursorConfig.smokeLifeTime,
            0.05f,
            10.0f,
            "%.2f s");
        if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
            ImGui::EndDisabled();
    }

    // 3. 文件选择器样式
    ImGui::Text("%s", TR_CACHE("ui.settings.software.picker_style").data());
    ImGui::SameLine();
    int pickerStyle = (int)settings.filePickerStyle;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_unified").data(),
             pickerStyle == (int)Config::FilePickerStyle::Unified) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Unified;
        changed                  = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_native").data(),
             pickerStyle == (int)Config::FilePickerStyle::Native) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Native;
        changed                  = true;
    }

    // 4. 保存偏好
    ImGui::Text("%s", TR_CACHE("ui.settings.software.save_format").data());
    ImGui::SameLine();
    int savePreference = (int)settings.saveFormatPreference;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.save_format.original").data(),
             savePreference == (int)Config::SaveFormatPreference::Original) ) {
        settings.saveFormatPreference = Config::SaveFormatPreference::Original;
        changed                       = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.save_format.force_mmm").data(),
             savePreference == (int)Config::SaveFormatPreference::ForceMMM) ) {
        settings.saveFormatPreference = Config::SaveFormatPreference::ForceMMM;
        changed                       = true;
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
    if ( ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_left").data(),
                            &visual.trackLayout.left,
                            0.0f,
                            1.0f) ) {
        visual.trackLayout.left =
            std::min(visual.trackLayout.left, visual.trackLayout.right - 0.01f);
        changed = true;
    }
    if ( ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_top").data(),
                            &visual.trackLayout.top,
                            0.0f,
                            1.0f) ) {
        visual.trackLayout.top =
            std::min(visual.trackLayout.top, visual.trackLayout.bottom - 0.01f);
        changed = true;
    }
    if ( ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_right").data(),
                            &visual.trackLayout.right,
                            0.0f,
                            1.0f) ) {
        visual.trackLayout.right =
            std::max(visual.trackLayout.right, visual.trackLayout.left + 0.01f);
        changed = true;
    }
    if ( ImGui::SliderFloat(TR_CACHE("ui.settings.visual.layout_bottom").data(),
                            &visual.trackLayout.bottom,
                            0.0f,
                            1.0f) ) {
        visual.trackLayout.bottom =
            std::max(visual.trackLayout.bottom, visual.trackLayout.top + 0.01f);
        changed = true;
    }
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
    changed |= ImGui::Checkbox(
        TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
        &visual.previewConfig.drawBeatLines);
    changed |= ImGui::Checkbox(
        TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
        &visual.previewConfig.drawTimingLines);

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
                           48.0f,
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
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.project.no_project").data());
        return;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.project.info").data());
    auto        u8 = project->m_projectRoot.u8string();
    std::string projPath(reinterpret_cast<const char*>(u8.c_str()), u8.size());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.path").data(),
                projPath.c_str());
}

void SettingsView::drawBeatmapSettings()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto  session = engine.getActiveSession();
    auto* project = engine.getCurrentProject();

    if ( !session || !session->getContext().currentBeatmap ) {
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.beatmap.no_beatmap").data());
        return;
    }

    auto& beatmap = *session->getContext().currentBeatmap;
    auto  meta    = beatmap.m_baseMapMetadata;
    bool  changed = false;

    ImGui::SeparatorText(TR_CACHE("ui.settings.beatmap.info").data());

    auto DrawInput = [&](const char* label, std::string& val) {
        char buf[256];
        strncpy(buf, val.c_str(), sizeof(buf));
        if ( ImGui::InputText(label, buf, sizeof(buf)) ) {
            val     = buf;
            changed = true;
        }
    };

    DrawInput(TR_CACHE("ui.settings.beatmap.name").data(), meta.name);
    DrawInput(TR_CACHE("ui.settings.beatmap.title").data(), meta.title);
    DrawInput(TR_CACHE("ui.settings.beatmap.title_unicode").data(),
              meta.title_unicode);
    DrawInput(TR_CACHE("ui.settings.beatmap.artist").data(), meta.artist);
    DrawInput(TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
              meta.artist_unicode);
    DrawInput(TR_CACHE("ui.settings.beatmap.mapper").data(), meta.author);
    DrawInput(TR_CACHE("ui.settings.beatmap.version").data(), meta.version);

    ImGui::SeparatorText(TR_CACHE("ui.settings.beatmap.cover_type").data());
    int coverType = (int)meta.cover_type;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.beatmap.cover_type.image").data(),
             coverType == 0) ) {
        meta.cover_type = MMM::CoverType::IMAGE;
        changed         = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.beatmap.cover_type.video").data(),
             coverType == 1) ) {
        meta.cover_type = MMM::CoverType::VIDEO;
        changed         = true;
    }

    if ( meta.cover_type == MMM::CoverType::VIDEO ) {
        if ( ImGui::InputInt(TR_CACHE("ui.settings.beatmap.video_start").data(),
                             &meta.video_starttime) ) {
            changed = true;
        }
    }

    int offsets[2] = { meta.bgxoffset, meta.bgyoffset };
    if ( ImGui::DragInt2(TR_CACHE("ui.settings.beatmap.bg_offset").data(),
                         offsets) ) {
        meta.bgxoffset = offsets[0];
        meta.bgyoffset = offsets[1];
        changed        = true;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.beatmap.preference").data());
    float bpm = (float)meta.preference_bpm;
    if ( ImGui::DragFloat(TR_CACHE("ui.settings.beatmap.bpm").data(),
                          &bpm,
                          0.1f,
                          -1.0f,
                          1000.0f,
                          "%.2f") ) {
        meta.preference_bpm = (double)bpm;
        changed             = true;
    }

    if ( ImGui::InputInt(TR_CACHE("ui.settings.beatmap.tracks").data(),
                         &meta.track_count) ) {
        changed = true;
    }

    ImGui::BeginDisabled();
    double length = meta.map_length;
    ImGui::InputDouble(
        TR_CACHE("ui.settings.beatmap.length").data(), &length, 0, 0, "%.3f s");
    ImGui::EndDisabled();

    ImGui::SeparatorText(TR_CACHE("ui.settings.beatmap.resource").data());

    auto        audioU8 = meta.main_audio_path.u8string();
    std::string currentAudioPath(reinterpret_cast<const char*>(audioU8.c_str()),
                                 audioU8.size());
    std::string audioPreview = currentAudioPath;
    if ( project && !audioPreview.empty() ) {
        std::filesystem::path p(audioU8);
        if ( p.is_absolute() ) {
            try {
                audioPreview =
                    std::filesystem::relative(p, project->m_projectRoot)
                        .generic_string();
            } catch ( ... ) {
            }
        }
    }

    bool audioExists = false;
    if ( project ) {
        auto absAudio = project->m_projectRoot / meta.main_audio_path;
        audioExists   = std::filesystem::exists(absAudio);
    }

    bool audioPushed = false;
    if ( !audioExists && !currentAudioPath.empty() ) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              Utils::UIThemeUtils::getWarningColor());
        audioPushed = true;
    }

    if ( ImGui::BeginCombo(TR_CACHE("ui.settings.beatmap.audio").data(),
                           audioPreview.c_str()) ) {
        if ( audioPushed ) {
            ImGui::PopStyleColor();
            audioPushed = false;
        }

        if ( project ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_type != MMM::AudioTrackType::Main ) continue;

                bool        isSelected = (currentAudioPath == res.m_path);
                std::string label      = res.m_id + "##" + res.m_path;
                if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                    meta.main_audio_path = res.m_path;
                    changed              = true;
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", res.m_path.c_str());
            }
        } else {
            ImGui::TextDisabled(
                "%s", TR_CACHE("ui.settings.beatmap.no_beatmap").data());
        }
        ImGui::EndCombo();
    }
    if ( audioPushed ) ImGui::PopStyleColor();

    auto        coverU8 = meta.main_cover_path.u8string();
    std::string currentCoverPath(reinterpret_cast<const char*>(coverU8.c_str()),
                                 coverU8.size());
    std::string coverPreview = currentCoverPath;
    if ( project && !coverPreview.empty() ) {
        std::filesystem::path p(coverU8);
        if ( p.is_absolute() ) {
            try {
                coverPreview =
                    std::filesystem::relative(p, project->m_projectRoot)
                        .generic_string();
            } catch ( ... ) {
            }
        }
    }

    bool coverExists = false;
    if ( project ) {
        auto absCover = project->m_projectRoot / meta.main_cover_path;
        coverExists   = std::filesystem::exists(absCover);
    }

    bool coverPushed = false;
    if ( !coverExists && !currentCoverPath.empty() ) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              Utils::UIThemeUtils::getWarningColor());
        coverPushed = true;
    }

    if ( ImGui::BeginCombo(TR_CACHE("ui.settings.beatmap.cover").data(),
                           coverPreview.c_str()) ) {
        if ( coverPushed ) {
            ImGui::PopStyleColor();
            coverPushed = false;
        }

        if ( project ) {
            // 扫描项目中的图片文件
            std::vector<std::string> images;
            try {
                for ( const auto& entry :
                      std::filesystem::recursive_directory_iterator(
                          project->m_projectRoot) ) {
                    if ( entry.is_regular_file() ) {
                        auto ext = entry.path().extension().string();
                        std::transform(
                            ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                             ext == ".bmp" || ext == ".mp4" || ext == ".avi" ) {
                            auto rel = std::filesystem::relative(
                                entry.path(), project->m_projectRoot);
                            images.push_back(rel.generic_string());
                        }
                    }
                }
            } catch ( ... ) {
            }

            for ( const auto& imgPath : images ) {
                bool        isSelected = (currentCoverPath == imgPath);
                std::string label      = imgPath + "##" + imgPath;
                if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                    meta.main_cover_path = imgPath;
                    changed              = true;
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
            }
        } else {
            ImGui::TextDisabled(
                "%s", TR_CACHE("ui.settings.beatmap.no_beatmap").data());
        }
        ImGui::EndCombo();
    }
    if ( coverPushed ) ImGui::PopStyleColor();

    if ( changed ) {
        engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
    }
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
    changed |= ImGui::Checkbox(
        TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
            .data(),
        &settings.disableScrollAccelerationWhileDrawing);

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

    ImGui::SeparatorText("Selection (框选)");
    int mode = (int)settings.selectionMode;
    if ( ImGui::RadioButton("Strict (严格)",
                            mode == (int)Config::SelectionMode::Strict) ) {
        settings.selectionMode = Config::SelectionMode::Strict;
        changed                = true;
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             "Intersection (相交)",
             mode == (int)Config::SelectionMode::Intersection) ) {
        settings.selectionMode = Config::SelectionMode::Intersection;
        changed                = true;
    }

    changed |= ImGui::SliderFloat("Thickness (边框宽度)",
                                  &settings.marqueeThickness,
                                  1.0f,
                                  10.0f,
                                  "%.1f px");
    changed |= ImGui::SliderFloat("Rounding (圆角范围)",
                                  &settings.marqueeRounding,
                                  0.0f,
                                  20.0f,
                                  "%.1f px");

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
