#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include <stb_image.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <miniz.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
/// @brief Resolve a stored metadata path through the current project if
/// present.
std::filesystem::path resolveCurrentProjectPath(
    const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( project ) {
        return (project->m_projectRoot / path).lexically_normal();
    }
    return path.lexically_normal();
}

/// @brief Store a filesystem path as a project-relative metadata path when
/// possible.
std::filesystem::path makeCurrentProjectRelativePath(
    const std::filesystem::path& path)
{
    if ( path.empty() ) return {};
    if ( path.is_relative() ) return path.lexically_normal();

    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return path.lexically_normal();

    std::error_code ec;
    auto root = std::filesystem::absolute(project->m_projectRoot, ec);
    if ( ec ) return path.filename();

    auto relativePath = std::filesystem::relative(path, root, ec);
    if ( !ec && !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }
    return path.filename();
}

/// @brief Normalize long-lived beatmap metadata paths for project storage.
void normalizeCurrentProjectMetadataPaths(MMM::BaseMapMeta& meta)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    meta.map_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.map_path));
    meta.main_audio_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_audio_path));
    meta.main_cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_cover_path));
    meta.cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.cover_path));
}

/// @brief 计算谱面文件的 FNV-1a 64 位哈希。
/// @param path 待读取文件路径。
/// @return 成功时返回哈希值，文件不可读时返回空。
std::optional<std::uint64_t> calculateBeatmapFileHash(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(path, filesystemError) ||
         filesystemError ) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if ( !file ) return std::nullopt;

    constexpr std::uint64_t fnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t fnvPrime  = 1099511628211ull;

    std::uint64_t               hash = fnvOffset;
    std::array<char, 64 * 1024> buffer{};
    while ( file ) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = file.gcount();
        for ( std::streamsize index = 0; index < bytesRead; ++index ) {
            hash ^= static_cast<std::uint8_t>(static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index)]));
            hash *= fnvPrime;
        }
    }

    if ( file.bad() ) return std::nullopt;
    return hash;
}

/// @brief 生成保存哈希缓存使用的路径键。
/// @param path 目标文件路径。
/// @return 规范化后的 UTF-8 路径键。
std::string makeBeatmapFileHashPathKey(const std::filesystem::path& path)
{
    return MMM::Config::pathToUtf8Generic(path.lexically_normal());
}

/// @brief 刷新会话中记录的单个谱面文件哈希。
/// @param savedBeatmapFileHashes 当前会话的谱面文件哈希缓存。
/// @param path 已加载或已成功保存的谱面路径。
void rememberBeatmapFileHash(
    std::unordered_map<std::string, std::uint64_t>& savedBeatmapFileHashes,
    const std::filesystem::path&                    path)
{
    if ( path.empty() ) return;

    const std::string key = makeBeatmapFileHashPathKey(path);
    if ( key.empty() ) return;

    if ( auto hash = calculateBeatmapFileHash(path) ) {
        savedBeatmapFileHashes[key] = *hash;
    } else {
        savedBeatmapFileHashes.erase(key);
    }
}

/// @brief 判断强制 MMM 保存是否需要用户确认覆盖。
/// @param settings 当前编辑器设置。
/// @param savedBeatmapFileHashes 当前会话的谱面文件哈希缓存。
/// @param cmd 保存命令。
/// @param savePath 本次实际写出的目标路径。
/// @return 需要确认时返回 true。
bool shouldConfirmForcedMmmOverwrite(
    const MMM::Config::EditorSettings& settings,
    const std::unordered_map<std::string, std::uint64_t>&
                                      savedBeatmapFileHashes,
    const MMM::Logic::CmdSaveBeatmap& cmd,
    const std::filesystem::path&      savePath)
{
    if ( cmd.allowExternallyModifiedOverwrite ) return false;
    if ( settings.saveFormatPreference !=
         MMM::Config::SaveFormatPreference::ForceMMM ) {
        return false;
    }

    std::error_code filesystemError;
    if ( !std::filesystem::exists(savePath, filesystemError) ||
         filesystemError ) {
        return false;
    }

    auto currentHash = calculateBeatmapFileHash(savePath);
    if ( !currentHash ) return true;

    const auto hashIt =
        savedBeatmapFileHashes.find(makeBeatmapFileHashPathKey(savePath));
    return hashIt == savedBeatmapFileHashes.end() ||
           hashIt->second != *currentHash;
}

/// @brief 格式化无快照上下文的状态栏时间文本。
std::string formatStatusTime(double timeSeconds)
{
    auto preference = MMM::Config::AppConfig::instance()
                          .getEditorSettings()
                          .timeFormatPreference;
    switch ( preference ) {
    case MMM::Config::TimeFormatPreference::Clock: {
        bool    negative = timeSeconds < 0.0;
        double  absTime  = std::abs(timeSeconds);
        auto    totalMs  = static_cast<int64_t>(std::llround(absTime * 1000.0));
        int64_t ms       = totalMs % 1000;
        int64_t seconds  = (totalMs / 1000) % 60;
        int64_t minutes  = (totalMs / 60000) % 60;
        int64_t hours    = totalMs / 3600000;
        return fmt::format("{}{:02}:{:02}:{:02}.{:03}",
                           negative ? "-" : "",
                           hours,
                           minutes,
                           seconds,
                           ms);
    }
    case MMM::Config::TimeFormatPreference::Milliseconds:
        return fmt::format(
            "{} ms", static_cast<int64_t>(std::llround(timeSeconds * 1000.0)));
    case MMM::Config::TimeFormatPreference::Beat:
    case MMM::Config::TimeFormatPreference::Seconds:
    default: return fmt::format("{:.3f} s", timeSeconds);
    }
}

