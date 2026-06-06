#include "ui/imgui/manager/NewProjectWizard.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <nfd.h>
#include <string>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 统一文件选择器弹窗 ID。
constexpr const char* PARENT_FOLDER_PICKER_ID = "NewProjectParentFolderPicker";

/// @brief 判断字符是否适合保留在文件夹名中。
/// @param ch 待检查字符。
/// @return 字符可保留时返回 true。
bool isSafeFolderNameChar(unsigned char ch)
{
    if ( ch < 32 ) return false;
    switch ( ch ) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|': return false;
    default: return true;
    }
}

/// @brief 从项目标题生成适合文件夹使用的名称。
/// @param title 项目标题。
/// @return 清理后的文件夹名。
std::string makeFolderNameFromTitle(std::string_view title)
{
    std::string result;
    result.reserve(title.size());
    bool lastWasSpace = false;
    for ( unsigned char ch : title ) {
        if ( std::isspace(ch) ) {
            if ( !result.empty() && !lastWasSpace ) {
                result.push_back('_');
                lastWasSpace = true;
            }
            continue;
        }

        if ( !isSafeFolderNameChar(ch) ) {
            if ( !result.empty() && !lastWasSpace ) {
                result.push_back('_');
                lastWasSpace = true;
            }
            continue;
        }

        result.push_back(static_cast<char>(ch));
        lastWasSpace = false;
    }

    while ( !result.empty() && (result.back() == '_' || result.back() == '.' ||
                                result.back() == ' ') ) {
        result.pop_back();
    }
    if ( result.empty() ) {
        result = "New_Project";
    }
    return result;
}

/// @brief 获取侧边栏页签显示文本。
/// @param tab 侧边栏页签。
/// @return 用户界面显示文本。
const char* sidebarTabLabel(SideBarTab tab)
{
    switch ( tab ) {
    case SideBarTab::FileExplorer:
        return TR("ui.wizard.new_project.sidebar.file").data();
    case SideBarTab::BeatMapExplorer:
        return TR("ui.wizard.new_project.sidebar.beatmap").data();
    case SideBarTab::AudioExplorer:
        return TR("ui.wizard.new_project.sidebar.audio").data();
    case SideBarTab::Search:
        return TR("ui.wizard.new_project.sidebar.search").data();
    case SideBarTab::None:
    case SideBarTab::Settings:
    default: return TR("ui.wizard.new_project.sidebar.none").data();
    }
}

}  // namespace

NewProjectWizard::NewProjectWizard() : IUIView("NewProjectWizard")
{
    reset();
}

void NewProjectWizard::copyToBuffer(char* buffer, std::size_t bufferSize,
                                    std::string_view value)
{
    if ( bufferSize == 0 ) return;

    const std::size_t copySize = std::min(bufferSize - 1, value.size());
    std::memcpy(buffer, value.data(), copySize);
    buffer[copySize] = '\0';
}

void NewProjectWizard::refreshFolderNameFromTitle()
{
    if ( m_folderNameEdited ) return;
    copyToBuffer(m_folderNameBuf,
                 sizeof(m_folderNameBuf),
                 makeFolderNameFromTitle(m_titleBuf));
}

std::filesystem::path NewProjectWizard::targetProjectPath() const
{
    if ( m_parentDirectory.empty() || m_folderNameBuf[0] == '\0' ) {
        return {};
    }
    return m_parentDirectory / Config::utf8ToPath(m_folderNameBuf);
}

bool NewProjectWizard::targetHasProjectFile() const
{
    const auto targetPath = targetProjectPath();
    if ( targetPath.empty() ) return false;

    std::error_code filesystemError;
    return std::filesystem::exists(targetPath / "mmm_project.json",
                                   filesystemError) &&
           !filesystemError;
}

