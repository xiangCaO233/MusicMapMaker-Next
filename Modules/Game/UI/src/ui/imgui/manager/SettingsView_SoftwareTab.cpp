#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/FontPreferenceValidator.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>
#include <nfd.h>
#include <system_error>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 默认皮肤目录名。
constexpr const char* kDefaultSkinDirectoryName = "mmm-default";

/// @brief 获取指定皮肤目录的入口脚本路径。
/// @param skinDirectoryName skins 根目录下的皮肤目录名。
/// @return 皮肤入口脚本完整路径。
std::filesystem::path skinLuaPathForDirectory(
    const std::string& skinDirectoryName)
{
    std::filesystem::path path = Config::AppPaths::skinsRootPath();
    path /= Config::utf8ToPath(skinDirectoryName);
    path /= "skin.lua";
    return path;
}

/// @brief 获取当前实际加载的皮肤目录名。
/// @param settings 编辑器设置。
/// @return 当前皮肤目录名。
std::string currentSkinDirectoryName(const Config::EditorSettings& settings)
{
    auto loadedName = Config::pathToUtf8(
        Config::SkinManager::instance().getData().skinPath.filename());
    if ( !loadedName.empty() ) {
        return loadedName;
    }
    if ( !settings.selectedSkinDirectory.empty() ) {
        return settings.selectedSkinDirectory;
    }
    return kDefaultSkinDirectoryName;
}

/// @brief 检查皮肤入口脚本是否存在。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 文件存在且是普通文件时返回 true。
bool skinLuaFileExists(const std::filesystem::path& skinLuaPath)
{
    std::error_code ec;
    return std::filesystem::exists(skinLuaPath, ec) &&
           std::filesystem::is_regular_file(skinLuaPath, ec);
}

/// @brief 按当前皮肤配置预加载所有皮肤音效。
/// @warning 低频资源重载路径：皮肤热切换后调用，会触发音频资源加载。
void preloadCurrentSkinSoundEffects()
{
    const auto& skinData = Config::SkinManager::instance().getData();
    for ( const auto& [key, path] : skinData.audioPaths ) {
        const auto   leadInIt = skinData.audioLeadInSeconds.find(key);
        const double leadInSeconds =
            leadInIt != skinData.audioLeadInSeconds.end() ? leadInIt->second
                                                          : 0.0;
        Audio::AudioManager::instance().preloadSoundEffect(
            key, Config::pathToUtf8(path), 1.0f, leadInSeconds);
    }
}
}  // namespace