/// @brief 判断项目相对路径是否包含越界片段。
/// @param relativePath 待检查的相对路径。
/// @return 路径是否会逃逸项目根目录。
bool packageRelativePathEscapesRoot(const std::filesystem::path& relativePath)
{
    if ( relativePath.empty() || relativePath.is_absolute() ||
         relativePath.has_root_name() ) {
        return true;
    }
    const auto normalizedPath = relativePath.lexically_normal();
    for ( const auto& part : normalizedPath ) {
        if ( part == std::filesystem::path("..") ) return true;
    }
    return false;
}

/// @brief 读取完整二进制文件。
/// @param path 待读取文件路径。
/// @param outBytes 输出文件字节。
/// @return 是否读取成功。
bool readPackageSourceFile(const std::filesystem::path& path,
                           std::vector<std::uint8_t>&   outBytes)
{
    outBytes.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;

    const auto fileSize = file.tellg();
    if ( fileSize < 0 ) return false;
    file.seekg(0, std::ios::beg);

    outBytes.resize(static_cast<std::size_t>(fileSize));
    if ( outBytes.empty() ) return true;

    file.read(reinterpret_cast<char*>(outBytes.data()),
              static_cast<std::streamsize>(fileSize));
    return file.good();
}

/// @brief 向指定路径写入二进制文件。
/// @param path 输出文件路径。
/// @param data 待写入数据指针。
/// @param size 待写入字节数。
/// @return 是否写入成功。
bool writePackageOutputFile(const std::filesystem::path& path, const void* data,
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
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return file.good();
}

/// @brief 取得打包格式要求的主谱面扩展名。
/// @param packageTypes 输出包格式对应的文件类型规则。
/// @return 带前导点的目标谱面扩展名。
std::string getPackageBeatmapOutputExtension(
    const MMM::PackageSupportedFileTypes& packageTypes)
{
    if ( packageTypes.m_beatmapExtensions.empty() ) return ".mmm";
    return std::string(packageTypes.m_beatmapExtensions.front());
}

/// @brief 判断谱面来源是否需要转换成当前包格式的谱面文件。
/// @param sourceExtension 来源文件扩展名。
/// @param outputExtension 目标谱面扩展名。
/// @return 是否需要在打包前转换。
bool shouldConvertPackageBeatmapSource(const std::string& sourceExtension,
                                       const std::string& outputExtension)
{
    return MMM::isKnownPackageResourceExtension(
               MMM::PackageResourceType::Beatmap, sourceExtension) &&
           !MMM::packageExtensionEquals(sourceExtension, outputExtension);
}

/// @brief 生成临时转换谱面文件路径。
/// @param sourcePath 来源谱面路径。
/// @param outputExtension 转换后的谱面扩展名。
/// @return 临时文件路径，失败时为空路径。
std::filesystem::path makeTemporaryConvertedBeatmapPath(
    const std::filesystem::path& sourcePath, const std::string& outputExtension)
{
    std::error_code filesystemError;
    auto tempRoot = std::filesystem::temp_directory_path(filesystemError);
    if ( filesystemError || tempRoot.empty() ) return {};

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path fileName = sourcePath.stem();
    if ( fileName.empty() ) fileName = "mmm_package_map";
    fileName += "_converted_";
    fileName += std::to_string(stamp);
    fileName += outputExtension;
    return (tempRoot / fileName).lexically_normal();
}

/// @brief 将谱面源文件转换成指定路径的目标格式文件。
/// @param sourcePath 来源谱面路径。
/// @param outputPath 转换后输出路径。
/// @param metadataOverride 转换时覆盖的基础谱面元数据；为空则使用源谱面元数据。
/// @return 是否转换成功。
bool convertPackageBeatmapFile(const std::filesystem::path& sourcePath,
                               const std::filesystem::path& outputPath,
                               const MMM::BaseMapMeta*      metadataOverride)
{
    auto beatMap = MMM::BeatMap::loadFromFile(sourcePath);
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) return false;
    if ( metadataOverride ) {
        beatMap.m_baseMapMetadata = *metadataOverride;
    }
    beatMap.m_baseMapMetadata.map_path = outputPath;
    return beatMap.saveToFile(outputPath);
}

/// @brief 读取转换后的目标谱面字节。
/// @param sourcePath 来源谱面路径。
/// @param projectOutputPath 保存到项目中时使用的目标路径。
/// @param outputExtension 目标谱面扩展名。
/// @param saveToProject 是否将转换产物留在项目目录中。
/// @param metadataOverride 转换时覆盖的基础谱面元数据；为空则使用源谱面元数据。
/// @param outBytes 输出文件字节。
/// @return 是否成功读取转换结果。
bool readConvertedPackageBeatmapBytes(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& projectOutputPath,
    const std::string& outputExtension, bool saveToProject,
    const MMM::BaseMapMeta*    metadataOverride,
    std::vector<std::uint8_t>& outBytes)
{
    const auto conversionPath =
        saveToProject
            ? projectOutputPath
            : makeTemporaryConvertedBeatmapPath(sourcePath, outputExtension);
    if ( conversionPath.empty() ) return false;

    if ( saveToProject ) {
        std::error_code filesystemError;
        const auto      parentPath = conversionPath.parent_path();
        if ( !parentPath.empty() ) {
            std::filesystem::create_directories(parentPath, filesystemError);
            if ( filesystemError ) return false;
        }
    }

    if ( !convertPackageBeatmapFile(
             sourcePath, conversionPath, metadataOverride) ) {
        if ( !saveToProject ) {
            std::error_code removeError;
            std::filesystem::remove(conversionPath, removeError);
        }
        return false;
    }

    const bool readOk = readPackageSourceFile(conversionPath, outBytes);
    if ( !saveToProject ) {
        std::error_code removeError;
        std::filesystem::remove(conversionPath, removeError);
    }
    return readOk;
}

/// @brief 将文件字节写入 zip 包，重复包内路径会自动跳过。
/// @param zipArchive 正在写入的 zip 归档。
/// @param archivedNames 已写入的包内路径集合。
/// @param archiveRelativePath 包内相对路径。
/// @param fileBytes 待写入的文件字节。
/// @param sourceRelativeUtf8 日志中使用的来源项目相对路径。
/// @return 写入成功或因重复路径跳过时返回 true。
bool addPackageArchiveBytes(mz_zip_archive&                  zipArchive,
                            std::unordered_set<std::string>& archivedNames,
                            const std::filesystem::path& archiveRelativePath,
                            const std::vector<std::uint8_t>& fileBytes,
                            const std::string&               sourceRelativeUtf8)
{
    std::string archiveName =
        MMM::Config::pathToUtf8Generic(archiveRelativePath);
    if ( archiveName.empty() ) {
        XERROR("PackBeatmap: empty archive path: {}", sourceRelativeUtf8);
        return false;
    }
    if ( !archivedNames.insert(archiveName).second ) {
        return true;
    }

    const void* fileData = fileBytes.empty() ? nullptr : fileBytes.data();
    if ( !mz_zip_writer_add_mem(&zipArchive,
                                archiveName.c_str(),
                                fileData,
                                fileBytes.size(),
                                MZ_DEFAULT_COMPRESSION) ) {
        XERROR("PackBeatmap: failed to add file to archive: {}",
               sourceRelativeUtf8);
        return false;
    }
    return true;
}

/// @brief 判断包内路径是否已在指定集合中。
/// @param archivedNames 包内路径集合。
/// @param archiveRelativePath 待检查的包内相对路径。
/// @return 已存在时返回 true。
bool hasPackageArchivePath(const std::unordered_set<std::string>& archivedNames,
                           const std::filesystem::path& archiveRelativePath)
{
    const std::string archiveName =
        MMM::Config::pathToUtf8Generic(archiveRelativePath);
    return !archiveName.empty() &&
           archivedNames.find(archiveName) != archivedNames.end();
}

/// @brief 将项目相对路径规范化为用于匹配打包元数据覆盖项的 UTF-8 路径。
/// @param relativePath 项目相对路径。
/// @return 规范化后的通用分隔符路径。
std::string normalizePackageRelativePathKey(
    const std::filesystem::path& relativePath)
{
    return MMM::Config::pathToUtf8Generic(relativePath.lexically_normal());
}

/// @brief 构建打包元数据覆盖项查询表。
/// @param metadataOverrides 命令中携带的元数据覆盖项。
/// @return 项目相对路径到基础元数据的映射。
std::unordered_map<std::string, MMM::BaseMapMeta>
makePackageMetadataOverrideMap(
    const std::vector<MMM::Logic::PackageBeatmapMetadataOverride>&
        metadataOverrides)
{
    std::unordered_map<std::string, MMM::BaseMapMeta> result;
    result.reserve(metadataOverrides.size());
    for ( const auto& metadataOverride : metadataOverrides ) {
        auto relativePath =
            MMM::Config::utf8ToPath(metadataOverride.relativePath);
        result[normalizePackageRelativePathKey(relativePath)] =
            metadataOverride.baseMeta;
    }
    return result;
}

/// @brief 构建已选原始 IMD 谱面在包内的路径集合。
/// @param selectedRelativePaths 需要打包的项目相对路径列表。
/// @return 已选 IMD 源文件对应的包内路径集合。
std::unordered_set<std::string> makeSelectedImdArchiveNameSet(
    const std::vector<std::string>& selectedRelativePaths)
{
    std::unordered_set<std::string> result;
    for ( const auto& relativeUtf8 : selectedRelativePaths ) {
        const auto relativePath =
            MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
        const auto extension =
            MMM::Config::pathToUtf8(relativePath.extension());
        if ( MMM::packageExtensionEquals(extension, ".imd") ) {
            result.insert(MMM::Config::pathToUtf8Generic(relativePath));
        }
    }
    return result;
}

/// @brief 写入 zip 兼容的谱面包。
/// @param projectRoot 当前项目根目录。
/// @param outputPath 输出包路径。
/// @param selectedRelativePaths 需要打包的项目相对路径列表。
/// @param packageTypes 输出包格式对应的文件类型规则。
/// @param metadataOverrides 转换指定谱面时临时覆盖的基础元数据列表。
/// @param saveConvertedBeatmapsToProject 是否将转换后的谱面文件保存回项目目录。
/// @param includeLegacyImdBeatmapsInPackage 是否额外写入旧皮肤兼容的 IMD 谱面。
/// @return 是否打包成功。
bool writeBeatmapPackage(
    const std::filesystem::path&          projectRoot,
    const std::filesystem::path&          outputPath,
    const std::vector<std::string>&       selectedRelativePaths,
    const MMM::PackageSupportedFileTypes& packageTypes,
    const std::vector<MMM::Logic::PackageBeatmapMetadataOverride>&
         metadataOverrides,
    bool saveConvertedBeatmapsToProject, bool includeLegacyImdBeatmapsInPackage)
{
    if ( selectedRelativePaths.empty() ) return false;

    mz_zip_archive zipArchive{};
    if ( !mz_zip_writer_init_heap(&zipArchive, 0, 0) ) {
        return false;
    }

    bool                            success = true;
    std::vector<std::uint8_t>       fileBytes;
    std::unordered_set<std::string> archivedNames;
    const std::string               packageBeatmapExtension =
        getPackageBeatmapOutputExtension(packageTypes);
    const bool includeLegacyImdBeatmaps =
        includeLegacyImdBeatmapsInPackage &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    const auto selectedImdArchiveNames =
        includeLegacyImdBeatmaps
            ? makeSelectedImdArchiveNameSet(selectedRelativePaths)
            : std::unordered_set<std::string>{};
    const auto metadataOverrideMap =
        makePackageMetadataOverrideMap(metadataOverrides);
    for ( const auto& relativeUtf8 : selectedRelativePaths ) {
        const auto relativePath =
            MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
        const auto relativePathKey =
            normalizePackageRelativePathKey(relativePath);
        if ( packageRelativePathEscapesRoot(relativePath) ) {
            XERROR("PackBeatmap: path escapes project root: {}", relativeUtf8);
            success = false;
            break;
        }

        const auto extension =
            MMM::Config::pathToUtf8(relativePath.extension());
        if ( !isPackageCandidateExtensionSupported(packageTypes, extension) ) {
            XERROR("PackBeatmap: unsupported file extension: {}", relativeUtf8);
            success = false;
            break;
        }

        const auto sourcePath = (projectRoot / relativePath).lexically_normal();
        std::error_code filesystemError;
        if ( !std::filesystem::is_regular_file(sourcePath, filesystemError) ||
             filesystemError ) {
            XERROR("PackBeatmap: source file not found: {}", relativeUtf8);
            success = false;
            break;
        }

        const bool isBeatmapSource = MMM::isKnownPackageResourceExtension(
            MMM::PackageResourceType::Beatmap, extension);
        if ( isBeatmapSource ) {
            auto       targetArchivePath = relativePath;
            const bool shouldConvert     = shouldConvertPackageBeatmapSource(
                extension, packageBeatmapExtension);
            if ( shouldConvert ) {
                targetArchivePath.replace_extension(packageBeatmapExtension);
            }

            const auto metadataIt = metadataOverrideMap.find(relativePathKey);
            const MMM::BaseMapMeta* metadataOverride =
                metadataIt == metadataOverrideMap.end() ? nullptr
                                                        : &metadataIt->second;

            if ( !hasPackageArchivePath(archivedNames, targetArchivePath) ) {
                if ( shouldConvert ) {
                    const auto projectOutputPath =
                        (projectRoot / targetArchivePath).lexically_normal();
                    if ( !readConvertedPackageBeatmapBytes(
                             sourcePath,
                             projectOutputPath,
                             packageBeatmapExtension,
                             saveConvertedBeatmapsToProject,
                             metadataOverride,
                             fileBytes) ) {
                        XERROR("PackBeatmap: failed to convert source file: {}",
                               relativeUtf8);
                        success = false;
                        break;
                    }
                } else if ( !readPackageSourceFile(sourcePath, fileBytes) ) {
                    XERROR("PackBeatmap: failed to read source file: {}",
                           relativeUtf8);
                    success = false;
                    break;
                }

                if ( !addPackageArchiveBytes(zipArchive,
                                             archivedNames,
                                             targetArchivePath,
                                             fileBytes,
                                             relativeUtf8) ) {
                    success = false;
                    break;
                }
            }

            if ( includeLegacyImdBeatmaps ) {
                auto imdArchivePath = relativePath;
                imdArchivePath.replace_extension(".imd");
                const bool sourceIsImd =
                    MMM::packageExtensionEquals(extension, ".imd");
                const bool rawImdSelected =
                    !sourceIsImd &&
                    hasPackageArchivePath(selectedImdArchiveNames,
                                          imdArchivePath);
                if ( !rawImdSelected &&
                     !hasPackageArchivePath(archivedNames, imdArchivePath) ) {
                    if ( sourceIsImd ) {
                        if ( !readPackageSourceFile(sourcePath, fileBytes) ) {
                            XERROR(
                                "PackBeatmap: failed to read source file: {}",
                                relativeUtf8);
                            success = false;
                            break;
                        }
                    } else {
                        const auto projectOutputPath =
                            (projectRoot / imdArchivePath).lexically_normal();
                        if ( !readConvertedPackageBeatmapBytes(
                                 sourcePath,
                                 projectOutputPath,
                                 ".imd",
                                 false,
                                 nullptr,
                                 fileBytes) ) {
                            XERROR(
                                "PackBeatmap: failed to convert legacy IMD "
                                "file: {}",
                                relativeUtf8);
                            success = false;
                            break;
                        }
                    }

                    if ( !addPackageArchiveBytes(zipArchive,
                                                 archivedNames,
                                                 imdArchivePath,
                                                 fileBytes,
                                                 relativeUtf8) ) {
                        success = false;
                        break;
                    }
                }
            }
            continue;
        }

        if ( hasPackageArchivePath(archivedNames, relativePath) ) {
            continue;
        }
        if ( !readPackageSourceFile(sourcePath, fileBytes) ) {
            XERROR("PackBeatmap: failed to read source file: {}", relativeUtf8);
            success = false;
            break;
        }

        if ( !addPackageArchiveBytes(zipArchive,
                                     archivedNames,
                                     relativePath,
                                     fileBytes,
                                     relativeUtf8) ) {
            success = false;
            break;
        }
    }

    void*       archiveBuffer = nullptr;
    std::size_t archiveSize   = 0;
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &zipArchive, &archiveBuffer, &archiveSize) ) {
        success = false;
    }

    mz_zip_writer_end(&zipArchive);

    if ( success ) {
        success =
            writePackageOutputFile(outputPath, archiveBuffer, archiveSize);
    }
    if ( archiveBuffer ) {
        mz_free(archiveBuffer);
    }
    return success;
}
}  // namespace