bool NewProjectWizard::hasValidTargetPath() const
{
    if ( m_parentDirectory.empty() || m_folderNameBuf[0] == '\0' ) {
        return false;
    }

    std::error_code filesystemError;
    if ( !std::filesystem::exists(m_parentDirectory, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(m_parentDirectory, filesystemError) ||
         filesystemError ) {
        return false;
    }

    const auto targetPath = targetProjectPath();
    if ( targetPath.empty() || targetHasProjectFile() ) {
        return false;
    }

    if ( std::filesystem::exists(targetPath, filesystemError) ) {
        if ( filesystemError ) return false;
        return std::filesystem::is_directory(targetPath, filesystemError) &&
               !filesystemError;
    }
    return true;
}

bool NewProjectWizard::canAdvance() const
{
    switch ( m_currentStep ) {
    case Step::ProjectInfo: return m_titleBuf[0] != '\0';
    case Step::Preferences: return true;
    case Step::Location: return hasValidTargetPath();
    }
    return false;
}

void NewProjectWizard::renderStepHeader() const
{
    const char* label = TR("ui.wizard.new_project.step.info").data();
    int         index = 0;
    switch ( m_currentStep ) {
    case Step::ProjectInfo:
        label = TR("ui.wizard.new_project.step.info").data();
        index = 0;
        break;
    case Step::Preferences:
        label = TR("ui.wizard.new_project.step.preferences").data();
        index = 1;
        break;
    case Step::Location:
        label = TR("ui.wizard.new_project.step.location").data();
        index = 2;
        break;
    }

    ImGui::Text("%s  %d / 3", label, index + 1);
    ImGui::Separator();
}

void NewProjectWizard::renderProjectInfoStep()
{
    ImGui::SeparatorText(TR("ui.settings.project.info").data());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ImGui::InputText(TR("ui.settings.project.name").data(),
                          m_titleBuf,
                          sizeof(m_titleBuf)) ) {
        refreshFolderNameFromTitle();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(TR("ui.settings.project.artist").data(),
                     m_artistBuf,
                     sizeof(m_artistBuf));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(TR("ui.settings.project.mapper").data(),
                     m_mapperBuf,
                     sizeof(m_mapperBuf));
}

void NewProjectWizard::renderPreferencesStep()
{
    ImGui::SeparatorText(TR("ui.wizard.new_project.preferences").data());

    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().noteColorPalettes;

    std::string previewName;
    if ( m_noteColorPaletteSchemeName.empty() ) {
        previewName = TR("ui.settings.project.note_palette.inherit").data();
    } else if ( m_noteColorPaletteSchemeName ==
                Config::NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        previewName = TR("ui.toolbar.note_palette.skin_default_scheme").data();
    } else {
        previewName = m_noteColorPaletteSchemeName;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ImGui::BeginCombo(TR("ui.settings.project.note_palette").data(),
                           previewName.c_str()) ) {
        const bool inheritSelected = m_noteColorPaletteSchemeName.empty();
        if ( ImGui::Selectable(
                 TR("ui.settings.project.note_palette.inherit").data(),
                 inheritSelected) ) {
            m_noteColorPaletteSchemeName.clear();
        }
        if ( inheritSelected ) ImGui::SetItemDefaultFocus();

        const bool skinSelected =
            m_noteColorPaletteSchemeName ==
            Config::NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        if ( ImGui::Selectable(
                 TR("ui.toolbar.note_palette.skin_default_scheme").data(),
                 skinSelected) ) {
            m_noteColorPaletteSchemeName =
                Config::NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        }
        if ( skinSelected ) ImGui::SetItemDefaultFocus();

        for ( const auto& scheme : paletteConfig.schemes ) {
            const bool selected = m_noteColorPaletteSchemeName == scheme.name;
            if ( ImGui::Selectable(scheme.name.c_str(), selected) ) {
                m_noteColorPaletteSchemeName = scheme.name;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ImGui::BeginCombo(TR("ui.wizard.new_project.initial_sidebar").data(),
                           sidebarTabLabel(m_initialSideBarTab)) ) {
        const SideBarTab tabs[] = { SideBarTab::FileExplorer,
                                    SideBarTab::BeatMapExplorer,
                                    SideBarTab::AudioExplorer,
                                    SideBarTab::Search,
                                    SideBarTab::None };
        for ( SideBarTab tab : tabs ) {
            const bool selected = m_initialSideBarTab == tab;
            if ( ImGui::Selectable(sidebarTabLabel(tab), selected) ) {
                m_initialSideBarTab = tab;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void NewProjectWizard::renderLocationStep()
{
    ImGui::SeparatorText(TR("ui.wizard.new_project.location").data());

    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ImGui::InputText(TR("ui.wizard.new_project.folder_name").data(),
                          m_folderNameBuf,
                          sizeof(m_folderNameBuf)) ) {
        m_folderNameEdited = true;
    }

    const float buttonWidth = std::floor(
        160.0f * Config::AppConfig::instance().getWindowContentScale());
    if ( ImGui::Button(TR("ui.wizard.new_project.select_parent").data(),
                       ImVec2(buttonWidth, 0.0f)) ) {
        openParentFolderPicker();
    }

    const std::string parentText =
        m_parentDirectory.empty()
            ? std::string(TR("ui.wizard.new_project.parent.none").data())
            : Config::pathToUtf8(m_parentDirectory);
    ImGui::TextUnformatted(TR("ui.wizard.new_project.parent").data());
    ImGui::Indent();
    ImGui::TextWrapped("%s", parentText.c_str());
    ImGui::Unindent();

    const auto        targetPath = targetProjectPath();
    const std::string targetText =
        targetPath.empty() ? std::string("-") : Config::pathToUtf8(targetPath);
    ImGui::TextUnformatted(TR("ui.wizard.new_project.target").data());
    ImGui::Indent();
    ImGui::TextWrapped("%s", targetText.c_str());
    ImGui::Unindent();

    if ( targetHasProjectFile() ) {
        const ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol,
            "%s",
            TR("ui.wizard.new_project.folder_has_project").data());
    } else if ( !hasValidTargetPath() ) {
        ImGui::TextDisabled(
            "%s", TR("ui.wizard.new_project.location_required").data());
    }
}

void NewProjectWizard::renderFooter()
{
    ImGui::Separator();

    const float buttonWidth = std::floor(
        110.0f * Config::AppConfig::instance().getWindowContentScale());
    ImGui::BeginDisabled(m_currentStep == Step::ProjectInfo);
    if ( ImGui::Button(TR("ui.wizard.new_project.back").data(),
                       ImVec2(buttonWidth, 0.0f)) ) {
        if ( m_currentStep == Step::Preferences ) {
            m_currentStep = Step::ProjectInfo;
        } else if ( m_currentStep == Step::Location ) {
            m_currentStep = Step::Preferences;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                       ImVec2(buttonWidth, 0.0f)) ) {
        close();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!canAdvance());
    const bool isLastStep = m_currentStep == Step::Location;
    if ( ImGui::Button(isLastStep ? TR("ui.wizard.new_project.create").data()
                                  : TR("ui.wizard.new_project.next").data(),
                       ImVec2(buttonWidth, 0.0f)) ) {
        if ( m_currentStep == Step::ProjectInfo ) {
            m_currentStep = Step::Preferences;
        } else if ( m_currentStep == Step::Preferences ) {
            m_currentStep = Step::Location;
        } else {
            submitCreateRequest();
        }
    }
    ImGui::EndDisabled();
}

void NewProjectWizard::openParentFolderPicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*      outPath     = nullptr;
        const char*       defaultPath = config.lastFilePickerPath.empty()
                                            ? nullptr
                                            : config.lastFilePickerPath.c_str();
        const nfdresult_t result      = NFD_PickFolder(&outPath, defaultPath);
        if ( result == NFD_OKAY ) {
            m_parentDirectory = Config::utf8ToPath(outPath);
            auto config = Logic::EditorEngine::instance().getEditorConfig();
            config.settings.lastFilePickerPath = outPath;
            Logic::EditorEngine::instance().setEditorConfig(config);
            NFD_FreePath(outPath);
        } else if ( result == NFD_ERROR ) {
            XERROR("NFD Error: {}", NFD_GetError());
        }
        return;
    }

    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = config.lastFilePickerPath.empty()
                                     ? std::string(".")
                                     : config.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                 ImGuiFileDialogFlags_HideColumnType |
                                 ImGuiFileDialogFlags_ReadOnlyFileNameField;
    ImGuiFileDialog::Instance()->OpenDialog(
        PARENT_FOLDER_PICKER_ID,
        TR("ui.wizard.new_project.select_parent").data(),
        nullptr,
        fdConfig);
}

void NewProjectWizard::renderParentFolderPicker(float dpiScale)
{
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        Utils::prepareCenteredModalWindow(
            ImVec2(600.0f * dpiScale, 400.0f * dpiScale));
    }
    if ( ImGuiFileDialog::Instance()->Display(
             PARENT_FOLDER_PICKER_ID,
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings,
             ImVec2(600.0f * dpiScale, 400.0f * dpiScale)) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string folderPath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            if ( folderPath.empty() ) {
                folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            m_parentDirectory = Config::utf8ToPath(folderPath);

            auto config = Logic::EditorEngine::instance().getEditorConfig();
            config.settings.lastFilePickerPath =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            Logic::EditorEngine::instance().setEditorConfig(config);
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

void NewProjectWizard::submitCreateRequest()
{
    if ( !hasValidTargetPath() ) return;

    Event::ProjectCreateRequestedEvent event;
    event.m_projectPath                = targetProjectPath();
    event.m_title                      = m_titleBuf;
    event.m_artist                     = m_artistBuf;
    event.m_mapper                     = m_mapperBuf;
    event.m_noteColorPaletteSchemeName = m_noteColorPaletteSchemeName;
    event.m_sidebarActiveTab =
        SideBarUI::workspaceNameFromTab(m_initialSideBarTab);
    Event::EventBus::instance().publish(event);
    close();
}

void NewProjectWizard::update(UIManager* sourceManager)
{
    (void)sourceManager;
    if ( !m_isOpen ) {
        return;
    }

    if ( m_shouldOpen ) {
        ImGui::OpenPopup(TR("ui.wizard.new_project.title").data());
        m_shouldOpen = false;
    }

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    constexpr ImGuiWindowFlags     WINDOW_FLAGS = ImGuiWindowFlags_NoResize;
    if ( modalScope.begin(TR("ui.wizard.new_project.title").data(),
                          &m_isOpen,
                          WINDOW_FLAGS,
                          ImVec2(560.0f * dpiScale, 420.0f * dpiScale),
                          false) ) {
        renderStepHeader();

        switch ( m_currentStep ) {
        case Step::ProjectInfo: renderProjectInfoStep(); break;
        case Step::Preferences: renderPreferencesStep(); break;
        case Step::Location: renderLocationStep(); break;
        }

        renderFooter();
        ImGui::EndPopup();
    }

    renderParentFolderPicker(dpiScale);
}

void NewProjectWizard::open()
{
    m_isOpen     = true;
    m_shouldOpen = true;
    reset();
}

void NewProjectWizard::close()
{
    m_isOpen = false;
    if ( ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        ImGuiFileDialog::Instance()->Close();
    }
    ImGui::CloseCurrentPopup();
}

void NewProjectWizard::reset()
{
    m_currentStep      = Step::ProjectInfo;
    m_folderNameEdited = false;
    copyToBuffer(m_titleBuf,
                 sizeof(m_titleBuf),
                 TR("ui.wizard.new_project.default_title").data());
    copyToBuffer(m_artistBuf, sizeof(m_artistBuf), "Unknown");
    copyToBuffer(m_mapperBuf, sizeof(m_mapperBuf), "Unknown");
    refreshFolderNameFromTitle();

    m_noteColorPaletteSchemeName.clear();
    m_initialSideBarTab = SideBarTab::FileExplorer;

    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( !settings.lastFilePickerPath.empty() ) {
        m_parentDirectory = Config::utf8ToPath(settings.lastFilePickerPath);
    } else {
        std::error_code filesystemError;
        m_parentDirectory = std::filesystem::current_path(filesystemError);
        if ( filesystemError ) {
            m_parentDirectory.clear();
        }
    }
}

}  // namespace MMM::UI