/// @brief 刷新可选皮肤目录名缓存。
/// @warning 低频文件系统路径：只在设置窗口打开或缓存标脏时扫描
/// AppPaths::skinsRootPath()，禁止每帧无条件调用。
void SettingsView::refreshAvailableSkinDirectories()
{
    m_availableSkinDirectories.clear();

    std::error_code ec;
    const auto      skinsRoot = Config::AppPaths::skinsRootPath();
    if ( !std::filesystem::exists(skinsRoot, ec) ||
         !std::filesystem::is_directory(skinsRoot, ec) ) {
        m_availableSkinDirectoriesDirty = false;
        return;
    }

    std::filesystem::directory_iterator it(
        skinsRoot,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator end;
    while ( it != end ) {
        std::error_code itemEc;
        if ( it->is_directory(itemEc) ) {
            const auto skinLuaPath = it->path() / "skin.lua";
            if ( skinLuaFileExists(skinLuaPath) ) {
                m_availableSkinDirectories.push_back(
                    Config::pathToUtf8(it->path().filename()));
            }
        }
        it.increment(ec);
        if ( ec ) {
            ec.clear();
        }
    }

    std::sort(m_availableSkinDirectories.begin(),
              m_availableSkinDirectories.end());
    std::stable_sort(
        m_availableSkinDirectories.begin(),
        m_availableSkinDirectories.end(),
        [](const std::string& lhs, const std::string& rhs) {
            if ( lhs == rhs ) return false;
            const bool lhsDefault = lhs == kDefaultSkinDirectoryName;
            const bool rhsDefault = rhs == kDefaultSkinDirectoryName;
            if ( lhsDefault != rhsDefault ) return lhsDefault;
            return lhs < rhs;
        });
    m_availableSkinDirectories.erase(
        std::unique(m_availableSkinDirectories.begin(),
                    m_availableSkinDirectories.end()),
        m_availableSkinDirectories.end());
    m_availableSkinDirectoriesDirty = false;
}

/// @brief 应用皮肤选择并请求图形/音频资源热重载。
/// @param skinDirectoryName skins 根目录下的皮肤目录名。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 切换成功时返回 true。
/// @warning 低频资源重载路径：会加载 Lua、清理音效池并请求 Vulkan
/// 资源重建，只能由设置页皮肤选择触发。
bool SettingsView::applySkinSelection(const std::string& skinDirectoryName,
                                      const std::filesystem::path& skinLuaPath)
{
    if ( skinDirectoryName.empty() || !skinLuaFileExists(skinLuaPath) ) {
        if ( auto ctx = Graphic::VKContext::get() ) {
            ctx->get().showCenterNotification("皮肤入口不存在");
        }
        return false;
    }

    if ( !Config::SkinManager::instance().loadSkin(
             Config::pathToUtf8(skinLuaPath)) ) {
        if ( auto ctx = Graphic::VKContext::get() ) {
            ctx->get().showCenterNotification("皮肤加载失败");
        }
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( Config::resetUnavailableFontPreferences(
             settings, Config::SkinManager::instance()) ) {
        XWARN("Unavailable font preference reset after skin switch");
    }
    settings.selectedSkinDirectory = skinDirectoryName;
    m_layoutMetricsCache.valid     = false;
    m_hasPreparedLayoutMetrics     = false;

    if ( m_sourceManager ) {
        if ( auto* mainDock = m_sourceManager->getView<MainDockSpaceUI>(
                 "MainDockSpaceUI") ) {
            mainDock->refreshPaletteAfterSkinChange();
        }
    }

    auto& audio = Audio::AudioManager::instance();
    audio.clearSoundEffects();
    preloadCurrentSkinSoundEffects();
    Logic::EditorEngine::instance().registerCurrentProjectEffectSoundEffects();
    if ( m_sourceManager ) {
        m_sourceManager->reloadOpenEffectAudioTracks();
    }

    if ( auto ctx = Graphic::VKContext::get() ) {
        ctx->get().applyTheme();
        ctx->get().requestFontRebuild();
        ctx->get().showCenterNotification("皮肤已切换: " + skinDirectoryName);
    }

    if ( m_sourceManager ) {
        m_sourceManager->requestSkinResourceReload();
    }
    return true;
}

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

                // 将 TreeNodeEx 约束到 Clay 边界：临时将 WindowPadding 设为 0
                // 以消除外扩，并把 WorkRect.Max.x 调整到 Clay 宽度。
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
                if ( ::MMM::UI::FeedbackCombo("##LangCombo",
                                              &currentLang,
                                              langs,
                                              IM_ARRAYSIZE(langs)) ) {
                    settings.language = langIDs[currentLang];
                    Config::SkinManager::instance().getTranslator().switchLang(
                        settings.language);
                    changed = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.default_creator").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputTextWithHint(
                         "##DefaultCreator",
                         TR_CACHE("ui.settings.software.default_creator.hint")
                             .data(),
                         m_defaultCreatorInputBuffer.data(),
                         m_defaultCreatorInputBuffer.size()) ) {
                    settings.defaultCreator = Config::normalizeCreatorIdentity(
                        m_defaultCreatorInputBuffer.data());
                    changed = true;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    refreshDefaultCreatorInputBuffer();
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
                if ( ::MMM::UI::FeedbackCombo("##FrameLimitCombo",
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
                if ( ::MMM::UI::FeedbackCheckbox(
                         "##AutoUploadPgoProfiles",
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
                if ( ::MMM::UI::FeedbackCombo("##AudioBackendCombo",
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
                    if ( ::MMM::UI::FeedbackCheckbox(
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
                            ::MMM::UI::FeedbackSliderFloat(
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
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_y").data(),
                    "##OpenALDirY",
                    &settings.openALSpatialConfig.directionY,
                    -1.0f,
                    1.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_z").data(),
                    "##OpenALDirZ",
                    &settings.openALSpatialConfig.directionZ,
                    -1.0f,
                    1.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_distance").data(),
                    "##OpenALDistance",
                    &settings.openALSpatialConfig.distance,
                    0.0f,
                    100.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_reference_distance")
                        .data(),
                    "##OpenALReferenceDistance",
                    &settings.openALSpatialConfig.referenceDistance,
                    0.01f,
                    100.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_max_distance").data(),
                    "##OpenALMaxDistance",
                    &settings.openALSpatialConfig.maxDistance,
                    0.01f,
                    1000.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_rolloff").data(),
                    "##OpenALRolloff",
                    &settings.openALSpatialConfig.rolloffFactor,
                    0.0f,
                    10.0f,
                    "%.4f");
            }
        }

        if ( m_availableSkinDirectoriesDirty ) {
            refreshAvailableSkinDirectories();
        }
        const std::string activeSkinDirectory =
            currentSkinDirectoryName(settings);
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.skin").data(),
            maxLabelW,
            [this, &changed, activeSkinDirectory](Clay_BoundingBox r, bool) {
                const ImGuiStyle& style         = ImGui::GetStyle();
                constexpr float   actionButtonW = 35.0f;
                const float       comboWidth =
                    std::max(1.0f,
                             r.width - actionButtonW * 3.0f -
                                 style.ItemSpacing.x * 3.0f);
                ImGui::SetNextItemWidth(comboWidth);
                if ( m_availableSkinDirectories.empty() ) {
                    ImGui::BeginDisabled();
                    if ( ::MMM::UI::FeedbackBeginCombo(
                             "##SkinCombo",
                             TR_CACHE("ui.settings.software.skin.none")
                                 .data()) ) {
                        ::MMM::UI::FeedbackEndCombo();
                    }
                    ImGui::EndDisabled();
                } else if ( ::MMM::UI::FeedbackBeginCombo(
                                "##SkinCombo", activeSkinDirectory.c_str()) ) {
                    for ( const auto& directoryName :
                          m_availableSkinDirectories ) {
                        const bool selected =
                            directoryName == activeSkinDirectory;
                        if ( ::MMM::UI::FeedbackSelectable(
                                 directoryName.c_str(), selected) ) {
                            if ( directoryName != activeSkinDirectory &&
                                 applySkinSelection(
                                     directoryName,
                                     skinLuaPathForDirectory(directoryName)) ) {
                                changed = true;
                            }
                        }
                        if ( selected ) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }

                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));

                ImGui::PushID("OpenSkinDirectory");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_FOLDER_OPEN,
                                               { actionButtonW, 0.0f }) ) {
                    openSkinDirectory();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.open_directory")
                        .data());
                ImGui::PopID();

                ImGui::SameLine();
                ImGui::PushID("ImportSkinPackage");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_DOWNLOAD,
                                               { actionButtonW, 0.0f }) ) {
                    changed |= openSkinImportFilePicker();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.import").data());
                ImGui::PopID();

                ImGui::SameLine();
                ImGui::PushID("ExportSkinPackage");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_PACK,
                                               { actionButtonW, 0.0f }) ) {
                    openSkinExportFilePicker();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.export").data());
                ImGui::PopID();

                ImGui::PopStyleVar(2);
            });

        // 4. UI 主题
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.theme").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                auto context = Graphic::VKContext::get();
                const Graphic::ImGuiThemeRegistry* themeRegistry =
                    context ? &context->get().getThemeRegistry() : nullptr;
                const Graphic::ImGuiTheme* selectedTheme =
                    themeRegistry ? themeRegistry->findTheme(settings.theme)
                                  : nullptr;
                const char* preview =
                    settings.theme == Config::UI_THEME_AUTO_ID
                        ? TR_CACHE("ui.settings.software.theme.auto").data()
                    : selectedTheme ? selectedTheme->displayName().data()
                                    : settings.theme.c_str();

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##ThemeCombo", preview) ) {
                    const bool automaticSelected =
                        settings.theme == Config::UI_THEME_AUTO_ID;
                    if ( ::MMM::UI::FeedbackSelectable(
                             TR_CACHE("ui.settings.software.theme.auto").data(),
                             automaticSelected) ) {
                        settings.theme = Config::UI_THEME_AUTO_ID;
                        if ( context ) {
                            context->get().applyTheme();
                        }
                        changed = true;
                    }
                    if ( automaticSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }

                    if ( themeRegistry ) {
                        for ( const auto& theme : themeRegistry->themes() ) {
                            if ( !theme ) continue;
                            const bool selected = settings.theme == theme->id();
                            ImGui::PushID(theme->id().data());
                            if ( ::MMM::UI::FeedbackSelectable(
                                     theme->displayName().data(), selected) ) {
                                settings.theme = theme->id();
                                context->get().applyTheme();
                                changed = true;
                            }
                            ImGui::PopID();
                            if ( selected ) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
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
                if ( ::MMM::UI::FeedbackBeginCombo("##AsciiFontCombo",
                                                   label.c_str()) ) {
                    // 1. 默认选项
                    {
                        bool isSelected = (currentAscii == "Default");
                        if ( ::MMM::UI::FeedbackSelectable(
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
                        if ( ::MMM::UI::FeedbackSelectable(lbl.c_str(),
                                                           isSelected) ) {
                            settings.preferredAsciiFont = name;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseAsciiClicked = ::MMM::UI::FeedbackButton(
                    "...##BrowseAscii", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseAsciiClicked ) {
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        ::MMM::UI::PlayPopupOpenFeedback();
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result = NativeFileDialog::openFile(
                            &outPath, filters, 1, nullptr);

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
                        const bool wasOpen =
                            ImGuiFileDialog::Instance()->IsOpened(
                                "AsciiFontPicker");
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AsciiFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                        if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                                             "AsciiFontPicker") ) {
                            ::MMM::UI::PlayPopupOpenFeedback();
                        }
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
                if ( ::MMM::UI::FeedbackBeginCombo("##CjkFontCombo",
                                                   label.c_str()) ) {
                    // 1. 默认选项
                    {
                        bool isSelected = (currentCjk == "Default");
                        if ( ::MMM::UI::FeedbackSelectable(
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
                        if ( ::MMM::UI::FeedbackSelectable(lbl.c_str(),
                                                           isSelected) ) {
                            settings.preferredCjkFont = name;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseCjkClicked = ::MMM::UI::FeedbackButton(
                    "...##BrowseCjk", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseCjkClicked ) {
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        ::MMM::UI::PlayPopupOpenFeedback();
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result = NativeFileDialog::openFile(
                            &outPath, filters, 1, nullptr);

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
                        const bool wasOpen =
                            ImGuiFileDialog::Instance()->IsOpened(
                                "CjkFontPicker");
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "CjkFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                        if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                                             "CjkFontPicker") ) {
                            ::MMM::UI::PlayPopupOpenFeedback();
                        }
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##UIScale", &tmpUIScale, 0.5f, 2.0f, "%.4f");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##FontScale", &tmpFontScale, 0.5f, 2.0f, "%.4f");
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
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##CursorSize",
                                   &settings.softwareCursorConfig.cursorSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##TrailSize",
                                   &settings.softwareCursorConfig.trailSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_life").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##TrailLife",
                                   &settings.softwareCursorConfig.trailLifeTime,
                                   0.05f,
                                   5.0f,
                                   "%.4f s");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.smoke_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##SmokeSize",
                                   &settings.softwareCursorConfig.smokeSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
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
                    changed |= ::MMM::UI::FeedbackSliderFloat(
                        "##SmokeLife",
                        &settings.softwareCursorConfig.smokeLifeTime,
                        0.05f,
                        10.0f,
                        "%.4f s");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinRounding", &tmpRounding, 0.0f, 32.0f, "%.4f px");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##FrameRounding", &tmpFrame, 0.0f, 32.0f, "%.4f px");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinGap", &tmpGap, 0.0f, 32.0f, "%.4f px");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##ItemSpacing", &tmpSpacing, 0.0f, 32.0f, "%.4f px");
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
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinPadding", &tmpPadding, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.windowPadding = tmpPadding;
                    changed                           = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    tmpPadding = settings.aesthetics.windowPadding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.animation_transition")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                static float tmpDuration =
                    settings.aesthetics.animationTransitionDuration;
                constexpr float maxDuration = 1.0f;
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##AnimationTransitionDuration",
                         &tmpDuration,
                         0.005f,
                         Config::UIAestheticsConfig::
                             MIN_ANIMATION_TRANSITION_DURATION,
                         maxDuration,
                         "%.2f s",
                         ImGuiSliderFlags_AlwaysClamp) ) {
                    tmpDuration =
                        std::clamp(tmpDuration,
                                   Config::UIAestheticsConfig::
                                       MIN_ANIMATION_TRANSITION_DURATION,
                                   maxDuration);
                    settings.aesthetics.animationTransitionDuration =
                        tmpDuration;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    settings.aesthetics.animationTransitionDuration =
                        tmpDuration;
                    changed = true;
                } else if ( !ImGui::IsItemActive() ) {
                    tmpDuration =
                        settings.aesthetics.animationTransitionDuration;
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

        auto& autoSaveGroup =
            addSettingGroup(*sec, sectionIndex, "AutoSaveSettingsGroup");
        addRadioSetting(
            autoSaveGroup,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.auto_save.mode").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.auto_save.mode.disabled").data(),
                (int)Config::AutoSaveMode::Disabled },
              { TR_CACHE("ui.settings.software.auto_save.mode.timed").data(),
                (int)Config::AutoSaveMode::Timed },
              { TR_CACHE("ui.settings.software.auto_save.mode.event").data(),
                (int)Config::AutoSaveMode::EventTriggered } },
            (int&)settings.autoSave.mode,
            changed,
            false);

        if ( settings.autoSave.mode == Config::AutoSaveMode::Timed ) {
            addRadioSetting(
                autoSaveGroup,
                rowIndex,
                sectionIndex,
                TR_CACHE("ui.settings.software.auto_save.interval_unit").data(),
                maxLabelW,
                { { TR_CACHE(
                        "ui.settings.software.auto_save.interval_unit.seconds")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Seconds },
                  { TR_CACHE(
                        "ui.settings.software.auto_save.interval_unit.minutes")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Minutes } },
                (int&)settings.autoSave.intervalUnit,
                changed,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.interval").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoSaveInterval",
                        &settings.autoSave.intervalValue,
                        5,
                        60);
                },
                false,
                false);
        } else if ( settings.autoSave.mode ==
                    Config::AutoSaveMode::EventTriggered ) {
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_object_modified")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveObjectModified",
                        &settings.autoSave.onObjectModified);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_beatmap_switch")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveBeatmapSwitch",
                        &settings.autoSave.onBeatmapSwitch);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_imgui_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveImGuiFocusLost",
                        &settings.autoSave.onImGuiWindowFocusLost);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_native_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveNativeFocusLost",
                        &settings.autoSave.onNativeWindowFocusLost);
                },
                false,
                false);
        }

        auto& autoBackupGroup =
            addSettingGroup(*sec, sectionIndex, "AutoBackupSettingsGroup");
        addRadioSetting(
            autoBackupGroup,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.auto_backup.mode").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.auto_backup.mode.disabled")
                    .data(),
                (int)Config::AutoSaveMode::Disabled },
              { TR_CACHE("ui.settings.software.auto_backup.mode.timed").data(),
                (int)Config::AutoSaveMode::Timed },
              { TR_CACHE("ui.settings.software.auto_backup.mode.event").data(),
                (int)Config::AutoSaveMode::EventTriggered } },
            (int&)settings.autoBackup.mode,
            changed,
            false);

        if ( settings.autoBackup.mode == Config::AutoSaveMode::Timed ) {
            addRadioSetting(
                autoBackupGroup,
                rowIndex,
                sectionIndex,
                TR_CACHE("ui.settings.software.auto_backup.interval_unit")
                    .data(),
                maxLabelW,
                { { TR_CACHE("ui.settings.software.auto_backup.interval_unit."
                             "seconds")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Seconds },
                  { TR_CACHE("ui.settings.software.auto_backup.interval_unit."
                             "minutes")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Minutes } },
                (int&)settings.autoBackup.intervalUnit,
                changed,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.interval").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoBackupInterval",
                        &settings.autoBackup.intervalValue,
                        5,
                        60);
                },
                false,
                false);
        } else if ( settings.autoBackup.mode ==
                    Config::AutoSaveMode::EventTriggered ) {
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_object_modified")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupObjectModified",
                        &settings.autoBackup.onObjectModified);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_beatmap_switch")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupBeatmapSwitch",
                        &settings.autoBackup.onBeatmapSwitch);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_imgui_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupImGuiFocusLost",
                        &settings.autoBackup.onImGuiWindowFocusLost);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE(
                    "ui.settings.software.auto_backup.on_native_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupNativeFocusLost",
                        &settings.autoBackup.onNativeWindowFocusLost);
                },
                false,
                false);
        }

        if ( settings.autoBackup.mode != Config::AutoSaveMode::Disabled ) {
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.max_count").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoBackupMaxCount",
                        &settings.autoBackup.maxBackupCount,
                        Config::AUTO_BACKUP_COUNT_MIN,
                        Config::AUTO_BACKUP_COUNT_MAX);
                },
                false,
                false);
        }

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
                if ( ::MMM::UI::FeedbackCombo("##TimeFormat",
                                              &timeFormat,
                                              timeFormats,
                                              IM_ARRAYSIZE(timeFormats)) ) {
                    settings.timeFormatPreference =
                        (Config::TimeFormatPreference)timeFormat;
                    changed = true;
                }
            });

        // 最近项目上限
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.recent_limit").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackSliderInt("##RecentLimit",
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
                if ( ::MMM::UI::FeedbackCombo("##SyncMode",
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
                               changed |= ::MMM::UI::FeedbackSliderFloat(
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
                               changed |= ::MMM::UI::FeedbackSliderFloat(
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
                           changed |= ::MMM::UI::FeedbackDragScalar(
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

    changed |= renderSkinPackageFileDialogs(
        Config::AppConfig::instance().getWindowContentScale());

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