namespace MMM::Logic
{

bool BeatmapSession::processCommands()
{
    LogicCommand cmd;
    bool         processed = false;
    while ( m_commandQueue.try_dequeue(cmd) ) {
        std::visit(
            [this, &processed](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr ( !std::is_same_v<T, CmdSetMousePosition> &&
                               !std::is_same_v<T, CmdSetHoveredEntity> ) {
                    processed = true;
                }
                if constexpr ( std::is_same_v<T, CmdUndo> ||
                               std::is_same_v<T, CmdRedo> ||
                               std::is_same_v<T, CmdLoadBeatmap> ||
                               std::is_same_v<T, CmdCreateBeatmap> ||
                               std::is_same_v<T, CmdRemoveBeatmap> ||
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                               std::is_same_v<T, CmdUpdateTimelineEvent> ||
                               std::is_same_v<T, CmdDeleteTimelineEvent> ||
                               std::is_same_v<T, CmdCreateTimelineEvent> ||
                               std::is_same_v<T, CmdCreateTimelineEvents> ||
                               std::is_same_v<T, CmdReplaceBeatmapTimings> ||
                               std::is_same_v<T, CmdReplaceBeatmapData> ) {
                    m_ctx->isTransformDirty = true;
                }

                // --- 自动更新操作状态描述 ---
                if constexpr ( std::is_same_v<T, CmdChangeTool> ) {
                    std::string toolName = TR("ui.status.ready").pStr;
                    switch ( arg.tool ) {
                    case EditTool::Move:
                        toolName = TR("ui.status.tool.select_move").pStr;
                        break;
                    case EditTool::Marquee:
                        toolName = TR("ui.status.tool.marquee").pStr;
                        break;
                    case EditTool::Draw:
                        toolName = TR("ui.status.tool.draw_brush").pStr;
                        break;
                    case EditTool::ColorBrush:
                        toolName = TR("ui.status.tool.color_brush").pStr;
                        break;
                    case EditTool::ColorEraser:
                        toolName = TR("ui.status.tool.color_eraser").pStr;
                        break;
                    }
                    m_ctx->lastActionMessage = fmt::format(
                        "{} {}", TR("ui.status.category.tool"), toolName);
                } else if constexpr ( std::is_same_v<T, CmdLoadBeatmap> ) {
                    if ( arg.beatmap ) {
                        m_ctx->lastActionMessage =
                            fmt::format("{} {}: {} [{}]",
                                        TR("ui.status.category.beatmap"),
                                        TR("ui.status.beatmap.loaded"),
                                        arg.beatmap->m_baseMapMetadata.name,
                                        arg.beatmap->m_baseMapMetadata.version);
                    } else {
                        m_ctx->lastActionMessage =
                            fmt::format("{} {}",
                                        TR("ui.status.category.beatmap"),
                                        TR("ui.status.beatmap.no_load"));
                    }
                } else if constexpr ( std::is_same_v<T, CmdSaveBeatmap> ||
                                      std::is_same_v<T, CmdSaveBeatmapAs> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.beatmap"),
                                    TR("ui.status.beatmap.saved"));
                } else if constexpr ( std::is_same_v<T, CmdMirrorSelected> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.action"),
                                    TR("ui.edit.mirror"));
                } else if constexpr ( std::is_same_v<
                                          T,
                                          CmdAlignSelectedToCommonBeats> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.action"),
                                    TR("ui.tools.align_beats"));
                } else if constexpr ( std::is_same_v<T, CmdSeek> ) {
                    const auto timeText = formatStatusTime(arg.time);
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.playback"),
                                    TR("ui.status.playback.seek"),
                                    timeText);
                } else if constexpr ( std::is_same_v<T, CmdSetPlaybackSpeed> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}: {:.2f}x",
                                    TR("ui.status.category.playback"),
                                    TR("ui.status.playback.speed"),
                                    arg.speed);
                } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.project"),
                                    TR("ui.status.project.track_count"),
                                    arg.trackCount);
                } else if constexpr ( std::is_same_v<T, CmdSelectAll> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.selection"),
                                    TR("ui.status.selection.all_selected"));
                }

                // --- Session 自己处理的命令 ---
                if constexpr ( std::is_same_v<T, CmdUpdateEditorConfig> ||
                               std::is_same_v<T, CmdUpdateViewport> ||
                               std::is_same_v<T, CmdLoadBeatmap> ||
                               std::is_same_v<T, CmdSaveBeatmap> ||
                               std::is_same_v<T, CmdSaveBeatmapAs> ||
                               std::is_same_v<T, CmdPackBeatmap> ||
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ) {
                    this->handleCommand(arg);
                }
                // --- Playback 处理的命令 ---
                else if constexpr ( std::is_same_v<T, CmdSetPlayState> ||
                                    std::is_same_v<T, CmdSeek> ||
                                    std::is_same_v<T, CmdSetPlaybackSpeed> ||
                                    std::is_same_v<T, CmdScroll> ) {
                    m_playback->handleCommand(arg);
                }
                // --- Interaction 处理的命令 ---
                else if constexpr ( std::is_same_v<T, CmdSetHoveredEntity> ||
                                    std::is_same_v<T, CmdSelectEntity> ||
                                    std::is_same_v<T, CmdStartDrag> ||
                                    std::is_same_v<T, CmdUpdateDrag> ||
                                    std::is_same_v<T, CmdEndDrag> ||
                                    std::is_same_v<T, CmdChangeTool> ||
                                    std::is_same_v<T, CmdSetMousePosition> ||
                                    std::is_same_v<T, CmdUpdateTrackCount> ||
                                    std::is_same_v<T, CmdSetBrushNoteColor> ||
                                    std::is_same_v<T, CmdSetBrushNotePalette> ||
                                    std::is_same_v<T, CmdStartMarquee> ||
                                    std::is_same_v<T, CmdUpdateMarquee> ||
                                    std::is_same_v<T, CmdEndMarquee> ||
                                    std::is_same_v<T, CmdRemoveMarqueeAt> ||
                                    std::is_same_v<T, CmdStartBrush> ||
                                    std::is_same_v<T, CmdUpdateBrush> ||
                                    std::is_same_v<T, CmdEndBrush> ||
                                    std::is_same_v<T, CmdStartErase> ||
                                    std::is_same_v<T, CmdUpdateErase> ||
                                    std::is_same_v<T, CmdEndErase> ||
                                    std::is_same_v<T, CmdSelectAll> ) {
                    m_interaction->handleCommand(arg);
                }
                // --- Action 处理的命令 ---
                else if constexpr (
                    std::is_same_v<T, CmdUndo> || std::is_same_v<T, CmdRedo> ||
                    std::is_same_v<T, CmdCopy> || std::is_same_v<T, CmdCut> ||
                    std::is_same_v<T, CmdPaste> ||
                    std::is_same_v<T, CmdUpdateTimelineEvent> ||
                    std::is_same_v<T, CmdDeleteTimelineEvent> ||
                    std::is_same_v<T, CmdCreateTimelineEvents> ||
                    std::is_same_v<T, CmdReplaceBeatmapTimings> ||
                    std::is_same_v<T, CmdReplaceBeatmapData> ||
                    std::is_same_v<T, CmdApplyNoteColorToSelection> ||
                    std::is_same_v<T, CmdApplyNotePaletteToSelection> ||
                    std::is_same_v<T, CmdApplyBrushPaletteToEntity> ||
                    std::is_same_v<T, CmdClearNoteColorOverrides> ||
                    std::is_same_v<T, CmdDeleteSelected> ||
                    std::is_same_v<T, CmdMirrorSelected> ||
                    std::is_same_v<T, CmdAlignSelectedToCommonBeats> ||
                    std::is_same_v<T, CmdCreateTimelineEvent> ) {
                    m_actions->handleCommand(arg);
                }
            },
            cmd);
    }
    return processed;
}

// --- Session 自己处理的 ---

void BeatmapSession::handleCommand(const CmdUpdateEditorConfig& cmd)
{
    m_ctx->lastConfig = cmd.config;
    auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        cache->isDirty = true;
    }
}

void BeatmapSession::handleCommand(const CmdUpdateViewport& cmd)
{
    if ( m_ctx->cameras.find(cmd.cameraId) == m_ctx->cameras.end() ) {
        m_ctx->cameras[cmd.cameraId] =
            CameraInfo{ cmd.cameraId, cmd.width, cmd.height };
    } else {
        m_ctx->cameras[cmd.cameraId].viewportWidth  = cmd.width;
        m_ctx->cameras[cmd.cameraId].viewportHeight = cmd.height;
    }
}

void BeatmapSession::handleCommand(const CmdLoadBeatmap& cmd)
{
    SessionUtils::loadBeatmap(*m_ctx, cmd.beatmap);
    m_savedBeatmapFileHashes.clear();
    if ( m_ctx->currentBeatmap ) {
        rememberBeatmapFileHash(
            m_savedBeatmapFileHashes,
            resolveCurrentProjectPath(
                m_ctx->currentBeatmap->m_baseMapMetadata.map_path));
    }
    m_ctx->isBpmEventsDirty = true;
}

void BeatmapSession::handleCommand(const CmdSaveBeatmap& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        auto oldPath  = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
        auto savePath = resolveCurrentProjectPath(oldPath);
        if ( m_ctx->lastConfig.settings.saveFormatPreference ==
             Config::SaveFormatPreference::ForceMMM ) {
            savePath.replace_extension(".mmm");
        }
        if ( shouldConfirmForcedMmmOverwrite(m_ctx->lastConfig.settings,
                                             m_savedBeatmapFileHashes,
                                             cmd,
                                             savePath) ) {
            Event::EventBus::instance().publish(Event::BeatmapSaveConflictEvent{
                .path = Config::pathToUtf8(savePath),
            });
            return;
        }

        m_ctx->m_needsTimingsSync = true;
        m_ctx->m_needsNotesSync   = true;
        SessionUtils::syncBeatmap(*m_ctx);
        SessionUtils::ensureHitEvents(*m_ctx);

        bool ok = m_ctx->currentBeatmap->saveToFile(savePath);
        if ( !ok ) {
            XERROR("SaveBeatmap: failed to save to {}",
                   Config::pathToUtf8(savePath));
            Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
                .path     = Config::pathToUtf8(savePath),
                .success  = false,
                .isExport = false,
            });
            return;
        }
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = Config::pathToUtf8(savePath),
            .success  = true,
            .isExport = false,
        });
        auto storedSavePath = makeCurrentProjectRelativePath(savePath);
        m_ctx->currentBeatmap->m_baseMapMetadata.map_path = storedSavePath;
        rememberBeatmapFileHash(m_savedBeatmapFileHashes, savePath);
        m_ctx->actionStack.markSaved();
        if ( oldPath != storedSavePath ) {
            EditorEngine::instance().updateBeatmapFilePathInProject(
                oldPath, storedSavePath);
        } else {
            EditorEngine::instance().syncProjectWithFile(savePath);
        }
    }
}

/// @brief 将当前谱面导出到指定路径，不接管当前会话的谱面路径或保存状态。
void BeatmapSession::handleCommand(const CmdSaveBeatmapAs& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        m_ctx->m_needsTimingsSync = true;
        m_ctx->m_needsNotesSync   = true;
        SessionUtils::syncBeatmap(*m_ctx);
        SessionUtils::ensureHitEvents(*m_ctx);
        auto savePath = resolveCurrentProjectPath(Config::utf8ToPath(cmd.path));
        bool ok       = m_ctx->currentBeatmap->saveToFile(savePath);
        if ( !ok ) {
            XERROR("SaveBeatmapAs: failed to save to {}", cmd.path);
            Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
                .path     = Config::pathToUtf8(savePath),
                .success  = false,
                .isExport = true,
            });
            return;
        }
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = Config::pathToUtf8(savePath),
            .success  = true,
            .isExport = true,
        });

        // 导出到项目目录时刷新项目资源列表，但不切换当前会话的谱面文件。
        EditorEngine::instance().syncProjectWithFile(savePath);
    }
}

void BeatmapSession::handleCommand(const CmdPackBeatmap& cmd)
{
    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) {
        XERROR("PackBeatmap: no project is opened");
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

    const auto  outputPath   = Config::utf8ToPath(cmd.exportPath);
    const auto  extension    = Config::pathToUtf8(outputPath.extension());
    const auto* packageTypes = findPackageSupportedFileTypes(extension);
    if ( !packageTypes ) {
        XERROR("PackBeatmap: unsupported package extension: {}",
               cmd.exportPath);
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

    const bool success =
        writeBeatmapPackage(project->m_projectRoot,
                            outputPath,
                            cmd.selectedProjectRelativePaths,
                            *packageTypes,
                            cmd.metadataOverrides,
                            cmd.saveConvertedBeatmapsToProject,
                            cmd.includeLegacyImdBeatmapsInPackage);
    if ( success ) {
        XINFO("PackBeatmap: package written to {}", cmd.exportPath);
    } else {
        XERROR("PackBeatmap: failed to write package {}", cmd.exportPath);
    }

    Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
        .path     = cmd.exportPath,
        .success  = success,
        .isExport = true,
    });
}

void BeatmapSession::handleCommand(const CmdUpdateBeatmapMetadata& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        auto oldMap = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
        auto oldAudio =
            m_ctx->currentBeatmap->m_baseMapMetadata.main_audio_path;
        auto oldCover =
            m_ctx->currentBeatmap->m_baseMapMetadata.main_cover_path;
        auto oldBPM   = m_ctx->currentBeatmap->m_baseMapMetadata.preference_bpm;
        auto oldTrack = m_ctx->currentBeatmap->m_baseMapMetadata.track_count;
        auto updatedMeta = cmd.baseMeta;
        normalizeCurrentProjectMetadataPaths(updatedMeta);

        m_ctx->currentBeatmap->m_baseMapMetadata = updatedMeta;
        XINFO("BeatmapSession: Metadata updated for {}",
              m_ctx->currentBeatmap->m_baseMapMetadata.name);
        if ( oldMap != updatedMeta.map_path ||
             oldAudio != updatedMeta.main_audio_path ) {
            EditorEngine::instance().refreshMainAudioSyncKeys();
        }

        // 同步轨道数到上下文，确保渲染实时更新
        m_ctx->trackCount = updatedMeta.track_count;

        // 如果关键渲染参数发生变化，刷新 ScrollCache
        if ( oldBPM != updatedMeta.preference_bpm ||
             oldTrack != updatedMeta.track_count ) {
            XINFO(
                "BeatmapSession: Critical metadata changed, dirtying "
                "ScrollCache...");
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                cache->isDirty = true;
            }
            m_ctx->isBpmEventsDirty = true;
        }

        // 如果音频路径发生变化，重新加载音频
        if ( oldAudio != updatedMeta.main_audio_path ) {
            XINFO("BeatmapSession: Audio path changed, reloading...");
            m_ctx->loadedMainAudioPath.clear();
            m_ctx->mainAudioTotalTime = 0.0;
            // 如果当前正在播放，先暂停播放
            if ( m_ctx->isPlaying ) {
                m_playback->handleCommand(CmdSetPlayState{ false });
            }
            // 复位画布时间为 0.0
            m_playback->handleCommand(CmdSeek{ 0.0 });
            auto* project = EditorEngine::instance().getCurrentProject();
            std::filesystem::path audioPath =
                SessionUtils::resolveMainAudioPath(*m_ctx, project);
            if ( std::filesystem::exists(audioPath) ) {
                // 查找对应的 AudioResource 配置
                AudioTrackConfig config;
                if ( project ) {
                    auto fileName = Config::pathToUtf8(
                        updatedMeta.main_audio_path.filename());
                    auto fullPathStr =
                        Config::pathToUtf8(updatedMeta.main_audio_path);

                    for ( const auto& res : project->m_audioResources ) {
                        if ( res.m_id == fileName ||
                             res.m_path == fullPathStr ) {
                            config = res.m_config;
                            break;
                        }
                    }
                }
                const std::string audioPathUtf8 = Config::pathToUtf8(audioPath);
                auto&             audio = Audio::AudioManager::instance();
                if ( audio.loadBGM(audioPathUtf8, config) ) {
                    m_ctx->loadedMainAudioPath = audioPathUtf8;
                    m_ctx->mainAudioTotalTime  = audio.getTotalTime();
                }
            } else {
                XERROR(
                    "BeatmapSession: Audio file does not exist at resolved "
                    "path: {}",
                    Config::pathToUtf8(audioPath));
            }
        }

        // 如果封面路径发生变化，更新背景图尺寸
        if ( oldCover != updatedMeta.main_cover_path ) {
            std::filesystem::path bgPath;
            auto* project = EditorEngine::instance().getCurrentProject();
            if ( project ) {
                bgPath = project->m_projectRoot / updatedMeta.main_cover_path;
            } else {
                bgPath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path
                             .parent_path() /
                         updatedMeta.main_cover_path;
            }
            if ( std::filesystem::exists(bgPath) ) {
                int w = 0, h = 0, comp = 0;
                if ( stbi_info(
                         Config::pathToUtf8(bgPath).c_str(), &w, &h, &comp) ) {
                    m_ctx->bgSize =
                        glm::vec2(static_cast<float>(w), static_cast<float>(h));
                }
            } else {
                m_ctx->bgSize = glm::vec2(0.0f);
            }
        }

        // 同步修改到项目入口列表中，确保侧边栏等 UI 实时更新
        auto* project = EditorEngine::instance().getCurrentProject();
        if ( project ) {
            for ( auto& entry : project->m_beatmaps ) {
                auto fullEntryPath = project->m_projectRoot /
                                     Config::utf8ToPath(entry.m_filePath);

                auto updatedMapPath =
                    resolveCurrentProjectPath(updatedMeta.map_path);
                std::error_code pathEc;
                if ( std::filesystem::exists(fullEntryPath, pathEc) &&
                     std::filesystem::equivalent(
                         fullEntryPath, updatedMapPath, pathEc) ) {
                    entry.m_name         = updatedMeta.version;
                    entry.m_audioTrackId = Config::pathToUtf8(
                        updatedMeta.main_audio_path.filename());
                    XINFO(
                        "BeatmapSession: Synced name '{}' and audioTrackId "
                        "'{}' to project entry",
                        entry.m_name,
                        entry.m_audioTrackId);
                    EditorEngine::instance().saveProject();
                    break;
                }
            }
        }
    }
}

}  // namespace MMM::Logic
