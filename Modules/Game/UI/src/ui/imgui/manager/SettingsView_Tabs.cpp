#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <nfd.h>

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
    // 统一 Padding 为 8px
    row.setPadding(8, 8, 0, 0).setSpacing(8).setAlignment(Alignment::Center());

    std::string labelId = "R" + std::to_string(rowIndex) + "_L_" + label;
    row.addElement(labelId + "_lbl",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       float textH  = ImGui::CalcTextSize(label).y;
                       float offset = (r.height - textH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       ImGui::Text("%s", label);
                   });

    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) {
                       float widgetH = ImGui::GetFrameHeight();
                       float offset  = (r.height - widgetH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       widget(r, h);
                   });

    float rowH = ImGui::GetFrameHeight() + 8.0f;
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

void SettingsView::addRadioSetting(
    CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex, const char* label,
    float labelWidth, const std::vector<std::pair<std::string, int>>& options,
    int& current, bool& changed)
{
    // 获取稳定宽度
    float totalAvailW  = ImGui::GetWindowContentRegionMax().x -
                         ImGui::GetWindowContentRegionMin().x;
    float widgetAvailW = std::max(100.0f, totalAvailW - labelWidth - 32.0f);

    auto& row = getRow(rowIndex++);
    row.setPadding(8, 8, 0, 0).setSpacing(8).setAlignment(Alignment::Center());

    std::string labelId = "S" + std::to_string(sectionIndex) + "_R" +
                          std::to_string(rowIndex) + "_L_" + label;
    row.addElement(labelId + "_lbl",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       float textH  = ImGui::CalcTextSize(label).y;
                       float offset = (r.height - textH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       ImGui::Text("%s", label);
                   });

    auto& containerVBox = getSection(sectionIndex++);
    containerVBox.clear();
    containerVBox.setSpacing(4).setPadding(0, 0, 0, 0);

    float     currentLineW   = 0;
    CLayHBox* currentLineRow = nullptr;
    int       lineCount      = 0;

    for ( size_t i = 0; i < options.size(); ++i ) {
        const auto& [optLabel, optValue] = options[i];
        float itemW = ImGui::CalcTextSize(optLabel.c_str()).x + 36.0f;

        if ( !currentLineRow || (currentLineW + itemW > widgetAvailW) ) {
            currentLineRow = &getRow(rowIndex++);
            currentLineRow->clear();
            currentLineRow->setPadding(0, 0, 0, 0)
                .setSpacing(12)
                .setAlignment(Alignment::Start());

            std::string lineId =
                labelId + "_line_" + std::to_string(lineCount++);
            containerVBox.addLayout(
                lineId.c_str(), *currentLineRow, Sizing::Grow(), Sizing::Fit());
            currentLineW = 0;
        }

        std::string optId = labelId + "_opt_" + std::to_string(i);
        currentLineRow->addElement(
            optId.c_str(),
            Sizing::Fixed(itemW),
            Sizing::Fixed(ImGui::GetFrameHeight()),
            [optLabel = optLabel, optValue = optValue, &current, &changed](
                Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                if ( ImGui::RadioButton(optLabel.c_str(),
                                        current == optValue) ) {
                    current = optValue;
                    changed = true;
                }
            });

        currentLineW += itemW + 12.0f;
    }

    row.addLayout((labelId + "_group").c_str(),
                  containerVBox,
                  Sizing::Grow(),
                  Sizing::Fit());

    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fit());
}

