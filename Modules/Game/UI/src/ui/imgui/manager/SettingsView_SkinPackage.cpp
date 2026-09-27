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
#include "ui/utils/NativeFileDialog.h"
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
///
/// 稳定 ID 与导出选择器隔离，避免两个模态共享打开状态。
constexpr const char* kSkinImportDialogId = "SkinPackageImportPicker";

/// @brief 统一文件选择器的 MSK 导出窗口标识。
///
/// 该 ID 只用于 ImGuiFileDialog，不作为面向用户标题。
constexpr const char* kSkinExportDialogId = "SkinPackageExportPicker";

/// @brief 显示设置页中央通知。
/// @param message 待显示的成功或失败文本。
/// @warning 低频操作反馈；Vulkan 上下文缺失时静默跳过显示。
void showSkinNotification(const std::string& message)
{
    // 关闭阶段上下文可能不可用，不为通知延长渲染器生命周期。
    if ( auto context = Graphic::VKContext::get() ) {
        context->get().showCenterNotification(message);
    }
}

/// @brief 获取文件选择器默认目录。
/// @return 最近浏览目录；尚无记录时回退皮肤根目录。
std::string skinPickerDefaultPath()
{
    // 最近目录在导入、导出和其他文件选择器之间共享用户导航习惯。
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    return settings.lastFilePickerPath.empty()
               ? Config::pathToUtf8(Config::AppPaths::skinsRootPath())
               : settings.lastFilePickerPath;
}

/// @brief 记住用户最近一次选择的文件所在目录。
/// @param filePath 已确认的导入或导出文件路径。
/// @warning 用户确认后的低频配置写入，会调用 AppConfig::save。
void rememberSkinPickerDirectory(const std::filesystem::path& filePath)
{
    // 只保存父目录，不把包文件名误作下一次选择器起始路径。
    const auto parentPath = filePath.parent_path();
    if ( parentPath.empty() ) return;

    // 路径按 UTF-8 写入通用编辑器设置，保持跨平台配置格式一致。
    auto& app                                  = Config::AppConfig::instance();
    app.getEditorSettings().lastFilePickerPath = Config::pathToUtf8(parentPath);
    app.save();
}

/// @brief 生成适合文件选择器使用的当前皮肤包文件名。
/// @return 清理非法字符并带 `.msk` 扩展名的建议文件名。
///
/// 名称取当前皮肤目录末级，替换 Windows 禁止字符和控制字符，并移除尾随空格
/// 或句点。空值、`.` 与 `..` 回退为 `skin`，避免生成路径语义名称。
std::string currentSkinPackageFileName()
{
    // 只使用目录文件名，不把完整皮肤路径暴露为导出建议名。
    std::string name = Config::pathToUtf8(
        Config::SkinManager::instance().getData().skinPath.filename());
    for ( char& character : name ) {
        // 按字节只识别 ASCII 控制与跨平台保守非法字符，UTF-8 字节保持原样。
        const unsigned char byte = static_cast<unsigned char>(character);
        if ( byte < 32 || character == '<' || character == '>' ||
             character == ':' || character == '"' || character == '/' ||
             character == '\\' || character == '|' || character == '?' ||
             character == '*' ) {
            character = '_';
        }
    }
    while ( !name.empty() && (name.back() == ' ' || name.back() == '.') ) {
        // Windows 不允许尾随空格或点，统一清理保证建议名跨平台可用。
        name.pop_back();
    }
    if ( name.empty() || name == "." || name == ".." ) {
        // 防止清理后名称为空或具有父目录导航语义。
        name = "skin";
    }
    return name + ".msk";
}

/// @brief 将输出路径扩展名统一为 .msk。
/// @param outputPath 用户选择的导出目标。
/// @return 替换扩展名后的路径；空输入返回空路径。
std::filesystem::path normalizeSkinPackageOutputPath(
    const std::filesystem::path& outputPath)
{
    // replace_extension 同时处理缺失扩展名和用户输入的其他后缀。
    if ( outputPath.empty() ) return {};
    auto normalized = outputPath;
    normalized.replace_extension(".msk");
    return normalized;
}

/// @brief 判断设置使用系统原生文件选择器。
/// @return 当前偏好为 Native 时返回 true。
bool useNativeSkinFilePicker()
{
    return Config::AppConfig::instance().getEditorSettings().filePickerStyle ==
           Config::FilePickerStyle::Native;
}

}  // namespace

/// @brief 创建皮肤根目录并在桌面文件管理器中打开。
///
/// 目录创建和外部程序启动均使用可报告失败的接口；任一步失败都会记录日志并
/// 显示中央通知，不把文件系统异常传播到 UI。
/// @warning 用户触发的低频路径：包含目录创建和外部文件管理器启动。
void SettingsView::openSkinDirectory()
{
    // skinsRootPath 是应用管理皮肤包的唯一根目录。
    const auto      skinsRoot = Config::AppPaths::skinsRootPath();
    std::error_code createError;
    std::filesystem::create_directories(skinsRoot, createError);
    if ( createError ||
         !DesktopPathUtils::openInFileManager(skinsRoot, false) ) {
        // 日志保留具体路径和系统原因，用户通知只显示本地化摘要。
        XERROR("Failed to open skins directory '{}': {}",
               Config::pathToUtf8(skinsRoot),
               createError ? createError.message()
                           : "desktop file manager launch failed");
        showSkinNotification(std::string(
            TR_CACHE("ui.settings.software.skin.open_directory_failed")
                .data()));
    }
}

/// @brief 打开皮肤包导入文件选择器，并处理 Native 模式即时结果。
/// @return Native 模式成功导入并应用新皮肤时返回 true；其他情况返回 false。
///
/// Native 模式在本调用内完成选择和导入；Unified 模式仅打开命名 IGFD，实际
/// 结果由 renderSkinPackageFileDialogs 在后续帧处理。
///
/// 导入选择约束：
/// - 起始目录优先使用最近文件选择路径；
/// - 没有历史路径时回退应用皮肤根目录；
/// - 原生和统一模式都只展示 `.msk` 包；
/// - 一次最多选择一个包；
/// - 取消不显示失败提示；
/// - NFD 错误不进入 SkinPackageService；
/// - 返回值只表达皮肤是否实际改变。
/// @warning 用户触发路径：Native 对话框可能阻塞 UI 直到用户关闭。
bool SettingsView::openSkinImportFilePicker()
{
    // 两种选择器共享同一起始目录和仅允许 `.msk` 的过滤条件。
    const std::string defaultPath = skinPickerDefaultPath();
    if ( useNativeSkinFilePicker() ) {
        // 原生对话框不经过 ImGui Popup，显式播放打开反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "MusicMapMaker Skin Package",
                                             "msk" } };
        const nfdresult_t result       = NativeFileDialog::openFile(
            &selectedPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && selectedPath != nullptr ) {
            // 路径在释放 NFD 缓冲前转换并完成同步导入。
            const bool changed =
                importSkinPackage(Config::utf8ToPath(selectedPath));
            NFD_FreePathU8(selectedPath);
            return changed;
        }
        if ( result == NFD_ERROR ) {
            // 取消不是错误，不记录日志或显示失败通知。
            const char* error = NFD_GetError();
            XERROR("Failed to open skin import dialog: {}",
                   error ? error : "Unknown NFD error");
            showSkinNotification(std::string(
                TR_CACHE("ui.settings.software.skin.import_failed").data()));
        }
        return false;
    }

    // Unified 导入禁止编辑文件名，只允许从列表选一个现有包。
    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags             = ImGuiFileDialogFlags_Modal |
                                     ImGuiFileDialogFlags_HideColumnType |
                                     ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(kSkinImportDialogId);
    // 重复 OpenDialog 不应重复播放反馈，wasOpen 记录状态跃迁。
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

/// @brief 打开当前皮肤包导出目标选择器。
///
/// Native 模式即时执行导出；Unified 模式预填安全文件名并延迟到逐帧渲染函数
/// 处理确认结果。取消不产生通知或文件写入。
///
/// 导出选择约束：
/// - 建议文件名来自当前皮肤目录名；
/// - 控制字符和跨平台非法字符替换为下划线；
/// - 尾随空格或句点会移除；
/// - 空名及目录导航名回退为 `skin.msk`；
/// - 最终路径无论输入后缀为何都替换为 `.msk`；
/// - 只有实际打包成功才更新最近浏览目录。
/// @warning 用户触发路径：Native 对话框及后续打包会执行阻塞文件操作。
void SettingsView::openSkinExportFilePicker()
{
    // 建议名来源于当前皮肤目录，并已按跨平台文件名规则清理。
    const std::string defaultPath     = skinPickerDefaultPath();
    const std::string defaultFileName = currentSkinPackageFileName();
    if ( useNativeSkinFilePicker() ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "MusicMapMaker Skin Package",
                                             "msk" } };
        const nfdresult_t result =
            NativeFileDialog::saveFile(&selectedPath,
                                       filters,
                                       1,
                                       defaultPath.c_str(),
                                       defaultFileName.c_str());
        if ( result == NFD_OKAY && selectedPath != nullptr ) {
            // exportCurrentSkinPackage 内部统一补齐扩展名和报告结果。
            exportCurrentSkinPackage(Config::utf8ToPath(selectedPath));
            NFD_FreePathU8(selectedPath);
        } else if ( result == NFD_ERROR ) {
            // NFD 错误详情进入日志，中央通知保持面向用户的简短文案。
            const char* error = NFD_GetError();
            XERROR("Failed to open skin export dialog: {}",
                   error ? error : "Unknown NFD error");
            showSkinNotification(std::string(
                TR_CACHE("ui.settings.software.skin.export_failed").data()));
        }
        return;
    }

    // Unified 导出允许编辑文件名，并预填清理后的 `.msk` 名称。
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

/// @brief 驱动 Unified 皮肤导入与导出对话框并处理完成结果。
/// @param dpiScale 当前窗口内容缩放。
/// @return 本帧导入并应用了新皮肤时返回 true。
///
/// 两个对话框使用独立 RAII 样式域；Display 完成后先复制路径、关闭 IGFD，
/// 再执行可能刷新皮肤资源的导入或导出，避免在对话框内部状态仍活跃时重载 UI。
/// @warning UI 热路径：设置窗口每帧调用；文件操作只在用户确认帧触发。
bool SettingsView::renderSkinPackageFileDialogs(float dpiScale)
{
    // changed 只反映导入导致的当前皮肤变更，导出不会要求设置页重建。
    bool changed = false;
    {
        // 导入模态仅在已打开时请求居中，避免干扰普通设置窗口。
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
                // 在 Close 前复制 IGFD 内部路径字符串到拥有的
                // filesystem::path。
                selectedPath = Config::utf8ToPath(
                    ImGuiFileDialog::Instance()->GetFilePathName());
            }
            ImGuiFileDialog::Instance()->Close();
            // 取消得到空路径，不调用导入服务。
            if ( !selectedPath.empty() ) {
                changed = importSkinPackage(selectedPath);
            }
        }
    }

    {
        // 导出与导入使用相同尺寸，但独立 ID 和路径处理动作。
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
                // 导出不改变当前皮肤选择，因此不修改 changed。
                exportCurrentSkinPackage(selectedPath);
            }
        }
    }
    return changed;
}

/// @brief 安装皮肤包、刷新目录列表并选择新皮肤。
/// @param packagePath 已确认的 `.msk` 文件路径。
/// @return 安装成功且新皮肤被应用时返回 true。
///
/// 最近目录在导入前记录；服务负责校验和安装到 skinsRoot。成功后强制刷新可用
/// 目录，再以安装目录中的 skin.lua 应用选择。
///
/// 导入事务边界：
/// - SkinPackageService 决定包结构是否合法；
/// - 安装目录必须位于 AppPaths::skinsRootPath；
/// - 服务失败时不刷新目录缓存或尝试选择皮肤；
/// - 服务成功后标记目录缓存脏并立即重建；
/// - applySkinSelection 使用服务返回的目录名和 skin.lua；
/// - 成功通知表达包已安装，返回值仍以皮肤应用结果为准。
/// @warning 用户触发的低频路径：包含压缩包读取、目录写入和皮肤资源重载。
bool SettingsView::importSkinPackage(const std::filesystem::path& packagePath)
{
    // 即使包校验失败也保留用户浏览目录，便于修正后重试。
    rememberSkinPickerDirectory(packagePath);
    const auto result = Config::SkinPackageService::importPackage(
        packagePath, Config::AppPaths::skinsRootPath());
    if ( !result.success ) {
        // 服务错误详情同时写入日志和用户通知，便于定位具体包问题。
        XERROR("Failed to import skin package '{}': {}",
               Config::pathToUtf8(packagePath),
               result.errorMessage);
        showSkinNotification(
            std::string(
                TR_CACHE("ui.settings.software.skin.import_failed").data()) +
            ": " + result.errorMessage);
        return false;
    }

    // 安装新增目录后使缓存失效，并在选择前立即刷新目录枚举。
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

/// @brief 将当前皮肤目录打包到用户选择的 `.msk` 路径。
/// @param outputPath 用户选择的目标路径。
///
/// 输出扩展名统一规范化，SkinPackageService 负责实际打包和错误文本。成功后记住
/// 目标父目录并显示完整路径，失败不更新最近目录。
///
/// 导出结果约束：
/// - 空输出路径直接进入失败分支；
/// - 服务源目录取当前 SkinManager 已加载皮肤路径；
/// - 错误详情为空时通知只显示通用失败文案；
/// - 错误详情非空时追加到日志和中央通知；
/// - 成功通知包含规范化后的完整目标路径；
/// - 本函数不切换当前皮肤或刷新可用目录列表。
/// @warning 用户触发的低频路径：包含皮肤目录扫描和压缩包写入。
void SettingsView::exportCurrentSkinPackage(
    const std::filesystem::path& outputPath)
{
    // 空目标在规范化后保持为空，并直接作为失败处理。
    const auto  normalizedPath = normalizeSkinPackageOutputPath(outputPath);
    std::string errorMessage;
    const bool success = !normalizedPath.empty() &&
                         Config::SkinPackageService::exportPackage(
                             Config::SkinManager::instance().getData().skinPath,
                             normalizedPath,
                             errorMessage);
    if ( !success ) {
        // normalizedPath 用于日志，确保记录的是服务实际尝试的最终扩展名。
        XERROR("Failed to export skin package '{}': {}",
               Config::pathToUtf8(normalizedPath),
               errorMessage);
        showSkinNotification(
            std::string(
                TR_CACHE("ui.settings.software.skin.export_failed").data()) +
            (errorMessage.empty() ? std::string{} : ": " + errorMessage));
        return;
    }

    // 只有成功写出后才把目标目录设为下一次选择器起点。
    rememberSkinPickerDirectory(normalizedPath);
    showSkinNotification(
        std::string(
            TR_CACHE("ui.settings.software.skin.export_success").data()) +
        ": " + Config::pathToUtf8(normalizedPath));
}

}  // namespace MMM::UI
