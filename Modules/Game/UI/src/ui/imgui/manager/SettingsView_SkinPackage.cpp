#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/SkinPackageService.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include "ui/utils/DesktopPathUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <nfd.h>
#include <string>
#include <system_error>

namespace MMM::UI
{

namespace
{

/// @brief 统一文件选择器的 MSK 导入窗口标识。
constexpr const char* kSkinImportDialogId = "SkinPackageImportPicker";

/// @brief 统一文件选择器的 MSK 导出窗口标识。
constexpr const char* kSkinExportDialogId = "SkinPackageExportPicker";

/// @brief 显示设置页中央通知。
void showSkinNotification(const std::string& message)
{
    if ( auto context = Graphic::VKContext::get() ) {
        context->get().showCenterNotification(message);
    }
}

/// @brief 获取文件选择器默认目录。
std::string skinPickerDefaultPath()
{
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    return settings.lastFilePickerPath.empty()
               ? Config::pathToUtf8(Config::AppPaths::skinsRootPath())
               : settings.lastFilePickerPath;
}

/// @brief 记住用户最近一次选择的文件所在目录。
void rememberSkinPickerDirectory(const std::filesystem::path& filePath)
{
    const auto parentPath = filePath.parent_path();
    if ( parentPath.empty() ) return;

    auto& app                                  = Config::AppConfig::instance();
    app.getEditorSettings().lastFilePickerPath = Config::pathToUtf8(parentPath);
    app.save();
}

/// @brief 生成适合文件选择器使用的当前皮肤包文件名。
std::string currentSkinPackageFileName()
{
    std::string name = Config::pathToUtf8(
        Config::SkinManager::instance().getData().skinPath.filename());
    for ( char& character : name ) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ( byte < 32 || character == '<' || character == '>' ||
             character == ':' || character == '"' || character == '/' ||
             character == '\\' || character == '|' || character == '?' ||
             character == '*' ) {
            character = '_';
        }
    }
    while ( !name.empty() && (name.back() == ' ' || name.back() == '.') ) {
        name.pop_back();
    }
    if ( name.empty() || name == "." || name == ".." ) {
        name = "skin";
    }
    return name + ".msk";
}

/// @brief 将输出路径扩展名统一为 .msk。
std::filesystem::path normalizeSkinPackageOutputPath(
    const std::filesystem::path& outputPath)
{
    if ( outputPath.empty() ) return {};
    auto normalized = outputPath;
    normalized.replace_extension(".msk");
    return normalized;
}

/// @brief 判断设置使用系统原生文件选择器。
bool useNativeSkinFilePicker()
{
    return Config::AppConfig::instance().getEditorSettings().filePickerStyle ==
           Config::FilePickerStyle::Native;
}

}  // namespace

void SettingsView::openSkinDirectory()
{
    const auto      skinsRoot = Config::AppPaths::skinsRootPath();
    std::error_code createError;
    std::filesystem::create_directories(skinsRoot, createError);
    if ( createError ||
         !DesktopPathUtils::openInFileManager(skinsRoot, false) ) {
        XERROR("Failed to open skins directory '{}': {}",
               Config::pathToUtf8(skinsRoot),
               createError ? createError.message()
                           : "desktop file manager launch failed");
        showSkinNotification(std::string(
            TR_CACHE("ui.settings.software.skin.open_directory_failed")
                .data()));
    }
}

bool SettingsView::openSkinImportFilePicker()
{
    const std::string defaultPath = skinPickerDefaultPath();
    if ( useNativeSkinFilePicker() ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "MusicMapMaker Skin Package",
                                             "msk" } };
        const nfdresult_t result =
            NFD_OpenDialogU8(&selectedPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && selectedPath != nullptr ) {
            const bool changed =
                importSkinPackage(Config::utf8ToPath(selectedPath));
            NFD_FreePathU8(selectedPath);
            return changed;
        }
        if ( result == NFD_ERROR ) {
            const char* error = NFD_GetError();
            XERROR("Failed to open skin import dialog: {}",
                   error ? error : "Unknown NFD error");
            showSkinNotification(std::string(
                TR_CACHE("ui.settings.software.skin.import_failed").data()));
        }
        return false;
    }

    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags             = ImGuiFileDialogFlags_Modal |
                                     ImGuiFileDialogFlags_HideColumnType |
                                     ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(kSkinImportDialogId);
    ImGuiFileDialog::Instance()->OpenDialog(
        kSkinImportDialogId,
        TR_CACHE("ui.settings.software.skin.import_dialog_title").data(),
        ".msk",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened(kSkinImportDialogId) ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
    return false;
}

void SettingsView::openSkinExportFilePicker()
{
    const std::string defaultPath     = skinPickerDefaultPath();
    const std::string defaultFileName = currentSkinPackageFileName();
    if ( useNativeSkinFilePicker() ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "MusicMapMaker Skin Package",
                                             "msk" } };
        const nfdresult_t result = NFD_SaveDialogU8(&selectedPath,
                                                    filters,
                                                    1,
                                                    defaultPath.c_str(),
                                                    defaultFileName.c_str());
        if ( result == NFD_OKAY && selectedPath != nullptr ) {
            exportCurrentSkinPackage(Config::utf8ToPath(selectedPath));
            NFD_FreePathU8(selectedPath);
        } else if ( result == NFD_ERROR ) {
            const char* error = NFD_GetError();
            XERROR("Failed to open skin export dialog: {}",
                   error ? error : "Unknown NFD error");
            showSkinNotification(std::string(
                TR_CACHE("ui.settings.software.skin.export_failed").data()));
        }
        return;
    }

    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.fileName          = defaultFileName;
    dialogConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(kSkinExportDialogId);
    ImGuiFileDialog::Instance()->OpenDialog(
        kSkinExportDialogId,
        TR_CACHE("ui.settings.software.skin.export_dialog_title").data(),
        ".msk",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened(kSkinExportDialogId) ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

bool SettingsView::renderSkinPackageFileDialogs(float dpiScale)
{
    bool changed = false;
    {
        Utils::CenteredModalPopupScope dialogStyle(dpiScale);
        if ( ImGuiFileDialog::Instance()->IsOpened(kSkinImportDialogId) ) {
            Utils::prepareCenteredModalWindow({ 600, 400 });
        }
        if ( ImGuiFileDialog::Instance()->Display(
                 kSkinImportDialogId,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings,
                 { 600, 400 }) ) {
            std::filesystem::path selectedPath;
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                selectedPath = Config::utf8ToPath(
                    ImGuiFileDialog::Instance()->GetFilePathName());
            }
            ImGuiFileDialog::Instance()->Close();
            if ( !selectedPath.empty() ) {
                changed = importSkinPackage(selectedPath);
            }
        }
    }

    {
        Utils::CenteredModalPopupScope dialogStyle(dpiScale);
        if ( ImGuiFileDialog::Instance()->IsOpened(kSkinExportDialogId) ) {
            Utils::prepareCenteredModalWindow({ 600, 400 });
        }
        if ( ImGuiFileDialog::Instance()->Display(
                 kSkinExportDialogId,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings,
                 { 600, 400 }) ) {
            std::filesystem::path selectedPath;
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                selectedPath = Config::utf8ToPath(
                    ImGuiFileDialog::Instance()->GetFilePathName());
            }
            ImGuiFileDialog::Instance()->Close();
            if ( !selectedPath.empty() ) {
                exportCurrentSkinPackage(selectedPath);
            }
        }
    }
    return changed;
}

bool SettingsView::importSkinPackage(const std::filesystem::path& packagePath)
{
    rememberSkinPickerDirectory(packagePath);
    const auto result = Config::SkinPackageService::importPackage(
        packagePath, Config::AppPaths::skinsRootPath());
    if ( !result.success ) {
        XERROR("Failed to import skin package '{}': {}",
               Config::pathToUtf8(packagePath),
               result.errorMessage);
        showSkinNotification(
            std::string(
                TR_CACHE("ui.settings.software.skin.import_failed").data()) +
            ": " + result.errorMessage);
        return false;
    }

    m_availableSkinDirectoriesDirty = true;
    refreshAvailableSkinDirectories();
    const bool applied = applySkinSelection(
        result.skinDirectoryName, result.installedDirectory / "skin.lua");
    showSkinNotification(
        std::string(
            TR_CACHE("ui.settings.software.skin.import_success").data()) +
        ": " + result.skinDirectoryName);
    return applied;
}

void SettingsView::exportCurrentSkinPackage(
    const std::filesystem::path& outputPath)
{
    const auto  normalizedPath = normalizeSkinPackageOutputPath(outputPath);
    std::string errorMessage;
    const bool success = !normalizedPath.empty() &&
                         Config::SkinPackageService::exportPackage(
                             Config::SkinManager::instance().getData().skinPath,
                             normalizedPath,
                             errorMessage);
    if ( !success ) {
        XERROR("Failed to export skin package '{}': {}",
               Config::pathToUtf8(normalizedPath),
               errorMessage);
        showSkinNotification(
            std::string(
                TR_CACHE("ui.settings.software.skin.export_failed").data()) +
            (errorMessage.empty() ? std::string{} : ": " + errorMessage));
        return;
    }

    rememberSkinPickerDirectory(normalizedPath);
    showSkinNotification(
        std::string(
            TR_CACHE("ui.settings.software.skin.export_success").data()) +
        ": " + Config::pathToUtf8(normalizedPath));
}

}  // namespace MMM::UI