void SettingsView::drawSoftwareSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "SW_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // Clamp TreeNodeEx to Clay bounding box: push zero
                // WindowPadding to eliminate outer_extend, and
                // temporarily set WorkRect.Max.x to match Clay width.
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.software.general").data(),
                               true) ) {
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
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
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
                auto&       skinMgr      = Config::SkinManager::instance();
                auto&       asciiFonts   = skinMgr.getAsciiFonts();
                std::string currentAscii = settings.preferredAsciiFont.empty()
                                               ? "Default"
                                               : settings.preferredAsciiFont;

                std::string label =
                    (currentAscii == "Default")
                        ? TR_CACHE("ui.settings.software.font.default").data()
                        : currentAscii;

                ImGui::SetNextItemWidth(r.width - 40.0f);
                if ( ImGui::BeginCombo("##AsciiFontCombo", label.c_str()) ) {
                    // 1. 默认选项
                    {
                        bool isSelected = (currentAscii == "Default");
                        if ( ImGui::Selectable(
                                 TR_CACHE("ui.settings.software.font.default")
                                     .data(),
                                 isSelected) ) {
                            settings.preferredAsciiFont = "Default";
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                        if ( isSelected ) ImGui::SetItemDefaultFocus();
                    }

                    // 2. 皮肤自带的额外字体
                    for ( const auto& [name, path] : asciiFonts ) {
                        bool        isSelected = (currentAscii == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ImGui::Selectable(lbl.c_str(), isSelected) ) {
                            settings.preferredAsciiFont = name;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if ( ImGui::Button("...##BrowseAscii", { 35, 0 }) ) {
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result =
                            NFD_OpenDialogU8(&outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            settings.preferredAsciiFont = outPath;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        IGFD::FileDialogConfig config;
                        config.path     = ".";
                        config.fileName = "";
                        config.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AsciiFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                    }
                }
            });

        // 5. 字体选择 (CJK)
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.cjk").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                auto&       skinMgr    = Config::SkinManager::instance();
                auto&       cjkFonts   = skinMgr.getCjkFonts();
                std::string currentCjk = settings.preferredCjkFont.empty()
                                             ? "Default"
                                             : settings.preferredCjkFont;
                std::string label =
                    (currentCjk == "Default")
                        ? TR_CACHE("ui.settings.software.font.default").data()
                        : currentCjk;

                ImGui::SetNextItemWidth(r.width - 40.0f);
                if ( ImGui::BeginCombo("##CjkFontCombo", label.c_str()) ) {
                    // 1. 默认选项
                    {
                        bool isSelected = (currentCjk == "Default");
                        if ( ImGui::Selectable(
                                 TR_CACHE("ui.settings.software.font.default")
                                     .data(),
                                 isSelected) ) {
                            settings.preferredCjkFont = "Default";
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                        if ( isSelected ) ImGui::SetItemDefaultFocus();
                    }

                    // 2. 皮肤自带的额外字体
                    for ( const auto& [name, path] : cjkFonts ) {
                        bool        isSelected = (currentCjk == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ImGui::Selectable(lbl.c_str(), isSelected) ) {
                            settings.preferredCjkFont = name;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if ( ImGui::Button("...##BrowseCjk", { 35, 0 }) ) {
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result =
                            NFD_OpenDialogU8(&outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            settings.preferredCjkFont = outPath;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        IGFD::FileDialogConfig config;
                        config.path     = ".";
                        config.fileName = "";
                        config.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "CjkFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                    }
                }
            });

        // 6. 界面全局缩放
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpUIScale = settings.uiScaleMultiplier;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##UIScale", &tmpUIScale, 0.5f, 2.0f, "%.2f");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.uiScaleMultiplier = tmpUIScale;
                    changed                    = true;
                    if ( auto ctx = Graphic::VKContext::get() ) {
                        ctx->get().applyTheme();
                        ctx->get().updateFontScales();
                        ctx->get().requestFontRebuild();
                    }
                } else if ( !ImGui::IsItemActive() ) {
                    tmpUIScale = settings.uiScaleMultiplier;
                }
            });

        // 7. 字体大小倍率
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpFontScale = settings.fontSizeMultiplier;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##FontScale", &tmpFontScale, 0.5f, 2.0f, "%.2f");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.fontSizeMultiplier = tmpFontScale;
                    changed                     = true;
                    if ( auto ctx = Graphic::VKContext::get() ) {
                        ctx->get().updateFontScales();
                        ctx->get().requestFontRebuild();
                    }
                } else if ( !ImGui::IsItemActive() ) {
                    tmpFontScale = settings.fontSizeMultiplier;
                }
            });

        // 处理文件选择器结果 (保持在 Clay 之后，因为它们开启新窗口)
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display("AsciiFontPicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                settings.preferredAsciiFont =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                if ( auto ctx = Graphic::VKContext::get() )
                    ctx->get().requestFontRebuild();
                changed = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }

        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display(
                 "CjkFontPicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                settings.preferredCjkFont =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                if ( auto ctx = Graphic::VKContext::get() )
                    ctx->get().requestFontRebuild();
                changed = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }

    // 2. 光标样式
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.software.cursor_params").data(), true) ) {

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

        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.cursor_software").data(),
                (int)Config::CursorStyle::Software },
              { TR_CACHE("ui.settings.editor.cursor_system").data(),
                (int)Config::CursorStyle::System } },
            (int&)settings.cursorStyle,
            changed);

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
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    changed |= ImGui::Checkbox(
                        "##BpmSync",
                        &settings.softwareCursorConfig.enableBpmSyncSmokeLife);
                });
            addSettingItem(
                *sec,
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

    // 界面美化/审美设置
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.software.aesthetics").data(), true) ) {
        const char* aesLabels[] = {
            TR_CACHE("ui.settings.software.aesthetics.window_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.frame_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_gap").data(),
            TR_CACHE("ui.settings.software.aesthetics.item_spacing").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : aesLabels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_rounding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpRounding = settings.aesthetics.windowRounding;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##WinRounding", &tmpRounding, 0.0f, 32.0f, "%.1f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.windowRounding = tmpRounding;
                    changed                            = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    tmpRounding = settings.aesthetics.windowRounding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.frame_rounding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpFrame = settings.aesthetics.frameRounding;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##FrameRounding", &tmpFrame, 0.0f, 32.0f, "%.1f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.frameRounding = tmpFrame;
                    changed                           = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    tmpFrame = settings.aesthetics.frameRounding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_gap").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpGap = settings.aesthetics.windowGap;
                ImGui::SetNextWindowSizeConstraints(ImVec2(r.width, -1),
                                                    ImVec2(r.width, -1));
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat("##WinGap", &tmpGap, 0.0f, 32.0f, "%.1f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.windowGap = tmpGap;
                    changed                       = true;
                } else if ( !ImGui::IsItemActive() ) {
                    tmpGap = settings.aesthetics.windowGap;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.item_spacing").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpSpacing = settings.aesthetics.itemSpacing;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##ItemSpacing", &tmpSpacing, 0.0f, 32.0f, "%.1f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.itemSpacing = tmpSpacing;
                    changed                         = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    tmpSpacing = settings.aesthetics.itemSpacing;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_padding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpPadding = settings.aesthetics.windowPadding;
                ImGui::SetNextItemWidth(r.width);
                ImGui::SliderFloat(
                    "##WinPadding", &tmpPadding, 0.0f, 32.0f, "%.1f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.windowPadding = tmpPadding;
                    changed                           = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    tmpPadding = settings.aesthetics.windowPadding;
                }
            });
    }

    // 3. 界面偏好 (文件选择器、保存格式等)
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.software.sync").data(), true) ) {

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
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.picker_style").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.picker_unified").data(),
                (int)Config::FilePickerStyle::Unified },
              { TR_CACHE("ui.settings.software.picker_native").data(),
                (int)Config::FilePickerStyle::Native } },
            (int&)settings.filePickerStyle,
            changed);

        // 保存偏好
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.save_format").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.save_format.original").data(),
                (int)Config::SaveFormatPreference::Original },
              { TR_CACHE("ui.settings.software.save_format.force_mmm").data(),
                (int)Config::SaveFormatPreference::ForceMMM } },
            (int&)settings.saveFormatPreference,
            changed);

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
        addSettingItem(
            *sec,
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
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

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
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "VS_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.layout").data(), true) ) {
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
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.layout_box_width").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##LayoutBoxWidth", &visual.trackBoxLineWidth, 1.0f, 10.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.judgeline").data(),
                               true) ) {
        float maxLabelW =
            measureLabelWidth(
                TR_CACHE("ui.settings.visual.judgeline_pos").data()) +
            8.0f;
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.judgeline_pos").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##JudgeLinePos", &visual.judgeline_pos, 0.0f, 1.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.beat_line").data(),
                               true) ) {
        float maxLabelW =
            measureLabelWidth(
                TR_CACHE("ui.settings.visual.beat_line_alpha").data()) +
            8.0f;
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BeatLineAlpha", &visual.beatLineAlpha, 0.0f, 1.0f);
            });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.note").data(), true) ) {
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

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.background").data(),
                               true) ) {
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgOpaque", &visual.background.opaque_ratio, 0.0f, 1.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_darken").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgDarken", &visual.background.darken_ratio, 0.0f, 1.0f);
            });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.preview").data(), true) ) {
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
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##MarginT", &visual.previewConfig.margin.top, 0.0f, 20.0f);
            });
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox("##DrawBeatLines",
                                           &visual.previewConfig.drawBeatLines);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DrawTimingLines", &visual.previewConfig.drawTimingLines);
            });
        addSettingItem(
            *sec,
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
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.visual.timeline_zoom_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
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

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.offset").data(), true) ) {
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
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

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

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "PRJ_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.project.info").data(), true) ) {
        std::string projPath = Config::pathToUtf8(project->m_projectRoot);
        float       labelW =
            measureLabelWidth(TR_CACHE("ui.settings.project.path").data()) + 8;
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.project.path").data(),
                       labelW,
                       [projPath](Clay_BoundingBox r, bool) {
                           float widgetH = ImGui::GetFrameHeight();
                           float offset  = (r.height - widgetH) * 0.5f;
                           ImGui::SetCursorScreenPos({ r.x, r.y + offset });

                           ImGui::SetNextItemWidth(r.width);
                           char buf[1024];
                           snprintf(buf, sizeof(buf), "%s", projPath.c_str());
                           ImGui::InputText("##ProjPath",
                                            buf,
                                            sizeof(buf),
                                            ImGuiInputTextFlags_ReadOnly);
                       });
    }

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });
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

    auto&       beatmap = *session->getContext().currentBeatmap;
    std::string currentPath =
        Config::pathToUtf8(beatmap.m_baseMapMetadata.map_path);
    if ( m_lastBeatmapPath != currentPath ) {
        m_editingMeta     = beatmap.m_baseMapMetadata;
        m_lastBeatmapPath = currentPath;
    }
    auto& meta    = m_editingMeta;
    bool  changed = false;

    bool isImd = false;
    if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
        auto ext = beatmap.m_baseMapMetadata.map_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if ( ext == ".imd" ) {
            isImd = true;
        }
    }

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "MAP_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.beatmap.info").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.beatmap.name").data(),
            TR_CACHE("ui.settings.beatmap.title").data(),
            TR_CACHE("ui.settings.beatmap.title_unicode").data(),
            TR_CACHE("ui.settings.beatmap.artist").data(),
            TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
            TR_CACHE("ui.settings.beatmap.mapper").data(),
            TR_CACHE("ui.settings.beatmap.version").data(),
            TR_CACHE("ui.settings.beatmap.path").data(),
            TR_CACHE("ui.settings.beatmap.stats").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        auto DrawInput = [&](const char*  labelPtr,
                             std::string& valRef,
                             bool         enabled = true) {
            addSettingItem(
                *sec,
                rowIndex,
                labelPtr,
                maxLabelW,
                [labelPtr, &valRef = valRef, &changed, enabled](
                    Clay_BoundingBox r, bool) {
                    ImGui::PushID(labelPtr);
                    if ( !enabled ) {
                        ImGui::BeginDisabled();
                    }
                    char buf[256];
                    strncpy(buf, valRef.c_str(), sizeof(buf));
                    buf[sizeof(buf) - 1] = '\0';
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::InputText("##Input", buf, sizeof(buf)) ) {
                        valRef  = buf;
                        changed = true;
                    }
                    if ( !enabled ) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                });
        };

        DrawInput(
            TR_CACHE("ui.settings.beatmap.name").data(), meta.name, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.title").data(), meta.title, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.title_unicode").data(),
                  meta.title_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.artist").data(), meta.artist, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
                  meta.artist_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.mapper").data(), meta.author, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.version").data(), meta.version, true);

        std::string relativePathStr = "";
        std::string absolutePathStr = "";
        if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
            auto absolutePath =
                std::filesystem::absolute(beatmap.m_baseMapMetadata.map_path);
            absolutePathStr = Config::pathToUtf8(absolutePath);
            if ( project ) {
                try {
                    auto relativePath = std::filesystem::relative(
                        absolutePath, project->m_projectRoot);
                    relativePathStr = Config::pathToUtf8(relativePath);
                } catch ( ... ) {
                    relativePathStr = absolutePathStr;
                }
            } else {
                relativePathStr = absolutePathStr;
            }
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.path").data(),
            maxLabelW,
            [relativePathStr, absolutePathStr](Clay_BoundingBox r, bool) {
                ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                ImVec2 textSize  = ImGui::CalcTextSize(relativePathStr.c_str());

                // 计算滚动位移
                float offset       = 0.0f;
                float visibleWidth = r.width;

                if ( textSize.x > visibleWidth ) {
                    float scrollRange = textSize.x - visibleWidth + 40.0f;
                    float time        = (float)ImGui::GetTime();
                    // 平滑往复滚动，两端停顿
                    float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
                    t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
                    offset  = t * scrollRange;
                }

                // 垂直居中计算
                float textH   = ImGui::GetFontSize();
                float widgetH = ImGui::GetFrameHeight();
                float offsetY = (widgetH - textH) * 0.5f;

                // 应用剪切矩形并绘制文本
                ImGui::PushClipRect(
                    cursorPos,
                    ImVec2(cursorPos.x + r.width, cursorPos.y + widgetH),
                    true);

                // 绘制一个透明的 dummy 控件以接收 hover 状态显示 Tooltip
                ImGui::SetCursorScreenPos(cursorPos);
                ImGui::Dummy(ImVec2(r.width, widgetH));
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", absolutePathStr.c_str());
                }

                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    relativePathStr.c_str());

                ImGui::PopClipRect();
            });

        // 物件统计
        size_t totalPlayableNotes = 0;
        size_t normalNotes        = 0;
        size_t holds              = 0;
        size_t flicks             = 0;
        size_t polylinesCount     = 0;

        auto noteView =
            session->getContext().noteRegistry.view<Logic::NoteComponent>();
        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<Logic::NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                polylinesCount++;
                for ( const auto& sub : nc.m_subNotes ) {
                    if ( sub.type == ::MMM::NoteType::NOTE )
                        normalNotes++;
                    else if ( sub.type == ::MMM::NoteType::HOLD )
                        holds++;
                    else if ( sub.type == ::MMM::NoteType::FLICK )
                        flicks++;
                }
            } else if ( !nc.m_isSubNote ) {
                if ( nc.m_type == ::MMM::NoteType::NOTE )
                    normalNotes++;
                else if ( nc.m_type == ::MMM::NoteType::HOLD )
                    holds++;
                else if ( nc.m_type == ::MMM::NoteType::FLICK )
                    flicks++;
            }
        }
        totalPlayableNotes = normalNotes + holds + flicks;

        std::string statsStr = TR_FMT("ui.settings.beatmap.stats_format",
                                      totalPlayableNotes,
                                      normalNotes,
                                      holds,
                                      flicks);

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.stats").data(),
                       maxLabelW,
                       [statsStr](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           ImGui::TextUnformatted(statsStr.c_str());
                       });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.cover_type").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            TR_CACHE("ui.settings.beatmap.video_start").data(),
            TR_CACHE("ui.settings.beatmap.bg_offset").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        if ( isImd ) {
            ImGui::BeginDisabled();
        }

        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.beatmap.cover_type.image").data(), 0 },
              { TR_CACHE("ui.settings.beatmap.cover_type.video").data(), 1 } },
            (int&)meta.cover_type,
            changed);

        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.beatmap.video_start").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               if ( ImGui::InputInt("##VideoStart",
                                                    &meta.video_starttime) ) {
                                   changed = true;
                               }
                           });
        }

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.bg_offset").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           int offsets[2] = { meta.bgxoffset, meta.bgyoffset };
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::DragInt2("##BgOffset", offsets) ) {
                               meta.bgxoffset = offsets[0];
                               meta.bgyoffset = offsets[1];
                               changed        = true;
                           }
                       });

        if ( isImd ) {
            ImGui::EndDisabled();
        }
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.preference").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            TR_CACHE("ui.settings.beatmap.length").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                if ( isImd ) {
                    ImGui::BeginDisabled();
                }
                float bpm = (float)meta.preference_bpm;
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::DragFloat(
                         "##BPM", &bpm, 0.1f, -1.0f, 1000.0f, "%.2f") ) {
                    meta.preference_bpm = (double)bpm;
                    changed             = true;
                }
                if ( isImd ) {
                    ImGui::EndDisabled();
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputInt("##Tracks", &meta.track_count) ) {
                    changed = true;
                }
            });

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.length").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::BeginDisabled();
                           double length = meta.map_length;
                           ImGui::SetNextItemWidth(r.width);
                           ImGui::InputDouble(
                               "##Length", &length, 0, 0, "%.3f s");
                           ImGui::EndDisabled();
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.beatmap.resource").data(),
                               true) ) {
        const char* labels[] = { TR_CACHE("ui.settings.beatmap.audio").data(),
                                 TR_CACHE("ui.settings.beatmap.cover").data() };
        float       maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        if ( isImd ) {
            ImGui::BeginDisabled();
        }

        // 音频选择
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.audio").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                std::string currentAudioPath =
                    Config::pathToUtf8(meta.main_audio_path);
                std::string audioPreview = currentAudioPath;
                if ( project && !audioPreview.empty() ) {
                    if ( meta.main_audio_path.is_absolute() ) {
                        try {
                            audioPreview =
                                Config::pathToUtf8(std::filesystem::relative(
                                    meta.main_audio_path,
                                    project->m_projectRoot));
                        } catch ( ... ) {
                        }
                    }
                }

                bool audioExists =
                    project && std::filesystem::exists(project->m_projectRoot /
                                                       meta.main_audio_path);
                bool audioPushed = false;
                if ( !audioExists && !currentAudioPath.empty() ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    audioPushed = true;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::BeginCombo("##AudioCombo", audioPreview.c_str()) ) {
                    if ( audioPushed ) {
                        ImGui::PopStyleColor();
                        audioPushed = false;
                    }
                    if ( project ) {
                        for ( const auto& res : project->m_audioResources ) {
                            if ( res.m_type != MMM::AudioTrackType::Main )
                                continue;
                            bool isSelected = (currentAudioPath == res.m_path);
                            if ( ImGui::Selectable(
                                     (res.m_id + "##" + res.m_path).c_str(),
                                     isSelected) ) {
                                meta.main_audio_path = res.m_path;
                                changed              = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                if ( audioPushed ) ImGui::PopStyleColor();
            });

        // 封面选择
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.cover").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                std::string currentCoverPath =
                    Config::pathToUtf8(meta.main_cover_path);
                std::string coverPreview = currentCoverPath;
                if ( project && !coverPreview.empty() ) {
                    if ( meta.main_cover_path.is_absolute() ) {
                        try {
                            coverPreview =
                                Config::pathToUtf8(std::filesystem::relative(
                                    meta.main_cover_path,
                                    project->m_projectRoot));
                        } catch ( ... ) {
                        }
                    }
                }

                bool coverExists =
                    project && std::filesystem::exists(project->m_projectRoot /
                                                       meta.main_cover_path);
                bool coverPushed = false;
                if ( !coverExists && !currentCoverPath.empty() ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    coverPushed = true;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::BeginCombo("##CoverCombo", coverPreview.c_str()) ) {
                    if ( coverPushed ) {
                        ImGui::PopStyleColor();
                        coverPushed = false;
                    }
                    if ( project ) {
                        std::vector<std::string> images;
                        try {
                            for ( const auto& entry :
                                  std::filesystem::recursive_directory_iterator(
                                      project->m_projectRoot) ) {
                                if ( entry.is_regular_file() ) {
                                    auto ext = Config::pathToUtf8(
                                        entry.path().extension());
                                    std::transform(ext.begin(),
                                                   ext.end(),
                                                   ext.begin(),
                                                   ::tolower);
                                    if ( ext == ".png" || ext == ".jpg" ||
                                         ext == ".jpeg" || ext == ".bmp" ||
                                         ext == ".mp4" || ext == ".avi" ) {
                                        images.push_back(Config::pathToUtf8(
                                            std::filesystem::relative(
                                                entry.path(),
                                                project->m_projectRoot)));
                                    }
                                }
                            }
                        } catch ( ... ) {
                        }

                        for ( const auto& imgPath : images ) {
                            bool isSelected = (currentCoverPath == imgPath);
                            if ( ImGui::Selectable(
                                     (imgPath + "##" + imgPath).c_str(),
                                     isSelected) ) {
                                meta.main_cover_path = imgPath;
                                changed              = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                if ( coverPushed ) ImGui::PopStyleColor();
            });

        if ( isImd ) {
            ImGui::EndDisabled();
        }
    }

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
    }
}

void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });

                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.behavior").data(), true) ) {
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DisableAccel",
                    &settings.disableScrollAccelerationWhileDrawing);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##ScrollMul",
                                              &settings.scrollSpeedMultiplier,
                                              1.0f,
                                              10.0f,
                                              "%.1f");
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.editor.scroll_multiplier_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
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
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.editor.beat_divisor_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.selection").data(),
                               true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.editor.selection").data(),
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            TR_CACHE("ui.settings.editor.selection.rounding").data()
        };
        float maxLabelW = 0;
        for ( auto* l : labels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.selection").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.selection.strict").data(),
                (int)Config::SelectionMode::Strict },
              { TR_CACHE("ui.settings.editor.selection.intersection").data(),
                (int)Config::SelectionMode::Intersection } },
            (int&)settings.selectionMode,
            changed);
        addSettingItem(
            *sec,
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

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.sfx").data(), true) ) {
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##FlickScale",
                    &settings.sfxConfig.enableFlickWidthVolumeScaling);
            });
        if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
            addSettingItem(
                *sec,
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
        addSettingItem(
            *sec,
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
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI