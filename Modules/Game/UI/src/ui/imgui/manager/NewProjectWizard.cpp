#include "ui/imgui/manager/NewProjectWizard.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectStorage.h"
#include "ui/UIManager.h"
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

/// @brief 尽量将路径转换为绝对规范路径。
/// @param path 待规范化路径。
/// @return 成功时返回绝对路径；失败时退回词法规范化路径。
std::filesystem::path makeAbsoluteNormalizedPath(
    const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    std::error_code filesystemError;
    auto normalized = std::filesystem::absolute(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    return path.lexically_normal();
}

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

/// @brief 如果路径存在且是目录，则返回可传给文件选择器的 UTF-8 路径。
/// @param path 待检查路径。
/// @return 有效目录的 UTF-8 文本，无效时返回空字符串。
std::string existingDirectoryPathToUtf8(const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    std::error_code filesystemError;
    const auto absolutePath = std::filesystem::absolute(path, filesystemError);
    if ( filesystemError ) {
        return {};
    }

    if ( !std::filesystem::exists(absolutePath, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(absolutePath, filesystemError) ||
         filesystemError ) {
        return {};
    }
    auto pickerPath =
        std::filesystem::weakly_canonical(absolutePath, filesystemError);
    if ( filesystemError ) {
        pickerPath = absolutePath;
        filesystemError.clear();
    }
    pickerPath.make_preferred();
    return Config::pathToUtf8(pickerPath);
}

/// @brief 打开原生父目录选择器，默认路径不被系统接受时自动无默认路径重试。
/// @param outPath 原生文件选择器输出路径。
/// @param defaultPath 原生文件选择器初始目录。
/// @return 原生文件选择器结果。
nfdresult_t pickNativeParentFolder(nfdu8char_t**      outPath,
                                   const std::string& defaultPath)
{
    const nfdu8char_t* defaultPathPtr =
        defaultPath.empty() ? nullptr : defaultPath.c_str();
    nfdresult_t result = NFD_PickFolderU8(outPath, defaultPathPtr);
    if ( result != NFD_ERROR || defaultPathPtr == nullptr ) {
        return result;
    }

    const char* errorText = NFD_GetError();
    XWARN("Native folder picker rejected default path [{}]: {}",
          defaultPath,
          errorText ? errorText : "");
    if ( outPath && *outPath ) {
        NFD_FreePathU8(*outPath);
        *outPath = nullptr;
    }
    return NFD_PickFolderU8(outPath, nullptr);
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
    return makeAbsoluteNormalizedPath(m_parentDirectory /
                                      Config::utf8ToPath(m_folderNameBuf));
}

bool NewProjectWizard::targetHasProjectFile() const
{
    const auto targetPath = targetProjectPath();
    if ( targetPath.empty() ) return false;

    return Logic::ProjectStorage::hasProjectConfiguration(targetPath);
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
    if ( renderLabeledInputText(TR("ui.settings.project.name").data(),
                                "##NewProjectTitle",
                                m_titleBuf,
                                sizeof(m_titleBuf)) ) {
        refreshFolderNameFromTitle();
    }
    renderLabeledInputText(TR("ui.settings.project.artist").data(),
                           "##NewProjectArtist",
                           m_artistBuf,
                           sizeof(m_artistBuf));
    renderLabeledInputText(TR("ui.settings.project.mapper").data(),
                           "##NewProjectMapper",
                           m_mapperBuf,
                           sizeof(m_mapperBuf));
}

void NewProjectWizard::renderPreferencesStep()
{
    ImGui::SeparatorText(TR("ui.wizard.new_project.preferences").data());

    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;

    std::string previewName;
    if ( m_colorPaletteSchemeName.empty() ) {
        previewName = TR("ui.settings.project.note_palette.inherit").data();
    } else if ( m_colorPaletteSchemeName ==
                Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        previewName = TR("ui.toolbar.note_palette.skin_default_scheme").data();
    } else {
        previewName = m_colorPaletteSchemeName;
    }

    ImGui::TextUnformatted(TR("ui.settings.project.note_palette").data());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewProjectNotePalette",
                                       previewName.c_str()) ) {
        const bool inheritSelected = m_colorPaletteSchemeName.empty();
        if ( ::MMM::UI::FeedbackSelectable(
                 TR("ui.settings.project.note_palette.inherit").data(),
                 inheritSelected) ) {
            m_colorPaletteSchemeName.clear();
        }
        if ( inheritSelected ) ImGui::SetItemDefaultFocus();

        const bool skinSelected = m_colorPaletteSchemeName ==
                                  Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        if ( ::MMM::UI::FeedbackSelectable(
                 TR("ui.toolbar.note_palette.skin_default_scheme").data(),
                 skinSelected) ) {
            m_colorPaletteSchemeName =
                Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        }
        if ( skinSelected ) ImGui::SetItemDefaultFocus();

        for ( const auto& scheme : paletteConfig.schemes ) {
            const bool selected = m_colorPaletteSchemeName == scheme.name;
            if ( ::MMM::UI::FeedbackSelectable(scheme.name.c_str(),
                                               selected) ) {
                m_colorPaletteSchemeName = scheme.name;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        ::MMM::UI::FeedbackEndCombo();
    }

    ImGui::TextUnformatted(TR("ui.wizard.new_project.initial_sidebar").data());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewProjectInitialSidebar",
                                       sidebarTabLabel(m_initialSideBarTab)) ) {
        const SideBarTab tabs[] = { SideBarTab::FileExplorer,
                                    SideBarTab::BeatMapExplorer,
                                    SideBarTab::AudioExplorer,
                                    SideBarTab::Search,
                                    SideBarTab::None };
        for ( SideBarTab tab : tabs ) {
            const bool selected = m_initialSideBarTab == tab;
            if ( ::MMM::UI::FeedbackSelectable(sidebarTabLabel(tab),
                                               selected) ) {
                m_initialSideBarTab = tab;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        ::MMM::UI::FeedbackEndCombo();
    }
}

void NewProjectWizard::renderLocationStep()
{
    ImGui::SeparatorText(TR("ui.wizard.new_project.location").data());

    if ( renderLabeledInputText(TR("ui.wizard.new_project.folder_name").data(),
                                "##NewProjectFolderName",
                                m_folderNameBuf,
                                sizeof(m_folderNameBuf)) ) {
        m_folderNameEdited = true;
    }

    const float buttonWidth = std::floor(
        160.0f * Config::AppConfig::instance().getWindowContentScale());
    if ( ::MMM::UI::FeedbackButton(
             TR("ui.wizard.new_project.select_parent").data(),
             ImVec2(buttonWidth, 0.0f)) ) {
        requestParentFolderPicker();
    }

    if ( !m_locationErrorText.empty() ) {
        ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                           "%s",
                           m_locationErrorText.c_str());
    }

    const std::string parentText =
        m_parentDirectory.empty()
            ? TR("ui.wizard.new_project.parent.none").toString()
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

bool NewProjectWizard::renderLabeledInputText(const char* label, const char* id,
                                              char*       buffer,
                                              std::size_t bufferSize)
{
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputText(id, buffer, bufferSize);
}

void NewProjectWizard::renderFooter()
{
    ImGui::Separator();

    const bool  suppressActions = shouldSuppressFooterActionsThisFrame();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float available   = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = std::floor(
        std::min(150.0f * dpiScale, (available - spacing * 2.0f) / 3.0f));
    const ImVec2 buttonSize{ std::max(96.0f * dpiScale, buttonWidth), 0.0f };
    ImGui::BeginDisabled(suppressActions || m_currentStep == Step::ProjectInfo);
    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_project.back").data(),
                                   buttonSize) ) {
        if ( m_currentStep == Step::Preferences ) {
            m_currentStep = Step::ProjectInfo;
        } else if ( m_currentStep == Step::Location ) {
            m_currentStep = Step::Preferences;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(suppressActions);
    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_beatmap.cancel").data(),
                                   buttonSize) ) {
        close();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(suppressActions || !canAdvance());
    const bool isLastStep = m_currentStep == Step::Location;
    if ( ::MMM::UI::FeedbackButton(
             isLastStep ? TR("ui.wizard.new_project.create").data()
                        : TR("ui.wizard.new_project.next").data(),
             buttonSize) ) {
        if ( m_currentStep == Step::ProjectInfo ) {
            m_currentStep = Step::Preferences;
        } else if ( m_currentStep == Step::Preferences ) {
            m_currentStep = Step::Location;
        } else {
            submitCreateRequest();
        }
    }
    ImGui::EndDisabled();

    if ( suppressActions && m_suppressFooterActionFrames > 0 ) {
        --m_suppressFooterActionFrames;
    }
}

void NewProjectWizard::requestParentFolderPicker()
{
    m_pendingParentFolderPicker  = true;
    m_suppressFooterActionFrames = 12;
}

void NewProjectWizard::processPendingParentFolderPicker()
{
    if ( !m_pendingParentFolderPicker ) {
        return;
    }

    m_pendingParentFolderPicker = false;
    auto& config = Config::AppConfig::instance().getEditorSettings();
    m_locationErrorText.clear();
    std::string defaultPath = existingDirectoryPathToUtf8(m_parentDirectory);
    if ( defaultPath.empty() && !config.lastFilePickerPath.empty() ) {
        defaultPath = existingDirectoryPathToUtf8(
            Config::utf8ToPath(config.lastFilePickerPath));
    }

    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath = nullptr;
        const nfdresult_t result =
            pickNativeParentFolder(&outPath, defaultPath);
        m_isOpen                     = true;
        m_shouldOpen                 = true;
        m_suppressFooterActionFrames = 12;
        if ( result == NFD_OKAY ) {
            m_parentDirectory = Config::utf8ToPath(outPath);
            m_locationErrorText.clear();
            auto editorConfig =
                Logic::EditorEngine::instance().getEditorConfig();
            editorConfig.settings.lastFilePickerPath = outPath;
            Logic::EditorEngine::instance().setEditorConfig(editorConfig);
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_ERROR ) {
            const char* errorText = NFD_GetError();
            XWARN(
                "Native folder picker failed, falling back to unified "
                "picker: {}",
                errorText ? errorText : "");
            m_locationErrorText.clear();
            openUnifiedParentFolderPicker(defaultPath);
        }
        return;
    }

    openUnifiedParentFolderPicker(defaultPath);
}

void NewProjectWizard::openUnifiedParentFolderPicker(
    const std::string& initialPath)
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path = initialPath.empty() ? std::string(".") : initialPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                 ImGuiFileDialogFlags_HideColumnType |
                                 ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID);
    ImGuiFileDialog::Instance()->OpenDialog(
        PARENT_FOLDER_PICKER_ID,
        TR("ui.wizard.new_project.select_parent").data(),
        nullptr,
        fdConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
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
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             ImVec2(600.0f * dpiScale, 400.0f * dpiScale)) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string folderPath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            if ( folderPath.empty() ) {
                folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            m_parentDirectory = Config::utf8ToPath(folderPath);
            m_locationErrorText.clear();

            auto config = Logic::EditorEngine::instance().getEditorConfig();
            config.settings.lastFilePickerPath =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            Logic::EditorEngine::instance().setEditorConfig(config);
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

bool NewProjectWizard::shouldSuppressFooterActionsThisFrame() const
{
    return m_suppressFooterActionFrames > 0;
}

void NewProjectWizard::submitCreateRequest()
{
    if ( !hasValidTargetPath() ) return;

    Event::ProjectCreateRequestedEvent event;
    event.m_projectPath            = targetProjectPath();
    event.m_title                  = m_titleBuf;
    event.m_artist                 = m_artistBuf;
    event.m_mapper                 = m_mapperBuf;
    event.m_colorPaletteSchemeName = m_colorPaletteSchemeName;
    event.m_sidebarActiveTab =
        SideBarUI::workspaceNameFromTab(m_initialSideBarTab);
    Event::EventBus::instance().publish(event);
    close();
}

/// @brief 更新并渲染新建项目向导窗口。
/// @param sourceManager 当前 UI 管理器，保留用于接口一致性。
/// @warning UI
/// 热路径：每帧仅在向导打开时渲染；除用户点击浏览目录触发文件选择器外
/// 禁止加入阻塞操作。
void NewProjectWizard::update(UIManager* sourceManager)
{
    (void)sourceManager;
    if ( !m_isOpen ) {
        return;
    }

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const std::string windowTitle =
        TR("ui.wizard.new_project.title").toString() +
        "###NewProjectWizardWindow";
    if ( m_shouldOpen ) {
        ::MMM::UI::FeedbackOpenPopup(windowTitle.c_str());
        m_shouldOpen = false;
    }

    Utils::CenteredModalPopupScope modalScope(dpiScale);
    constexpr ImGuiWindowFlags     WINDOW_FLAGS = ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoDocking;
    const bool                     windowVisible =
        modalScope.begin(windowTitle.c_str(),
                         &m_isOpen,
                         WINDOW_FLAGS,
                         ImVec2(640.0f * dpiScale, 460.0f * dpiScale),
                         false);
    if ( windowVisible ) {
        renderStepHeader();

        const float footerReserve = ImGui::GetFrameHeightWithSpacing() +
                                    ImGui::GetStyle().ItemSpacing.y * 3.0f;
        const float contentHeight =
            std::max(120.0f * dpiScale,
                     ImGui::GetContentRegionAvail().y - footerReserve);
        {
            Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);
            ImGui::BeginChild("##NewProjectWizardContent",
                              ImVec2(0.0f, contentHeight),
                              false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            switch ( m_currentStep ) {
            case Step::ProjectInfo: renderProjectInfoStep(); break;
            case Step::Preferences: renderPreferencesStep(); break;
            case Step::Location: renderLocationStep(); break;
            }
            ImGui::EndChild();
        }

        renderFooter();

        processPendingParentFolderPicker();
        renderParentFolderPicker(dpiScale);
        ImGui::EndPopup();
    }
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
    const auto& settings = Config::AppConfig::instance().getEditorSettings();

    m_currentStep      = Step::ProjectInfo;
    m_folderNameEdited = false;
    copyToBuffer(m_titleBuf,
                 sizeof(m_titleBuf),
                 TR("ui.wizard.new_project.default_title").data());
    copyToBuffer(m_artistBuf, sizeof(m_artistBuf), "Unknown");
    copyToBuffer(
        m_mapperBuf,
        sizeof(m_mapperBuf),
        settings.defaultCreator.empty() ? "Unknown" : settings.defaultCreator);
    refreshFolderNameFromTitle();
    m_locationErrorText.clear();
    m_suppressFooterActionFrames = 0;
    m_pendingParentFolderPicker  = false;

    m_colorPaletteSchemeName.clear();
    m_initialSideBarTab = SideBarTab::FileExplorer;

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
