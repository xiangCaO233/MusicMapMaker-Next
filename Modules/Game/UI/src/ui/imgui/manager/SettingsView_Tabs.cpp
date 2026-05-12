#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
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

float SettingsView::measureLabelWidth(const char* label)
{
    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) font = ImGui::GetFont();
    float  fontSize = font->LegacySize * font->Scale;
    ImVec2 sz       = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    return sz.x;
}

void SettingsView::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                  const char* label, float labelWidth,
                                  CLayBox::DrawFunc widget)
{
    auto& row = getRow(rowIndex++);
    row.setPadding(4, 4, 2, 2).setSpacing(6).setAlignment(Alignment::Center());

    // 标签列（固定宽度 = 最长标签宽度，右对齐文本，垂直居中）
    row.addElement(std::string(label) + "_label",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       auto&   skinMgr = Config::SkinManager::instance();
                       ImFont* font    = skinMgr.getFont("content");
                       if ( !font ) font = ImGui::GetFont();
                       float  fontSize = font->LegacySize * font->Scale;
                       ImVec2 textSz =
                           font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
                       // 右对齐 + 垂直居中
                       ImVec2 pos = { r.x + r.width - textSz.x,
                                      r.y + (r.height - textSz.y) * 0.5f };
                       // 使用 ImGui 主题的 Text 颜色
                       ImVec4 textCol = ImGui::GetStyle().Colors[ImGuiCol_Text];
                       textCol.w *= 0.85f;
                       ImGui::GetWindowDrawList()->AddText(
                           font,
                           fontSize,
                           pos,
                           ImGui::ColorConvertFloat4ToU32(textCol),
                           label);
                   });

    // 控件列（自适应剩余宽度）
    row.addElement(
        std::string(label) + "_widget", Sizing::Grow(), Sizing::Grow(), widget);

    // 行高 = ImGui 标准帧高（与控件高度一致）
    float rowH = ImGui::GetFrameHeightWithSpacing();
    parent.addLayout((std::string(label) + "_row").c_str(),
                     row,
                     Sizing::Grow(),
                     Sizing::Fixed(rowH));
}

