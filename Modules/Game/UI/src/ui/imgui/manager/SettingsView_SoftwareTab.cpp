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
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 渲染软件设置页。
void SettingsView::drawSoftwareSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

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
        // 采用全局统一最大标签宽度 maxLabelW

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

        // 2. 帧数限制
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.framelimit").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         limit    = (int)settings.frameLimit;
                const char* limits[] = {
                    TR_CACHE("ui.settings.software.framelimit.vsync").data(),
                    TR_CACHE("ui.settings.software.framelimit.2x").data(),
                    TR_CACHE("ui.settings.software.framelimit.4x").data(),
                    TR_CACHE("ui.settings.software.framelimit.8x").data(),
                    TR_CACHE("ui.settings.software.framelimit.unlimited").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##FrameLimitCombo",
                                  &limit,
                                  limits,
                                  IM_ARRAYSIZE(limits)) ) {
                    settings.frameLimit = (Config::FrameLimitPreference)limit;
                    changed             = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.auto_upload_pgo_profiles").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                if ( ImGui::Checkbox("##AutoUploadPgoProfiles",
                                     &settings.autoUploadPgoProfiles) ) {
                    settings.pgoProfileUploadConsentAsked = true;
                    changed                               = true;
                }
            });

        // 3. 音频播放后端
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.audio_backend").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int backend = settings.audioPlaybackBackend ==
                                      Config::AudioPlaybackBackend::OpenAL
                                  ? 1
                                  : 0;
                const char* backends[] = {
                    TR_CACHE("ui.settings.software.audio_backend.sdl").data(),
                    TR_CACHE("ui.settings.software.audio_backend.openal").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##AudioBackendCombo",
                                  &backend,
                                  backends,
                                  IM_ARRAYSIZE(backends)) ) {
                    auto target = backend == 1
                                      ? Config::AudioPlaybackBackend::OpenAL
                                      : Config::AudioPlaybackBackend::SDL;
                    if ( Audio::AudioManager::instance().setPlaybackBackend(
                             target) ) {
                        settings.audioPlaybackBackend = target;
                        changed                       = true;
                    }
                }
            });

        if ( settings.audioPlaybackBackend ==
             Config::AudioPlaybackBackend::OpenAL ) {
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.openal_spatial").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos(
                        { r.x,
                          r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                    if ( ImGui::Checkbox(
                             "##OpenALSpatial",
                             &settings.openALSpatialConfig.enabled) ) {
                        Audio::AudioManager::instance().setOpenALSpatialConfig(
                            settings.openALSpatialConfig);
                        changed = true;
                    }
                });

            if ( settings.openALSpatialConfig.enabled ) {
                auto addSpatialSlider = [&](const char* label,
                                            const char* id,
                                            float*      value,
                                            float       minValue,
                                            float       maxValue,
                                            const char* format) {
                    addSettingItem(
                        *sec,
                        rowIndex,
                        label,
                        maxLabelW,
                        [&, id, value, minValue, maxValue, format](
                            Clay_BoundingBox r, bool) {
                            ImGui::SetNextItemWidth(r.width);
                            ImGui::SliderFloat(
                                id, value, minValue, maxValue, format);
                            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                                Audio::AudioManager::instance()
                                    .setOpenALSpatialConfig(
                                        settings.openALSpatialConfig);
                                changed = true;
                            }
                        });
                };

                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_x").data(),
                    "##OpenALDirX",
                    &settings.openALSpatialConfig.directionX,
                    -1.0f,
                    1.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_y").data(),
                    "##OpenALDirY",
                    &settings.openALSpatialConfig.directionY,
                    -1.0f,
                    1.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_z").data(),
                    "##OpenALDirZ",
                    &settings.openALSpatialConfig.directionZ,
                    -1.0f,
                    1.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_distance").data(),
                    "##OpenALDistance",
                    &settings.openALSpatialConfig.distance,
                    0.0f,
                    100.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_reference_distance")
                        .data(),
                    "##OpenALReferenceDistance",
                    &settings.openALSpatialConfig.referenceDistance,
                    0.01f,
                    100.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_max_distance").data(),
                    "##OpenALMaxDistance",
                    &settings.openALSpatialConfig.maxDistance,
                    0.01f,
                    1000.0f,
                    "%.2f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_rolloff").data(),
                    "##OpenALRolloff",
                    &settings.openALSpatialConfig.rolloffFactor,
                    0.0f,
                    10.0f,
                    "%.2f");
            }
        }

        // 4. UI 主题
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
                    "Cecilia",
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

                const ImGuiStyle& style         = ImGui::GetStyle();
                const float       browseButtonW = 35.0f;
                const float       comboW        = std::max(
                    1.0f, r.width - browseButtonW - style.ItemSpacing.x);
                ImGui::SetNextItemWidth(comboW);
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
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseAsciiClicked =
                    ImGui::Button("...##BrowseAscii", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseAsciiClicked ) {
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

                const ImGuiStyle& style         = ImGui::GetStyle();
                const float       browseButtonW = 35.0f;
                const float       comboW        = std::max(
                    1.0f, r.width - browseButtonW - style.ItemSpacing.x);
                ImGui::SetNextItemWidth(comboW);
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
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseCjkClicked =
                    ImGui::Button("...##BrowseCjk", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseCjkClicked ) {
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
                        ctx->get().showCenterNotification(
                            TR_CACHE("ui.settings.software.font.restart")
                                .data());
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
                        ctx->get().showCenterNotification(
                            TR_CACHE("ui.settings.software.font.restart")
                                .data());
                    }
                } else if ( !ImGui::IsItemActive() ) {
                    tmpFontScale = settings.fontSizeMultiplier;
                }
            });

        // 处理文件选择器结果 (保持在 Clay 之后，因为它们开启新窗口)
        {
            const float dpiScale =
                Config::AppConfig::instance().getWindowContentScale();
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AsciiFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
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
        }

        {
            const float dpiScale =
                Config::AppConfig::instance().getWindowContentScale();
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "CjkFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
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
    }

    // 2. 光标样式
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.software.cursor_params").data(), true) ) {

        // 采用全局统一最大标签宽度 maxLabelW

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
        // 采用全局统一最大标签宽度 maxLabelW

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

        // 采用全局统一最大标签宽度 maxLabelW

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

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.time_format").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         timeFormat    = (int)settings.timeFormatPreference;
                const char* timeFormats[] = {
                    TR_CACHE("ui.settings.software.time_format.clock").data(),
                    TR_CACHE("ui.settings.software.time_format.seconds").data(),
                    TR_CACHE("ui.settings.software.time_format.milliseconds")
                        .data(),
                    TR_CACHE("ui.settings.software.time_format.beat").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##TimeFormat",
                                  &timeFormat,
                                  timeFormats,
                                  IM_ARRAYSIZE(timeFormats)) ) {
                    settings.timeFormatPreference =
                        (Config::TimeFormatPreference)timeFormat;
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
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.sync_mode").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         syncMode    = (int)settings.syncConfig.mode;
                const char* syncModes[] = {
                    TR_CACHE("ui.settings.software.sync_mode.none").data(),
                    TR_CACHE("ui.settings.software.sync_mode.integral").data(),
                    TR_CACHE("ui.settings.software.sync_mode.watertank").data()
                };
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

}  // namespace MMM::UI
