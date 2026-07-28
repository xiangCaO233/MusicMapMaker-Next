#include "logic/ProjectController.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include <fstream>
#include <iomanip>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <system_error>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 将新建项目初始设置写入项目实例。
/// @param project 要初始化的项目。
/// @param options 新项目初始设置。
/// @param fallbackTitle 标题为空时使用的回退名称。
void applyProjectCreationOptions(
    Project& project, const ProjectController::ProjectCreationOptions& options,
    const std::string& fallbackTitle)
{
    project.m_metadata.m_title =
        options.m_title.empty() ? fallbackTitle : options.m_title;
    project.m_metadata.m_artist =
        options.m_artist.empty() ? "Unknown" : options.m_artist;
    project.m_metadata.m_mapper =
        options.m_mapper.empty() ? "Unknown" : options.m_mapper;
    project.m_settings.m_colorPaletteSchemeName =
        options.m_colorPaletteSchemeName;
    project.m_settings.m_workspace.m_sidebarActiveTab =
        options.m_sidebarActiveTab.empty() ? std::string{ "FileExplorer" }
                                           : options.m_sidebarActiveTab;
}

/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断文件扩展名是否是可按 zip 读取的谱面包。
/// @param path 待检查路径。
/// @return 扩展名受支持时返回 true。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}

/// @brief 将文件名片段净化为临时目录名可用的 ASCII 字符串。
/// @param name 原始 UTF-8 名称。
/// @return 净化后的目录名片段。
std::string sanitizeTemporaryFolderName(std::string name)
{
    for ( char& ch : name ) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        const bool ok = (byte >= 'a' && byte <= 'z') ||
                        (byte >= 'A' && byte <= 'Z') ||
                        (byte >= '0' && byte <= '9') || ch == '-' || ch == '_';
        if ( !ok ) ch = '_';
    }
    while ( !name.empty() && name.front() == '_' ) {
        name.erase(name.begin());
    }
    while ( !name.empty() && name.back() == '_' ) {
        name.pop_back();
    }
    if ( name.empty() ) return "package";
    return name;
}

/// @brief 读取文件全部字节。
/// @param path 文件路径。
/// @param outBytes 输出字节。
/// @return 读取成功返回 true。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   outBytes)
{
    outBytes.clear();

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;

    const std::ifstream::pos_type endPos = file.tellg();
    if ( endPos <= 0 ) return false;

    outBytes.resize(static_cast<std::size_t>(endPos));
    file.seekg(0, std::ios::beg);
    if ( !file.read(reinterpret_cast<char*>(outBytes.data()),
                    static_cast<std::streamsize>(outBytes.size())) ) {
        outBytes.clear();
        return false;
    }
    return true;
}

/// @brief 写入文件字节并创建父目录。
/// @param path 输出文件路径。
/// @param data 文件字节指针。
/// @param size 文件字节数。
/// @return 写入成功返回 true。
bool writeBytesToFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    std::error_code filesystemError;
    const auto      parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::filesystem::create_directories(parentPath, filesystemError);
        if ( filesystemError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    if ( size > 0 ) {
        file.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(file);
}

/// @brief 以临时文件替换方式写入项目描述文件。
/// @param project 需要序列化的项目。
/// @param projectFile 项目描述文件路径。
/// @return 写入和替换成功时返回 true。
bool writeProjectFile(const Project&               project,
                      const std::filesystem::path& projectFile)
{
    const auto parentPath = projectFile.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parentPath, createDirectoryError);
        if ( createDirectoryError ) {
            XERROR("Failed to create project directory: {}. Error: {}",
                   Config::pathToUtf8(parentPath),
                   createDirectoryError.message());
            return false;
        }
    }

    nlohmann::json        jsonData = project;
    std::filesystem::path tempPath = projectFile;
    tempPath += ".tmp";
    {
        std::ofstream file(tempPath);
        if ( !file.is_open() ) {
            XERROR("Failed to open project temp file: {}",
                   Config::pathToUtf8(tempPath));
            return false;
        }
        file << std::setw(4) << jsonData << '\n';
        if ( !file.good() ) {
            XERROR("Failed to write project temp file: {}",
                   Config::pathToUtf8(tempPath));
            return false;
        }
    }

    std::error_code replaceError;
    std::filesystem::rename(tempPath, projectFile, replaceError);
    if ( !replaceError ) return true;

    std::error_code copyError;
    std::filesystem::copy_file(
        tempPath,
        projectFile,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    std::error_code removeTempError;
    std::filesystem::remove(tempPath, removeTempError);
    if ( copyError ) {
        XERROR("Failed to replace project file: {}. Error: {}",
               Config::pathToUtf8(projectFile),
               copyError.message());
        return false;
    }
    return true;
}

/// @brief 将 zip 包内路径归一化为通用分隔符。
/// @param archiveName 原始包内路径。
/// @return 归一化后的包内路径。
std::string normalizeArchiveName(std::string archiveName)
{
    std::replace(archiveName.begin(), archiveName.end(), '\\', '/');
    while ( !archiveName.empty() && archiveName.back() == '/' ) {
        archiveName.pop_back();
    }
    return archiveName;
}

/// @brief 判断 Windows 文件名片段是否包含不兼容字符或形式。
/// @param name 单个文件名片段，不包含路径分隔符。
/// @return 不兼容时返回原因，否则返回空。
std::optional<std::string> describeWindowsIncompatibleFileName(
    const std::string& name)
{
    if ( name.empty() ) return "空文件名片段";

    for ( const unsigned char ch : name ) {
        if ( ch < 32 ) return "控制字符";
        switch ( ch ) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '|':
        case '?':
        case '*':
            return std::string("Windows 不支持的字符 '") +
                   static_cast<char>(ch) + "'";
        default: break;
        }
    }

    if ( name.back() == ' ' || name.back() == '.' ) {
        return "文件名不能以空格或点结尾";
    }

    std::string baseName = name;
    if ( const auto dotPos = baseName.find('.'); dotPos != std::string::npos ) {
        baseName.resize(dotPos);
    }
    std::transform(
        baseName.begin(),
        baseName.end(),
        baseName.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    if ( baseName == "CON" || baseName == "PRN" || baseName == "AUX" ||
         baseName == "NUL" ) {
        return "Windows 保留设备名";
    }
    if ( baseName.size() == 4 &&
         (baseName.starts_with("COM") || baseName.starts_with("LPT")) &&
         baseName[3] >= '1' && baseName[3] <= '9' ) {
        return "Windows 保留设备名";
    }

    return std::nullopt;
}

/// @brief 检查 zip 包内路径是否安全且可在 Windows 上落盘。
/// @param archiveName 归一化后的包内路径。
/// @return 不安全或不兼容时返回原因，否则返回空。
std::optional<std::string> describeUnsafeArchiveName(
    const std::string& archiveName)
{
    if ( archiveName.empty() ) {
        return "谱面包包含空路径";
    }
    if ( archiveName.front() == '/' ) {
        return "谱面包包含绝对路径";
    }

    const auto path = Config::utf8ToPath(archiveName);
    if ( path.empty() || path.is_absolute() ) {
        return "谱面包包含绝对路径";
    }
    for ( const auto& part : path.lexically_normal() ) {
        if ( part == ".." ) {
            return "谱面包路径越出目标目录";
        }
        if ( part == "." ) {
            continue;
        }
        const auto partText = Config::pathToUtf8(part);
        if ( auto reason = describeWindowsIncompatibleFileName(partText) ) {
            return *reason;
        }
    }

    return std::nullopt;
}

/// @brief 发布项目或谱面包打开失败事件。
/// @param path 尝试打开的路径。
/// @param message 失败原因。
/// @param isPackage 是否为谱面包打开失败。
void publishProjectOpenFailed(const std::filesystem::path& path,
                              const std::string& message, bool isPackage)
{
    Event::ProjectOpenFailedEvent event;
    event.m_projectPath  = Config::pathToUtf8(path);
    event.m_errorMessage = message;
    event.m_isPackage    = isPackage;
    Event::EventBus::instance().publish(event);
}

/// @brief 尽量取得路径的绝对规范形式。
/// @param path 待规范化路径。
/// @return 成功时返回 weakly_canonical/absolute
/// 路径，失败时退回词法规范化路径。
std::filesystem::path makeAbsoluteNormalizedPath(
    const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    std::error_code filesystemError;
    auto normalized = std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    filesystemError.clear();
    normalized = std::filesystem::absolute(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    return path.lexically_normal();
}

/// @brief 将 zip 兼容谱面包安全解压到目标目录。
/// @param packagePath 谱面包路径。
/// @param destinationRoot 解压目标目录。
/// @param errorMessage 失败时写入错误信息。
/// @return 解压成功返回 true。
bool extractZipPackageToDirectory(const std::filesystem::path& packagePath,
                                  const std::filesystem::path& destinationRoot,
                                  std::string&                 errorMessage)
{
    errorMessage.clear();

    std::vector<std::uint8_t> packageBytes;
    if ( !readFileBytes(packagePath, packageBytes) ) {
        errorMessage = "无法读取谱面包文件";
        return false;
    }

    mz_zip_archive zipArchive{};
    if ( !mz_zip_reader_init_mem(
             &zipArchive, packageBytes.data(), packageBytes.size(), 0) ) {
        errorMessage = "谱面包不是可读取的 zip 兼容格式";
        return false;
    }

    bool          success   = true;
    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "读取谱面包条目失败";
            success      = false;
            break;
        }

        const std::string archiveName =
            normalizeArchiveName(std::string(fileStat.m_filename));
        if ( archiveName.empty() ) continue;
        if ( auto unsafeReason = describeUnsafeArchiveName(archiveName) ) {
            errorMessage = fmt::format("{}：{}", *unsafeReason, archiveName);
            success      = false;
            break;
        }

        const auto destinationPath =
            (destinationRoot / Config::utf8ToPath(archiveName))
                .lexically_normal();

        if ( mz_zip_reader_is_file_a_directory(&zipArchive, index) ) {
            std::error_code filesystemError;
            std::filesystem::create_directories(destinationPath,
                                                filesystemError);
            if ( filesystemError ) {
                errorMessage = filesystemError.message();
                success      = false;
                break;
            }
            continue;
        }

        std::size_t extractedSize = 0;
        void*       extractedData = mz_zip_reader_extract_to_heap(
            &zipArchive, index, &extractedSize, 0);
        if ( !extractedData ) {
            errorMessage = "解压谱面包条目失败：" + archiveName;
            success      = false;
            break;
        }

        success =
            writeBytesToFile(destinationPath, extractedData, extractedSize);
        mz_free(extractedData);
        if ( !success ) {
            errorMessage = "写入临时项目文件失败：" + archiveName;
            break;
        }
    }

    mz_zip_reader_end(&zipArchive);
    return success;
}

/// @brief 创建唯一的临时项目目录。
/// @param packagePath 原始谱面包路径。
/// @param errorMessage 失败时写入错误信息。
/// @return 成功时返回临时项目目录。
std::optional<std::filesystem::path> createTemporaryProjectRoot(
    const std::filesystem::path& packagePath, std::string& errorMessage)
{
    std::error_code filesystemError;
    const auto      tempRoot =
        std::filesystem::temp_directory_path(filesystemError) /
        "MusicMapMaker-Next" / "temporary_projects";
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return std::nullopt;
    }

    std::filesystem::create_directories(tempRoot, filesystemError);
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return std::nullopt;
    }

    const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const std::string baseName =
        sanitizeTemporaryFolderName(Config::pathToUtf8(packagePath.stem()));

    for ( int attempt = 0; attempt < 64; ++attempt ) {
        const auto candidate =
            tempRoot / fmt::format("{}_{}_{}", baseName, tick, attempt);
        if ( std::filesystem::exists(candidate, filesystemError) &&
             !filesystemError ) {
            continue;
        }
        filesystemError.clear();
        std::filesystem::create_directories(candidate, filesystemError);
        if ( !filesystemError ) return candidate;
    }

    errorMessage = "无法创建唯一的临时项目目录";
    return std::nullopt;
}

/// @brief 判断目录是否为空。
/// @param path 待检查目录。
/// @return 空目录返回 true。
bool isDirectoryEmpty(const std::filesystem::path& path)
{
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(path, filesystemError);
    if ( filesystemError ) return false;
    const std::filesystem::directory_iterator endIterator;
    return iterator == endIterator;
}

/// @brief 为保存临时项目选择不会覆盖用户文件的最终目录。
/// @param selectedPath 用户选择的路径。
/// @param project 当前临时项目。
/// @return 实际用于保存的项目目录。
std::filesystem::path resolveTemporaryProjectSaveRoot(
    const std::filesystem::path& selectedPath, const Project& project)
{
    std::error_code filesystemError;
    if ( selectedPath.empty() ) return {};
    if ( !std::filesystem::exists(selectedPath, filesystemError) ||
         filesystemError ) {
        return selectedPath;
    }
    filesystemError.clear();
    if ( !std::filesystem::is_directory(selectedPath, filesystemError) ||
         filesystemError ) {
        return {};
    }
    if ( isDirectoryEmpty(selectedPath) ) return selectedPath;

    std::string baseName =
        Config::pathToUtf8(project.m_temporarySourcePackagePath.stem());
    if ( baseName.empty() ) {
        baseName = project.m_metadata.m_title;
    }
    baseName = sanitizeTemporaryFolderName(baseName);

    for ( int attempt = 0; attempt < 128; ++attempt ) {
        const auto candidate =
            selectedPath /
            (attempt == 0 ? baseName : fmt::format("{}_{}", baseName, attempt));
        if ( !std::filesystem::exists(candidate, filesystemError) &&
             !filesystemError ) {
            return candidate;
        }
        filesystemError.clear();
    }
    return {};
}

/// @brief 递归复制目录内容。
/// @param sourceRoot 源目录。
/// @param destinationRoot 目标目录。
/// @param errorMessage 失败时写入错误信息。
/// @return 复制成功返回 true。
bool copyDirectoryContents(const std::filesystem::path& sourceRoot,
                           const std::filesystem::path& destinationRoot,
                           std::string&                 errorMessage)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(destinationRoot, filesystemError);
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return false;
    }

    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(
        sourceRoot, directoryOptions, filesystemError);
    const std::filesystem::recursive_directory_iterator endIterator;
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return false;
    }

    while ( iterator != endIterator ) {
        const auto entryPath = iterator->path();
        auto       relativePath =
            std::filesystem::relative(entryPath, sourceRoot, filesystemError);
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }

        const auto destinationPath =
            (destinationRoot / relativePath).lexically_normal();
        filesystemError.clear();
        if ( iterator->is_directory(filesystemError) && !filesystemError ) {
            std::filesystem::create_directories(destinationPath,
                                                filesystemError);
        } else if ( iterator->is_regular_file(filesystemError) &&
                    !filesystemError ) {
            std::filesystem::create_directories(destinationPath.parent_path(),
                                                filesystemError);
            if ( !filesystemError ) {
                std::filesystem::copy_file(
                    entryPath,
                    destinationPath,
                    std::filesystem::copy_options::overwrite_existing,
                    filesystemError);
            }
        }
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }

        iterator.increment(filesystemError);
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }
    }
    return true;
}

/// @brief 将项目描述文件写入指定目录。
/// @param project 项目数据。
/// @param projectRoot 输出项目目录。
/// @param errorMessage 失败时写入错误信息。
/// @return 写入成功返回 true。
bool writeProjectFileTo(const Project&               project,
                        const std::filesystem::path& projectRoot,
                        std::string&                 errorMessage)
{
    const auto    projectFile = projectRoot / "mmm_project.json";
    std::ofstream file(projectFile, std::ios::trunc);
    if ( !file ) {
        errorMessage = "无法写入项目描述文件";
        return false;
    }

    nlohmann::json jsonData = project;
    file << std::setw(4) << jsonData << std::endl;
    if ( !file ) {
        errorMessage = "写入项目描述文件失败";
        return false;
    }
    return true;
}
}  // namespace

/// @brief 获取项目控制器全局实例。
/// @return 项目控制器全局实例引用。
ProjectController& ProjectController::instance()
{
    static ProjectController instance;
    return instance;
}

/// @brief 构造项目控制器并订阅项目请求事件。
ProjectController::ProjectController()
{
    /// @brief 项目控制器订阅项目事件使用的全局事件总线。
    auto& eventBus            = Event::EventBus::instance();
    m_openProjectSubscription = eventBus.subscribe<Event::OpenProjectEvent>(
        [this](const Event::OpenProjectEvent& event) {
            requestOpenProject(event.m_projectPath);
        });
    m_openTemporaryProjectSubscription =
        eventBus.subscribe<Event::OpenTemporaryProjectPackageEvent>(
            [this](const Event::OpenTemporaryProjectPackageEvent& event) {
                requestOpenTemporaryProjectPackage(event.m_packagePath);
            });
    m_closeProjectSubscription =
        eventBus.subscribe<Event::ProjectCloseRequestedEvent>(
            [this](const Event::ProjectCloseRequestedEvent&) {
                requestCloseProject();
            });
    m_createProjectSubscription =
        eventBus.subscribe<Event::ProjectCreateRequestedEvent>(
            [this](const Event::ProjectCreateRequestedEvent& event) {
                ProjectCreationOptions options;
                options.m_title  = event.m_title;
                options.m_artist = event.m_artist;
                options.m_mapper = event.m_mapper;
                options.m_colorPaletteSchemeName =
                    event.m_colorPaletteSchemeName;
                options.m_sidebarActiveTab = event.m_sidebarActiveTab;
                requestCreateProject(event.m_projectPath, options);
            });
    m_projectSwitchCompletedSubscription =
        eventBus.subscribe<Event::ProjectSwitchCompletedEvent>(
            [this](const Event::ProjectSwitchCompletedEvent&) {
                completePendingProjectSwitch();
            });
    m_projectSwitchCancelledSubscription =
        eventBus.subscribe<Event::ProjectSwitchCancelledEvent>(
            [this](const Event::ProjectSwitchCancelledEvent&) {
                cancelPendingProjectSwitch();
            });
}

/// @brief 析构项目控制器并取消项目事件订阅。
ProjectController::~ProjectController()
{
    /// @brief 项目控制器取消项目事件订阅使用的全局事件总线。
    auto& eventBus = Event::EventBus::instance();
    if ( m_openProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::OpenProjectEvent>(
            m_openProjectSubscription);
    }
    if ( m_openTemporaryProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::OpenTemporaryProjectPackageEvent>(
            m_openTemporaryProjectSubscription);
    }
    if ( m_closeProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectCloseRequestedEvent>(
            m_closeProjectSubscription);
    }
    if ( m_createProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectCreateRequestedEvent>(
            m_createProjectSubscription);
    }
    if ( m_projectSwitchCompletedSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectSwitchCompletedEvent>(
            m_projectSwitchCompletedSubscription);
    }
    if ( m_projectSwitchCancelledSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectSwitchCancelledEvent>(
            m_projectSwitchCancelledSubscription);
    }
    stopDirectoryWatcher();
}

/// @brief 发布项目切换需要关闭旧画布的事件。
/// @param projectPathToOpen 旧画布关闭后需要打开的项目路径。
/// @param closeOnly 是否只关闭当前项目而不打开新项目。
void ProjectController::publishProjectSwitchNeedsCanvasClose(
    const std::filesystem::path& projectPathToOpen, bool closeOnly) const
{
    /// @brief 项目切换等待旧画布关闭的事件载荷。
    Event::ProjectSwitchNeedsCanvasCloseEvent event;
    event.m_projectPathToOpen = projectPathToOpen;
    event.m_closeOnly         = closeOnly;
    Event::EventBus::instance().publish(event);
}

/// @brief 获取当前项目。
/// @return 当前项目指针；未打开项目时返回 nullptr。
Project* ProjectController::currentProject()
{
    return m_currentProject.get();
}

/// @brief 获取当前项目。
/// @return 当前项目只读指针；未打开项目时返回 nullptr。
const Project* ProjectController::currentProject() const
{
    return m_currentProject.get();
}

/// @brief 请求打开项目，必要时等待 UI 完成旧画布关闭。
/// @param projectPath 要打开的项目目录或谱面文件路径。
void ProjectController::requestOpenProject(
    const std::filesystem::path& projectPath)
{
    if ( projectPath.empty() ) {
        return;
    }

    /// @brief 保护本次打开请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectCreationOptions.reset();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectClose    = false;
    m_pendingProjectClose      = false;
    m_projectCloseReady        = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_pendingProjectSwitchPath     = projectPath;
        m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
        m_pendingProjectSwitchCreationOptions.reset();
    } else {
        m_requestedProjectPath     = projectPath;
        m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求打开谱面包为临时只读项目。
/// @param packagePath 要解压阅览的谱面包路径。
void ProjectController::requestOpenTemporaryProjectPackage(
    const std::filesystem::path& packagePath)
{
    if ( packagePath.empty() ) {
        return;
    }

    /// @brief 保护本次临时谱面包打开请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_pendingProjectSwitchPath     = packagePath;
        m_pendingProjectSwitchOpenMode = ProjectOpenMode::TemporaryPackage;
        m_pendingProjectSwitchCreationOptions.reset();
    } else {
        m_requestedProjectPath     = packagePath;
        m_requestedProjectOpenMode = ProjectOpenMode::TemporaryPackage;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求创建并打开项目，必要时等待 UI 完成旧画布关闭。
/// @param projectPath 要创建的项目根目录。
/// @param options 新项目初始设置。
void ProjectController::requestCreateProject(
    const std::filesystem::path&  projectPath,
    const ProjectCreationOptions& options)
{
    if ( projectPath.empty() ) {
        return;
    }

    /// @brief 保护本次新建项目请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_requestedProjectOpenMode = ProjectOpenMode::Normal;
        m_requestedProjectCreationOptions.reset();
        m_pendingProjectSwitchPath            = projectPath;
        m_pendingProjectSwitchOpenMode        = ProjectOpenMode::Normal;
        m_pendingProjectSwitchCreationOptions = options;
    } else {
        m_requestedProjectPath            = projectPath;
        m_requestedProjectOpenMode        = ProjectOpenMode::Normal;
        m_requestedProjectCreationOptions = options;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求关闭当前项目，必要时等待 UI 完成旧画布关闭。
void ProjectController::requestCloseProject()
{
    /// @brief 保护本次关闭请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectPath.clear();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectCreationOptions.reset();
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    m_requestedProjectClose = true;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 是否存在等待旧谱面画布关闭后的项目打开或关闭流程。
/// @return 有挂起项目切换流程时返回 true。
bool ProjectController::hasPendingProjectSwitch() const
{
    /// @brief 保护挂起项目切换状态读取的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return !m_pendingProjectSwitchPath.empty() || m_pendingProjectClose;
}

/// @brief 完成 UI 侧逐个关闭旧谱面画布后的项目切换流程。
void ProjectController::completePendingProjectSwitch()
{
    /// @brief 保护挂起项目切换状态推进的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if ( m_pendingProjectClose ) {
        m_pendingProjectClose = false;
        m_projectCloseReady   = true;
        m_hasPendingProjectAction.store(true, std::memory_order_release);
        return;
    }

    if ( m_pendingProjectSwitchPath.empty() ) return;

    m_pendingProjectPath            = m_pendingProjectSwitchPath;
    m_pendingProjectOpenMode        = m_pendingProjectSwitchOpenMode;
    m_pendingProjectCreationOptions = m_pendingProjectSwitchCreationOptions;
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 取消所有挂起项目切换流程。
void ProjectController::cancelPendingProjectSwitch()
{
    /// @brief 保护挂起项目切换状态清理的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectPath.clear();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectCreationOptions.reset();
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    m_hasPendingProjectAction.store(false, std::memory_order_release);
}

/// @brief 消费逻辑线程本轮需要处理的项目切换动作。
/// @param needsCanvasClose 当前是否需要先关闭旧谱面画布。
/// @return 本轮需要执行的关闭或打开动作。
ProjectController::PendingProjectAction
ProjectController::consumePendingProjectAction(bool needsCanvasClose)
{
    if ( !m_hasPendingProjectAction.exchange(false,
                                             std::memory_order_acq_rel) ) {
        return {};
    }

    /// @brief 本轮要返回给逻辑线程的项目动作。
    PendingProjectAction action;
    /// @brief 本轮消费到的项目打开请求。
    std::filesystem::path requestedPath;
    /// @brief 本轮消费到的项目打开模式。
    ProjectOpenMode requestedOpenMode = ProjectOpenMode::Normal;
    /// @brief 本轮消费到的项目创建初始设置。
    std::optional<ProjectCreationOptions> requestedCreationOptions;
    /// @brief 本轮是否消费到项目关闭请求。
    bool requestedClose = false;
    /// @brief 本轮是否需要通知 UI 先关闭旧画布。
    bool shouldPublishCanvasClose = false;
    /// @brief 通知 UI 关闭旧画布后要打开的项目路径。
    std::filesystem::path canvasCloseProjectPath;
    /// @brief 通知 UI 关闭旧画布后是否只关闭项目。
    bool canvasCloseOnly = false;

    {
        /// @brief 保护从待处理请求队列取出请求的锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if ( m_requestedProjectClose ) {
            requestedClose          = true;
            m_requestedProjectClose = false;
        }
        if ( !m_requestedProjectPath.empty() ) {
            requestedPath            = m_requestedProjectPath;
            requestedOpenMode        = m_requestedProjectOpenMode;
            requestedCreationOptions = m_requestedProjectCreationOptions;
            m_requestedProjectPath.clear();
            m_requestedProjectOpenMode = ProjectOpenMode::Normal;
            m_requestedProjectCreationOptions.reset();
        }
    }

    if ( requestedClose ) {
        if ( needsCanvasClose ) {
            /// @brief 保护关闭请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
            m_pendingProjectSwitchPath.clear();
            m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectSwitchCreationOptions.reset();
            m_pendingProjectClose    = true;
            shouldPublishCanvasClose = true;
            canvasCloseOnly          = true;
            XINFO(
                "Project close deferred until current beatmap canvases close.");
        } else {
            action.m_closeProject = true;
        }
    }

    if ( !requestedPath.empty() ) {
        if ( needsCanvasClose ) {
            /// @brief 保护打开请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
            m_pendingProjectClose                 = false;
            m_pendingProjectSwitchPath            = requestedPath;
            m_pendingProjectSwitchOpenMode        = requestedOpenMode;
            m_pendingProjectSwitchCreationOptions = requestedCreationOptions;
            shouldPublishCanvasClose              = true;
            canvasCloseProjectPath                = requestedPath;
            canvasCloseOnly                       = false;
            XINFO(
                "Project open deferred until current beatmap canvases close: "
                "{}",
                Config::pathToUtf8(requestedPath));
        } else {
            action.m_projectPathToOpen      = requestedPath;
            action.m_projectCreationOptions = requestedCreationOptions;
            action.m_projectOpenMode        = requestedOpenMode;
        }
    }

    if ( shouldPublishCanvasClose ) {
        publishProjectSwitchNeedsCanvasClose(canvasCloseProjectPath,
                                             canvasCloseOnly);
    }

    {
        /// @brief 保护 UI 完成后的延迟动作消费锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if ( m_projectCloseReady ) {
            action.m_closeProject = true;
            m_projectCloseReady   = false;
        }
        if ( action.m_projectPathToOpen.empty() &&
             !m_pendingProjectPath.empty() ) {
            action.m_projectPathToOpen      = m_pendingProjectPath;
            action.m_projectOpenMode        = m_pendingProjectOpenMode;
            action.m_projectCreationOptions = m_pendingProjectCreationOptions;
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
        }
    }

    return action;
}

/// @brief 判断是否存在待逻辑线程消费的项目切换动作。
bool ProjectController::hasPendingProjectAction() const
{
    return m_hasPendingProjectAction.load(std::memory_order_acquire);
}

/// @brief 打开项目并启动项目目录监听。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @return 打开项目后的结果信息。
ProjectController::OpenProjectResult ProjectController::openProject(
    const std::filesystem::path&                 projectPath,
    const std::optional<ProjectCreationOptions>& creationOptions,
    const std::optional<TemporaryProjectInfo>&   temporaryInfo)
{
    /// @brief 本次打开项目的返回结果。
    OpenProjectResult result;
    /// @brief 调用方传入路径的绝对规范形式。
    const std::filesystem::path requestedProjectPath =
        makeAbsoluteNormalizedPath(projectPath);
    /// @brief 实际打开的项目目录路径。
    std::filesystem::path actualProjectPath = requestedProjectPath;
    /// @brief 若传入谱面文件，则记录需要自动打开的谱面路径。
    std::filesystem::path targetBeatmapPath;
    /// @brief 本次打开是否来自临时谱面包。
    const bool isPackageOpen = temporaryInfo && temporaryInfo->m_isTemporary;
    /// @brief 失败提示中展示给用户的源路径。
    const std::filesystem::path failureDisplayPath =
        isPackageOpen ? temporaryInfo->m_sourcePackagePath : projectPath;

    if ( creationOptions ) {
        std::error_code filesystemError;
        if ( !std::filesystem::exists(actualProjectPath, filesystemError) ) {
            std::filesystem::create_directories(actualProjectPath,
                                                filesystemError);
            if ( filesystemError ) {
                const std::string message =
                    "无法创建项目目录：" +
                    Config::pathToUtf8(actualProjectPath);
                XERROR("Failed to create project directory: {}",
                       Config::pathToUtf8(actualProjectPath));
                publishProjectOpenFailed(
                    failureDisplayPath, message, isPackageOpen);
                return result;
            }
        }

        filesystemError.clear();
        if ( !std::filesystem::is_directory(actualProjectPath,
                                            filesystemError) ||
             filesystemError ) {
            const std::string message = "创建项目失败，目标不是文件夹：" +
                                        Config::pathToUtf8(actualProjectPath);
            XERROR("Failed to create project: Target is not a directory: {}",
                   Config::pathToUtf8(actualProjectPath));
            publishProjectOpenFailed(
                failureDisplayPath, message, isPackageOpen);
            return result;
        }
        actualProjectPath = makeAbsoluteNormalizedPath(actualProjectPath);
    } else {
        std::error_code requestedPathError;
        const bool      requestedPathIsFile =
            std::filesystem::exists(requestedProjectPath, requestedPathError) &&
            !requestedPathError &&
            std::filesystem::is_regular_file(requestedProjectPath,
                                             requestedPathError) &&
            !requestedPathError;
        if ( requestedPathIsFile ) {
            targetBeatmapPath = requestedProjectPath;
            actualProjectPath =
                makeAbsoluteNormalizedPath(requestedProjectPath.parent_path());
        }
    }

    std::error_code actualPathError;
    const bool      actualProjectPathIsDirectory =
        std::filesystem::exists(actualProjectPath, actualPathError) &&
        !actualPathError &&
        std::filesystem::is_directory(actualProjectPath, actualPathError) &&
        !actualPathError;
    if ( !actualProjectPathIsDirectory ) {
        const std::string message =
            "路径不存在或不是文件夹：" + Config::pathToUtf8(actualProjectPath);
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            Config::pathToUtf8(actualProjectPath));
        publishProjectOpenFailed(failureDisplayPath, message, isPackageOpen);
        return result;
    }

    XINFO("Opening project at: {}", Config::pathToUtf8(actualProjectPath));

    /// @brief 新创建并等待接管为当前项目的项目实例。
    auto newProject           = std::make_unique<Project>();
    newProject->m_projectRoot = actualProjectPath;
    if ( temporaryInfo && temporaryInfo->m_isTemporary ) {
        newProject->m_isTemporaryProject = true;
        newProject->m_temporarySourcePackagePath =
            temporaryInfo->m_sourcePackagePath;
    }
    newProject->m_metadata.m_title =
        Config::pathToUtf8(actualProjectPath.filename());

    /// @brief 当前项目目录扫描结果。
    auto directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
    if ( !directoryScan.m_success ) {
        XERROR("Error while scanning project directory: {}",
               Config::pathToUtf8(actualProjectPath));
    }

    m_projectResourceService.buildInitialResources(*newProject, directoryScan);

    /// @brief 项目描述文件路径。
    std::filesystem::path projectFile = actualProjectPath / "mmm_project.json";
    /// @brief 项目描述文件是否已存在。
    std::error_code projectFileExistsError;
    const bool      projectFileExists =
        std::filesystem::exists(projectFile, projectFileExistsError) &&
        !projectFileExistsError;
    if ( projectFileExists ) {
        /// @brief 项目描述文件输入流。
        std::ifstream file(projectFile);
        if ( !file.is_open() ) {
            XWARN(
                "Failed to open existing mmm_project.json, using scanned "
                "results.");
        } else {
            /// @brief 项目描述 JSON 数据。
            nlohmann::json jsonData =
                nlohmann::json::parse(file, nullptr, false);
            if ( jsonData.is_discarded() || !jsonData.is_object() ||
                 file.bad() ) {
                XWARN(
                    "Failed to parse existing mmm_project.json, using scanned "
                    "results.");
            } else {
                /// @brief 从项目描述文件反序列化出的项目配置。
                Project loadedProject = jsonData.get<Project>();
                /// @brief 需要从旧版 m_volume 迁移且不信任持久化类型的资源。
                const auto legacyAudioResourceKeys =
                    ProjectResourceService::collectLegacyAudioResourceKeys(
                        jsonData);
                newProject->m_metadata = loadedProject.m_metadata;
                newProject->m_settings = loadedProject.m_settings;
                newProject->m_excludedBeatmapPaths =
                    loadedProject.m_excludedBeatmapPaths;
                newProject->m_excludedAudioPaths =
                    loadedProject.m_excludedAudioPaths;

                m_projectResourceService.mergePersistedAudioResources(
                    *newProject, loadedProject, legacyAudioResourceKeys);
                if ( !legacyAudioResourceKeys.empty() ) {
                    XINFO("Migrated {} legacy audio resource configurations.",
                          legacyAudioResourceKeys.size());
                }

                const auto legacyBeatmapAudioMigration =
                    m_projectResourceService.migrateLegacyBeatmapAudioTracks(
                        *newProject, loadedProject);
                if ( legacyBeatmapAudioMigration.m_migratedBeatmapCount > 0 ) {
                    XINFO(
                        "Migrated {} legacy beatmap audio track references to "
                        "MMM v2 samples.",
                        legacyBeatmapAudioMigration.m_migratedBeatmapCount);
                }
                for ( const auto& failedBeatmapPath :
                      legacyBeatmapAudioMigration.m_failedBeatmapPaths ) {
                    XWARN(
                        "Failed to migrate legacy beatmap audio reference: "
                        "{}",
                        failedBeatmapPath);
                }

                XINFO("Project configuration loaded from mmm_project.json");
            }
        }
    }

    if ( creationOptions && !projectFileExists ) {
        applyProjectCreationOptions(
            *newProject,
            *creationOptions,
            Config::pathToUtf8(actualProjectPath.filename()));
    }
    if ( temporaryInfo && temporaryInfo->m_isTemporary ) {
        const auto packageTitle =
            Config::pathToUtf8(temporaryInfo->m_sourcePackagePath.stem());
        if ( !packageTitle.empty() ) {
            newProject->m_metadata.m_title = packageTitle;
        }
    }

    m_projectResourceService.applyExcludedResources(*newProject);

    if ( temporaryInfo && temporaryInfo->m_isTemporary &&
         newProject->m_beatmaps.empty() ) {
        const std::string message = "谱面包内没有可打开的谱面文件";
        XERROR("Temporary project package contains no supported beatmaps: {}",
               Config::pathToUtf8(temporaryInfo->m_sourcePackagePath));
        publishProjectOpenFailed(
            temporaryInfo->m_sourcePackagePath, message, true);
        return result;
    }

    if ( temporaryInfo && temporaryInfo->m_isTemporary &&
         targetBeatmapPath.empty() && !newProject->m_beatmaps.empty() ) {
        targetBeatmapPath =
            actualProjectPath /
            Config::utf8ToPath(newProject->m_beatmaps.front().m_filePath);
    }

    (void)writeProjectFile(*newProject, projectFile);

    for ( const auto& resource : newProject->m_audioResources ) {
        if ( resource.m_type != AudioTrackType::Effect ) {
            continue;
        }

        /// @brief 音效资源在项目目录中的绝对路径。
        auto absolutePath =
            actualProjectPath / Config::utf8ToPath(resource.m_path);
        std::error_code resourcePathError;
        if ( !std::filesystem::exists(absolutePath, resourcePathError) ||
             resourcePathError ) {
            continue;
        }

        /// @brief 需要调用方登记的按需加载音效请求。
        ProjectCommandService::AudioRegistrationRequest registrationRequest;
        registrationRequest.m_resource     = resource;
        registrationRequest.m_absolutePath = absolutePath;
        result.m_effectRegistrations.push_back(registrationRequest);
    }

    m_currentProject = std::move(newProject);
    m_projectDirectoryWatcher.start(actualProjectPath);
    if ( !temporaryInfo || !temporaryInfo->m_isTemporary ) {
        Config::AppConfig::instance().addRecentProject(
            Config::pathToUtf8(actualProjectPath));
    }

    result.m_opened            = true;
    result.m_actualProjectPath = actualProjectPath;
    result.m_targetBeatmapPath = targetBeatmapPath;
    result.m_projectTitle      = m_currentProject->m_metadata.m_title;
    result.m_beatmapCount      = m_currentProject->m_beatmaps.size();

    return result;
}

/// @brief 解压谱面包并作为临时只读项目打开。
/// @param packagePath 要打开的谱面包路径。
/// @return 打开项目后的结果信息。
ProjectController::OpenProjectResult
ProjectController::openTemporaryProjectPackage(
    const std::filesystem::path& packagePath)
{
    OpenProjectResult result;
    auto              prepared = prepareTemporaryProjectPackage(packagePath);
    if ( !prepared.m_success ) {
        XERROR("Temporary package open failed: {}", prepared.m_errorMessage);
        publishProjectOpenFailed(packagePath, prepared.m_errorMessage, true);
        return result;
    }

    result = openProject(prepared.m_temporaryInfo.m_cacheProjectPath,
                         std::nullopt,
                         prepared.m_temporaryInfo);
    if ( !result.m_opened ) {
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
    }
    return result;
}

/// @brief 仅解压谱面包并准备临时项目目录，不切换当前项目。
/// @param packagePath 要准备的谱面包路径。
/// @return 临时项目准备结果。
ProjectController::PreparedTemporaryProjectResult
ProjectController::prepareTemporaryProjectPackage(
    const std::filesystem::path& packagePath) const
{
    PreparedTemporaryProjectResult result;
    std::error_code                filesystemError;
    if ( packagePath.empty() ||
         !std::filesystem::is_regular_file(packagePath, filesystemError) ||
         filesystemError ) {
        result.m_errorMessage = "谱面包文件不存在";
        return result;
    }
    if ( !isTemporaryPackagePath(packagePath) ) {
        result.m_errorMessage = "不支持的谱面包扩展名";
        return result;
    }

    std::string errorMessage;
    auto        tempProjectRoot =
        createTemporaryProjectRoot(packagePath, errorMessage);
    if ( !tempProjectRoot ) {
        result.m_errorMessage = errorMessage;
        return result;
    }

    if ( !extractZipPackageToDirectory(
             packagePath, *tempProjectRoot, errorMessage) ) {
        std::filesystem::remove_all(*tempProjectRoot, filesystemError);
        result.m_errorMessage = errorMessage;
        return result;
    }

    result.m_success                           = true;
    result.m_temporaryInfo.m_isTemporary       = true;
    result.m_temporaryInfo.m_sourcePackagePath = packagePath;
    result.m_temporaryInfo.m_cacheProjectPath  = *tempProjectRoot;
    return result;
}

/// @brief 当前是否打开了临时项目。
/// @return 当前项目是临时项目时返回 true。
bool ProjectController::isCurrentProjectTemporary() const
{
    return m_currentProject && m_currentProject->m_isTemporaryProject;
}

/// @brief 获取当前临时项目信息。
/// @return 当前临时项目源文件与缓存路径；非临时项目时返回默认值。
ProjectController::TemporaryProjectInfo
ProjectController::currentTemporaryProjectInfo() const
{
    TemporaryProjectInfo info;
    if ( !m_currentProject || !m_currentProject->m_isTemporaryProject ) {
        return info;
    }

    info.m_isTemporary       = true;
    info.m_sourcePackagePath = m_currentProject->m_temporarySourcePackagePath;
    info.m_cacheProjectPath  = m_currentProject->m_projectRoot;
    return info;
}

/// @brief 将当前临时项目复制保存到正式目录，并原地转为正式项目。
/// @param destinationPath 用户选择的保存目录。
/// @return 保存结果。
ProjectController::SaveTemporaryProjectResult
ProjectController::saveTemporaryProjectTo(
    const std::filesystem::path& destinationPath)
{
    SaveTemporaryProjectResult result;
    if ( !m_currentProject || !m_currentProject->m_isTemporaryProject ) {
        result.m_errorMessage = "当前没有临时项目";
        return result;
    }

    const auto saveRoot =
        resolveTemporaryProjectSaveRoot(destinationPath, *m_currentProject);
    if ( saveRoot.empty() ) {
        result.m_errorMessage = "保存目录不可用";
        return result;
    }

    std::string errorMessage;
    if ( !copyDirectoryContents(
             m_currentProject->m_projectRoot, saveRoot, errorMessage) ) {
        result.m_errorMessage = errorMessage;
        return result;
    }

    Project savedProject              = *m_currentProject;
    savedProject.m_isTemporaryProject = false;
    savedProject.m_temporarySourcePackagePath.clear();
    savedProject.m_projectRoot = saveRoot;
    if ( !writeProjectFileTo(savedProject, saveRoot, errorMessage) ) {
        result.m_errorMessage = errorMessage;
        return result;
    }

    *m_currentProject = savedProject;
    m_projectDirectoryWatcher.start(saveRoot);
    Config::AppConfig::instance().addRecentProject(
        Config::pathToUtf8(saveRoot));

    result.m_success          = true;
    result.m_savedProjectPath = saveRoot;
    return result;
}

/// @brief 关闭当前项目并停止项目目录监听。
/// @return 被关闭项目的信息。
ProjectController::CloseProjectResult ProjectController::closeProject()
{
    /// @brief 本次关闭项目的返回结果。
    CloseProjectResult result;
    if ( !m_currentProject ) {
        return result;
    }

    result.m_projectTitle = m_currentProject->m_metadata.m_title;
    /// @brief 被关闭项目的根目录路径快照。
    std::filesystem::path projectPath = m_currentProject->m_projectRoot;
    result.m_project                  = std::move(m_currentProject);
    result.m_closed                   = true;
    m_projectDirectoryWatcher.stop();

    /// @brief 项目关闭完成后向 UI 和其它监听者发布的生命周期事件。
    Event::ProjectClosedEvent closedEvent;
    closedEvent.m_projectTitle = result.m_projectTitle;
    closedEvent.m_projectPath  = projectPath;
    Event::EventBus::instance().publish(closedEvent);

    return result;
}

/// @brief 停止项目目录监听。
void ProjectController::stopDirectoryWatcher()
{
    m_projectDirectoryWatcher.stop();
}

/// @brief 消费项目目录监听器捕获到的变更标记。
/// @return 有待处理目录变更时返回 true。
bool ProjectController::consumeDirectoryChangePending()
{
    return m_projectDirectoryWatcher.consumeChangePending();
}

/// @brief 扫描当前项目目录并同步项目资源列表。
/// @return 目录同步结果，包含是否改变项目和需要预加载的音效。
ProjectResourceService::DirectorySyncResult
ProjectController::scanProjectDirectory()
{
    /// @brief 本次目录扫描同步结果。
    ProjectResourceService::DirectorySyncResult result;
    if ( !m_currentProject ) {
        return result;
    }

    /// @brief 当前项目根目录路径。
    auto actualProjectPath = m_currentProject->m_projectRoot;
    /// @brief 当前项目目录扫描结果。
    ProjectDirectoryScanner::ScanResult directoryScan;

    directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
    if ( !directoryScan.m_success ) {
        return result;
    }

    return m_projectResourceService.syncDirectoryResources(*m_currentProject,
                                                           directoryScan);
}

/// @brief 保存当前项目配置。
/// @return 保存成功时返回 true。
bool ProjectController::saveProject()
{
    if ( !m_currentProject ) return false;

    /// @brief 当前项目描述文件路径。
    std::filesystem::path projectFile =
        m_currentProject->m_projectRoot / "mmm_project.json";
    XINFO("Saving project to {}", Config::pathToUtf8(projectFile));

    if ( !writeProjectFile(*m_currentProject, projectFile) ) {
        return false;
    }

    XINFO("Project saved successfully.");
    Event::EventBus::instance().publish(Event::ProjectSavedEvent{
        .m_projectFilePath = Config::pathToUtf8(projectFile),
    });
    return true;
}

/// @brief 创建谱面文件并登记到当前项目。
/// @param cmd 新建谱面命令。
/// @return 新建谱面的处理结果。
ProjectCommandService::CreateBeatmapResult ProjectController::createBeatmap(
    const CmdCreateBeatmap& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot create beatmap: No project opened.");
        return {};
    }
    return m_projectCommandService.createBeatmap(*m_currentProject, cmd);
}

/// @brief 导入音频文件并登记到当前项目。
/// @param cmd 导入音频命令。
/// @return 导入音频的处理结果。
ProjectCommandService::ImportAudioResult ProjectController::importAudio(
    const CmdImportAudio& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot import audio: No project opened.");
        return {};
    }
    return m_projectCommandService.importAudio(*m_currentProject, cmd);
}

/// @brief 将单个谱面文件同步到当前项目谱面列表。
/// @param mapPath 需要同步的谱面文件路径。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectController::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.syncProjectWithFile(*m_currentProject,
                                                       mapPath);
}

/// @brief 更新当前项目内谱面条目的文件路径关联。
/// @param oldPath 旧谱面路径。
/// @param newPath 新谱面路径。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectController::updateBeatmapFilePath(const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateBeatmapFilePath(
        *m_currentProject, oldPath, newPath);
}

/// @brief 更新当前项目的音频资源类型。
/// @param cmd 更新音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 更新音频资源的处理结果。
ProjectCommandService::UpdateAudioResourceResult
ProjectController::updateAudioResource(
    const CmdUpdateAudioResource&             cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateAudioResource(
        *m_currentProject, cmd, openBeatmapReferences);
}

/// @brief 从当前项目中删除音频资源。
/// @param cmd 删除音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 删除音频资源的处理结果。
ProjectCommandService::RemoveAudioResourceResult
ProjectController::removeAudioResource(
    const CmdRemoveAudioResource&             cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeAudioResource(
        *m_currentProject, cmd, openBeatmapReferences);
}

/// @brief 从当前项目谱面列表中删除谱面。
/// @param cmd 删除谱面命令。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult ProjectController::removeBeatmap(
    const CmdRemoveBeatmap& cmd)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeBeatmap(*m_currentProject, cmd);
}

}  // namespace MMM::Logic