void SettingsView::drawSoftwareSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(4).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        ImGuiID id     = ImGui::GetID(label);
        bool    isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeightWithSpacing();
        row.addElement(
            std::string(label) + "_header",
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushStyleColor(
                    ImGuiCol_Header,
                    ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::CollapsingHeader(
                    label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                ImGui::PopStyleColor();
            });
        m_contentVBox.addLayout((std::string(label) + "_hdr").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(2).setPadding(8, 8, 4, 4);
            m_contentVBox.addLayout((std::string(label) + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.software.general").data(), true) ) {
        // 计算本段所有标签的最大宽度
        const char* genLabels[] = {
            TR_CACHE("ui.settings.software.language").data(),
            TR_CACHE("ui.settings.software.vsync").data(),
            TR_CACHE("ui.settings.software.theme").data(),
            TR_CACHE("ui.settings.software.font.ascii").data(),
            TR_CACHE("ui.settings.software.font.cjk").data(),
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            TR_CACHE("ui.settings.software.font.multiplier").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : genLabels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        // 1. 语言选择
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.language").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                const char* langs[] = { "简体中文 (zh_cn)", "English (en_us)" };
                const char* langIDs[] = { "zh_cn", "en_us" };
                int currentLang       = (settings.language == "en_us") ? 1 : 0;
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##LangCombo",
                                  &currentLang,
                                  langs,
                                  IM_ARRAYSIZE(langs)) ) {
                    settings.language = langIDs[currentLang];
                    Config::SkinManager::instance().getTranslator().switchLang(
                        settings.language);
                    changed = true;
                }
            });

        // 2. 垂直同步
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.software.vsync").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |=
                               ImGui::Checkbox("##VSync", &settings.vsync);
                       });

        // 3. UI 主题
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.theme").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         theme    = (int)settings.theme;
                const char* themes[] = {
                    TR_CACHE("ui.settings.software.theme.auto").data(),
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
                    "KazamCherry"
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##ThemeCombo",
                                  &theme,
                                  themes,
                                  IM_ARRAYSIZE(themes)) ) {
                    settings.theme = (Config::UITheme)theme;
                    changed        = true;
                }
            });

        // 4. 字体选择 (ASCII)
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.ascii").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                auto& skinMgr    = Config::SkinManager::instance();
                auto& asciiFonts = skinMgr.getAsciiFonts();
                std::string currentAscii = settings.preferredAsciiFont.empty()
                                               ? "Default"
                                               : settings.preferredAsciiFont;
                if ( currentAscii == "Default" && !asciiFonts.empty() )
                    currentAscii = asciiFonts.front().first;

                ImGui::SetNextItemWidth(r.width - 40.0f);
                if ( ImGui::BeginCombo("##AsciiFontCombo",
                                       currentAscii.c_str()) ) {
                    for ( const auto& [name, path] : asciiFonts ) {
                        bool        isSelected = (currentAscii == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ImGui::Selectable(lbl.c_str(), isSelected) ) {
                            settings.preferredAsciiFont = name;
                            if (auto ctx = Graphic::VKContext::get()) ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if ( ImGui::Button("...##BrowseAscii", { 35, 0 }) ) {
                    IGFD::FileDialogConfig config;
                    config.path = ".";
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "AsciiFontPicker",
                        TR_CACHE("ui.settings.software.font.browse").data(),
                        ".ttf,.otf",
                        config);
                }
            });

        // 5. 字体选择 (CJK)
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.cjk").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                auto& skinMgr  = Config::SkinManager::instance();
                auto& cjkFonts = skinMgr.getCjkFonts();
                std::string currentCjk = settings.preferredCjkFont.empty()
                                             ? "Default"
                                             : settings.preferredCjkFont;
                if ( currentCjk == "Default" && !cjkFonts.empty() )
                    currentCjk = cjkFonts.front().first;

                ImGui::SetNextItemWidth(r.width - 40.0f);
                if ( ImGui::BeginCombo("##CjkFontCombo", currentCjk.c_str()) ) {
                    for ( const auto& [name, path] : cjkFonts ) {
                        bool        isSelected = (currentCjk == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ImGui::Selectable(lbl.c_str(), isSelected) ) {
                            settings.preferredCjkFont = name;
                            if (auto ctx = Graphic::VKContext::get()) ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if ( ImGui::Button("...##BrowseCjk", { 35, 0 }) ) {
                    IGFD::FileDialogConfig config;
                    config.path = ".";
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "CjkFontPicker",
                        TR_CACHE("ui.settings.software.font.browse").data(),
                        ".ttf,.otf",
                        config);
                }
            });

        // 6. 界面全局缩放
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::SliderFloat("##UIScale",
                                        &settings.uiScaleMultiplier,
                                        0.5f,
                                        2.0f,
                                        "%.2f") ) {
                    changed = true;
                    if ( auto ctx = Graphic::VKContext::get() ) {
                        ctx->get().applyTheme();
                        ctx->get().updateFontScales();
                        ctx->get().requestFontRebuild();
                    }
                }
            });

        // 7. 字体大小倍率
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.software.font.multiplier").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##FontScale",
                                                   &settings.fontSizeMultiplier,
                                                   0.5f,
                                                   2.0f,
                                                   "%.2f") ) {
                               changed = true;
                               if ( auto ctx = Graphic::VKContext::get() ) {
                                   ctx->get().updateFontScales();
                                   ctx->get().requestFontRebuild();
                               }
                           }
                       });

        // 处理文件选择器结果 (保持在 Clay 之后，因为它们开启新窗口)
        if ( ImGuiFileDialog::Instance()->Display("AsciiFontPicker",
                                                   ImGuiWindowFlags_NoCollapse,
                                                   { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                settings.preferredAsciiFont =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                if (auto ctx = Graphic::VKContext::get()) ctx->get().requestFontRebuild();
                changed = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if ( ImGuiFileDialog::Instance()->Display(
                 "CjkFontPicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                settings.preferredCjkFont =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                if (auto ctx = Graphic::VKContext::get()) ctx->get().requestFontRebuild();
                changed = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }

    // 2. 光标样式
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.software.cursor_params").data(),
                   true) ) {

        const char* curLabels[] = {
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            TR_CACHE("ui.settings.software.cursor_size").data(),
            TR_CACHE("ui.settings.software.trail_size").data(),
            TR_CACHE("ui.settings.software.trail_life").data(),
            TR_CACHE("ui.settings.software.smoke_size").data(),
            TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
            TR_CACHE("ui.settings.software.smoke_life").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : curLabels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int cursorStyle = (int)settings.cursorStyle;
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.editor.cursor_software").data(),
                         cursorStyle == (int)Config::CursorStyle::Software) ) {
                    settings.cursorStyle = Config::CursorStyle::Software;
                    changed              = true;
                }
                ImGui::SameLine();
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.editor.cursor_system").data(),
                         cursorStyle == (int)Config::CursorStyle::System) ) {
                    settings.cursorStyle = Config::CursorStyle::System;
                    changed              = true;
                }
            });

        if ( settings.cursorStyle == Config::CursorStyle::Software ) {
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.cursor_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##CursorSize",
                                   &settings.softwareCursorConfig.cursorSize,
                                   4.0f,
                                   512.0f,
                                   "%.1f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##TrailSize",
                                   &settings.softwareCursorConfig.trailSize,
                                   4.0f,
                                   512.0f,
                                   "%.1f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_life").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##TrailLife",
                                   &settings.softwareCursorConfig.trailLifeTime,
                                   0.05f,
                                   5.0f,
                                   "%.2f s");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.smoke_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##SmokeSize",
                                   &settings.softwareCursorConfig.smokeSize,
                                   4.0f,
                                   512.0f,
                                   "%.1f px");
                           });
            addSettingItem(*sec,
                rowIndex,
                TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    changed |= ImGui::Checkbox(
                        "##BpmSync",
                        &settings.softwareCursorConfig.enableBpmSyncSmokeLife);
                });
            addSettingItem(*sec,
                rowIndex,
                TR_CACHE("ui.settings.software.smoke_life").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
                        ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ImGui::SliderFloat(
                        "##SmokeLife",
                        &settings.softwareCursorConfig.smokeLifeTime,
                        0.05f,
                        10.0f,
                        "%.2f s");
                    if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
                        ImGui::EndDisabled();
                });
        }
    }

    // 3. 界面偏好 (文件选择器、保存格式等)
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.software.sync").data(), true) ) {

        const char* syncLabels[] = {
            TR_CACHE("ui.settings.software.picker_style").data(),
            TR_CACHE("ui.settings.software.save_format").data(),
            TR_CACHE("ui.settings.software.recent_limit").data(),
            TR_CACHE("ui.settings.software.sync_mode").data(),
            TR_CACHE("ui.settings.software.sync_factor").data(),
            TR_CACHE("ui.settings.software.sync_buffer").data(),
            TR_CACHE("ui.settings.software.sync_interval").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : syncLabels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        // 文件选择器样式
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.picker_style").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int pickerStyle = (int)settings.filePickerStyle;
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.software.picker_unified").data(),
                         pickerStyle ==
                             (int)Config::FilePickerStyle::Unified) ) {
                    settings.filePickerStyle = Config::FilePickerStyle::Unified;
                    changed                  = true;
                }
                ImGui::SameLine();
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.software.picker_native").data(),
                         pickerStyle ==
                             (int)Config::FilePickerStyle::Native) ) {
                    settings.filePickerStyle = Config::FilePickerStyle::Native;
                    changed                  = true;
                }
            });

        // 保存偏好
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.save_format").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int savePreference = (int)settings.saveFormatPreference;
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.software.save_format.original")
                             .data(),
                         savePreference ==
                             (int)Config::SaveFormatPreference::Original) ) {
                    settings.saveFormatPreference =
                        Config::SaveFormatPreference::Original;
                    changed = true;
                }
                ImGui::SameLine();
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.software.save_format.force_mmm")
                             .data(),
                         savePreference ==
                             (int)Config::SaveFormatPreference::ForceMMM) ) {
                    settings.saveFormatPreference =
                        Config::SaveFormatPreference::ForceMMM;
                    changed = true;
                }
            });

        // 最近项目上限
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.software.recent_limit").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderInt("##RecentLimit",
                                                 &settings.recentProjectsLimit,
                                                 1,
                                                 50) ) {
                               changed = true;
                           }
                       });

        // 同步设置
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.software.sync_mode").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         syncMode    = (int)settings.syncConfig.mode;
                const char* syncModes[] = { "None", "Integral", "WaterTank" };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##SyncMode",
                                  &syncMode,
                                  syncModes,
                                  IM_ARRAYSIZE(syncModes)) ) {
                    settings.syncConfig.mode = (Config::SyncMode)syncMode;
                    changed                  = true;
                }
            });

        if ( settings.syncConfig.mode == Config::SyncMode::Integral ) {
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.sync_factor").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##IntegralFactor",
                                   &settings.syncConfig.integralFactor,
                                   0.0f,
                                   1.0f);
                           });
        } else if ( settings.syncConfig.mode == Config::SyncMode::WaterTank ) {
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.sync_buffer").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ImGui::SliderFloat(
                                   "##WaterTankBuffer",
                                   &settings.syncConfig.waterTankBuffer,
                                   0.0f,
                                   0.5f);
                           });
        }

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.software.sync_interval").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::DragScalar(
                               "##SyncInterval",
                               ImGuiDataType_Double,
                               &settings.syncConfig.syncInterval,
                               0.1f,
                               nullptr,
                               nullptr,
                               "%.1f s");
                       });
    }

    // 统一执行 Clay 布局渲染
    ImVec2 sz = m_contentVBox.renderInCurrent(
        ImGui::GetCursorScreenPos(), { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::Dummy({ 0, sz.y });

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

    m_contentVBox.clear();
    m_contentVBox.setSpacing(4).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        ImGuiID id     = ImGui::GetID(label);
        bool    isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeightWithSpacing();
        row.addElement(
            std::string(label) + "_header",
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushStyleColor(
                    ImGuiCol_Header,
                    ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::CollapsingHeader(
                    label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                ImGui::PopStyleColor();
            });
        m_contentVBox.addLayout((std::string(label) + "_hdr").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(2).setPadding(8, 8, 4, 4);
            m_contentVBox.addLayout((std::string(label) + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.layout").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.visual.layout_left").data(),
            TR_CACHE("ui.settings.visual.layout_top").data(),
            TR_CACHE("ui.settings.visual.layout_right").data(),
            TR_CACHE("ui.settings.visual.layout_bottom").data(),
            TR_CACHE("ui.settings.visual.layout_box_width").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_left").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutLeft",
                                                   &visual.trackLayout.left,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.left =
                                   std::min(visual.trackLayout.left,
                                            visual.trackLayout.right - 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.layout_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::SliderFloat(
                         "##LayoutTop", &visual.trackLayout.top, 0.0f, 1.0f) ) {
                    visual.trackLayout.top =
                        std::min(visual.trackLayout.top,
                                 visual.trackLayout.bottom - 0.01f);
                    changed = true;
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_right").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutRight",
                                                   &visual.trackLayout.right,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.right =
                                   std::max(visual.trackLayout.right,
                                            visual.trackLayout.left + 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_bottom").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutBottom",
                                                   &visual.trackLayout.bottom,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.bottom =
                                   std::max(visual.trackLayout.bottom,
                                            visual.trackLayout.top + 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.layout_box_width").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##LayoutBoxWidth", &visual.trackBoxLineWidth, 1.0f, 10.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.judgeline").data(), true) ) {
        float maxLabelW =
            measureLabelWidth(
                TR_CACHE("ui.settings.visual.judgeline_pos").data()) +
            8.0f;
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.judgeline_pos").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##JudgeLinePos", &visual.judgeline_pos, 0.0f, 1.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.beat_line").data(), true) ) {
        float maxLabelW =
            measureLabelWidth(
                TR_CACHE("ui.settings.visual.beat_line_alpha").data()) +
            8.0f;
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BeatLineAlpha", &visual.beatLineAlpha, 0.0f, 1.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.note").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.visual.note_scale_x").data(),
            TR_CACHE("ui.settings.visual.note_scale_y").data(),
            TR_CACHE("ui.settings.visual.note_fill_mode").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.note_scale_x").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##NoteScaleX", &visual.noteScaleX, 0.5f, 3.0f);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.note_scale_y").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##NoteScaleY", &visual.noteScaleY, 0.5f, 3.0f);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.note_fill_mode").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           int         noteFillMode = (int)visual.noteFillMode;
                           const char* fillModes[]  = {
                               "Stretch", "AspectFit", "AspectFill", "Center"
                           };
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::Combo("##NoteFillMode",
                                             &noteFillMode,
                                             fillModes,
                                             IM_ARRAYSIZE(fillModes)) ) {
                               visual.noteFillMode =
                                   (Config::BackgroundFillMode)noteFillMode;
                               changed = true;
                           }
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.background").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            TR_CACHE("ui.settings.visual.bg_darken").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           int bgFillMode = (int)visual.background.fillMode;
                           const char* fillModes[] = {
                               "Stretch", "AspectFit", "AspectFill", "Center"
                           };
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::Combo("##BgFillMode",
                                             &bgFillMode,
                                             fillModes,
                                             IM_ARRAYSIZE(fillModes)) ) {
                               visual.background.fillMode =
                                   (Config::BackgroundFillMode)bgFillMode;
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgOpaque", &visual.background.opaque_ratio, 0.0f, 1.0f);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_darken").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgDarken", &visual.background.darken_ratio, 0.0f, 1.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.preview").data(), true) ) {
        const char* labels[] = {
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
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.preview_ratio").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##PreviewRatio",
                               &visual.previewConfig.areaRatio,
                               1.0f,
                               10.0f);
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##EdgeSens",
                    &visual.previewConfig.edgeScrollSensitivity,
                    0.0f,
                    5.0f,
                    "%.2f");
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##MarginL",
                                              &visual.previewConfig.margin.left,
                                              0.0f,
                                              20.0f);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##MarginT", &visual.previewConfig.margin.top, 0.0f, 20.0f);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ImGui::SliderFloat("##MarginR",
                                       &visual.previewConfig.margin.right,
                                       0.0f,
                                       20.0f);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ImGui::SliderFloat("##MarginB",
                                       &visual.previewConfig.margin.bottom,
                                       0.0f,
                                       20.0f);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox("##DrawBeatLines",
                                           &visual.previewConfig.drawBeatLines);
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DrawTimingLines", &visual.previewConfig.drawTimingLines);
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.timeline_zoom").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat("##TimelineZoom",
                                                         &visual.timelineZoom,
                                                         0.1f,
                                                         5.0f,
                                                         "%.2fx");
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.linear_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox(
                               "##LinearScroll",
                               &visual.enableLinearScrollMapping);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.snap_threshold").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat("##SnapThreshold",
                                                         &visual.snapThreshold,
                                                         0.0f,
                                                         48.0f,
                                                         "%.1f px");
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.offset").data(), true) ) {
        float maxLabelW =
            measureLabelWidth(
                TR_CACHE("ui.settings.visual.visual_offset").data()) +
            8.0f;
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.visual_offset").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::DragFloat("##VisualOffset",
                                                 &visual.visualOffset,
                                                 0.001f,
                                                 -0.5f,
                                                 0.5f,
                                                 "%.3f s") )
                               changed = true;
                       });
    }

    // 统一执行 Clay 布局渲染
    ImVec2 sz = m_contentVBox.renderInCurrent(
        ImGui::GetCursorScreenPos(), { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::Dummy({ 0, sz.y });

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

    if ( ImGui::CollapsingHeader(TR_CACHE("ui.settings.project.info").data(),
                                 ImGuiTreeNodeFlags_DefaultOpen) ) {
        std::string projPath = Config::pathToUtf8(project->m_projectRoot);
        ImGui::Text("%s: %s",
                    TR_CACHE("ui.settings.project.path").data(),
                    projPath.c_str());
    }
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

    if ( ImGui::CollapsingHeader(TR_CACHE("ui.settings.beatmap.info").data(),
                                 ImGuiTreeNodeFlags_DefaultOpen) ) {

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
    }

    if ( ImGui::CollapsingHeader(
             TR_CACHE("ui.settings.beatmap.cover_type").data(),
             ImGuiTreeNodeFlags_DefaultOpen) ) {
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
            if ( ImGui::InputInt(
                     TR_CACHE("ui.settings.beatmap.video_start").data(),
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
    }

    if ( ImGui::CollapsingHeader(
             TR_CACHE("ui.settings.beatmap.preference").data(),
             ImGuiTreeNodeFlags_DefaultOpen) ) {
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
        ImGui::InputDouble(TR_CACHE("ui.settings.beatmap.length").data(),
                           &length,
                           0,
                           0,
                           "%.3f s");
        ImGui::EndDisabled();
    }

    if ( ImGui::CollapsingHeader(
             TR_CACHE("ui.settings.beatmap.resource").data(),
             ImGuiTreeNodeFlags_DefaultOpen) ) {

        std::string currentAudioPath = Config::pathToUtf8(meta.main_audio_path);
        std::string audioPreview     = currentAudioPath;
        if ( project && !audioPreview.empty() ) {
            if ( meta.main_audio_path.is_absolute() ) {
                try {
                    audioPreview = Config::pathToUtf8(std::filesystem::relative(
                        meta.main_audio_path, project->m_projectRoot));
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

        std::string currentCoverPath = Config::pathToUtf8(meta.main_cover_path);
        std::string coverPreview     = currentCoverPath;
        if ( project && !coverPreview.empty() ) {
            if ( meta.main_cover_path.is_absolute() ) {
                try {
                    coverPreview = Config::pathToUtf8(std::filesystem::relative(
                        meta.main_cover_path, project->m_projectRoot));
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
                            auto ext =
                                Config::pathToUtf8(entry.path().extension());
                            std::transform(
                                ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if ( ext == ".png" || ext == ".jpg" ||
                                 ext == ".jpeg" || ext == ".bmp" ||
                                 ext == ".mp4" || ext == ".avi" ) {
                                auto rel = std::filesystem::relative(
                                    entry.path(), project->m_projectRoot);
                                images.push_back(Config::pathToUtf8(rel));
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
    }

    if ( changed ) {
        engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
    }
}

void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(4).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        ImGuiID id     = ImGui::GetID(label);
        bool    isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeightWithSpacing();
        row.addElement(
            std::string(label) + "_header",
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushStyleColor(
                    ImGuiCol_Header,
                    ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::CollapsingHeader(
                    label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                ImGui::PopStyleColor();
            });
        m_contentVBox.addLayout((std::string(label) + "_hdr").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(2).setPadding(8, 8, 4, 4);
            m_contentVBox.addLayout((std::string(label) + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.behavior").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.editor.reverse_scroll").data(),
            TR_CACHE("ui.settings.editor.scroll_snap").data(),
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            TR_CACHE("ui.settings.editor.beat_divisor").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.reverse_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox("##ReverseScroll",
                                                      &settings.reverseScroll);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.scroll_snap").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox("##ScrollSnap",
                                                      &settings.scrollSnap);
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DisableAccel",
                    &settings.disableScrollAccelerationWhileDrawing);
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##ScrollMul",
                               &settings.scrollSpeedMultiplier,
                               1.0f,
                               10.0f,
                               "%.1f");
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int beatDivisor = settings.beatDivisor;
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::SliderInt("##BeatDivisor", &beatDivisor, 1, 64) ) {
                    settings.beatDivisor = beatDivisor;
                    changed              = true;
                }
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.selection").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.editor.selection").data(),
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            TR_CACHE("ui.settings.editor.selection.rounding").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.selection").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int mode = (int)settings.selectionMode;
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.editor.selection.strict").data(),
                         mode == (int)Config::SelectionMode::Strict) ) {
                    settings.selectionMode = Config::SelectionMode::Strict;
                    changed                = true;
                }
                ImGui::SameLine();
                if ( ImGui::RadioButton(
                         TR_CACHE("ui.settings.editor.selection.intersection")
                             .data(),
                         mode == (int)Config::SelectionMode::Intersection) ) {
                    settings.selectionMode =
                        Config::SelectionMode::Intersection;
                    changed = true;
                }
            });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##MarqueeThick",
                                              &settings.marqueeThickness,
                                              1.0f,
                                              10.0f,
                                              "%.1f px");
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.selection.rounding").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |=
                               ImGui::SliderFloat("##MarqueeRound",
                                                  &settings.marqueeRounding,
                                                  0.0f,
                                                  20.0f,
                                                  "%.1f px");
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.sfx").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.editor.sfx_strategy").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.sfx_strategy").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           int strategy =
                               (int)settings.sfxConfig.polylineStrategy;
                           const char* strategies[] = { "Exact",
                                                        "InternalAsNormal",
                                                        "OnlyTailExact",
                                                        "AllAsNormal" };
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::Combo("##SfxStrategy",
                                             &strategy,
                                             strategies,
                                             IM_ARRAYSIZE(strategies)) ) {
                               settings.sfxConfig.polylineStrategy =
                                   (Config::PolylineSfxStrategy)strategy;
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##FlickScale",
                    &settings.sfxConfig.enableFlickWidthVolumeScaling);
            });
        if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
            addSettingItem(*sec,
                rowIndex,
                TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ImGui::SliderFloat(
                        "##FlickMul",
                        &settings.sfxConfig.flickWidthVolumeMultiplier,
                        0.0f,
                        10.0f);
                });
        }
        addSettingItem(*sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                bool syncSpeedChanged = ImGui::Checkbox(
                    "##SyncSpeed", &settings.sfxConfig.hitSfxSyncSpeed);
                if ( syncSpeedChanged ) {
                    changed = true;
                    Audio::AudioManager::instance().updateSFXSyncSpeedRouting(
                        settings.sfxConfig.hitSfxSyncSpeed);
                }
            });
    }

    // 统一执行 Clay 布局渲染
    ImVec2 sz = m_contentVBox.renderInCurrent(
        ImGui::GetCursorScreenPos(), { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::Dummy({ 0, sz.y });

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI