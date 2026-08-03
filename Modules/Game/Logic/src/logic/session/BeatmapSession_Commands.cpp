#include "logic/BeatmapSession.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapLoadDiagnosticPublisher.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
/// @brief 在存在当前项目时，将元数据路径解析为项目内路径。
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

/// @brief 尽量将文件系统路径保存为当前项目相对元数据路径。
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

/// @brief 按当前项目资源刷新 Malody song.file 提示并清除旧单音轨字段。
/// @param beatMap 保存或导出前需要更新的谱面。
/// @note 只更新提示字段，不创建或移动任何自动采样。
void refreshCurrentProjectSongFileHint(MMM::BeatMap& beatMap)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        beatMap.m_baseMapMetadata.main_audio_path.clear();
        return;
    }
    (void)MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
        *project, beatMap, beatMap.m_baseMapMetadata.map_path);
}

/// @brief 将长期保存的谱面元数据路径规范化为项目存储路径。
void normalizeCurrentProjectMetadataPaths(MMM::BaseMapMeta& meta)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    meta.map_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.map_path));
    meta.main_audio_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_audio_path));
    meta.song_file_hint = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.song_file_hint));
    meta.main_cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_cover_path));
    meta.cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.cover_path));
}

/// @brief 判断两份基础谱面元数据是否完全一致。
/// @param lhs 左侧元数据。
/// @param rhs 右侧元数据。
/// @return 所有基础字段都一致时返回 true。
bool baseMapMetadataEqual(const MMM::BaseMapMeta& lhs,
                          const MMM::BaseMapMeta& rhs)
{
    return lhs.name == rhs.name && lhs.title == rhs.title &&
           lhs.title_unicode == rhs.title_unicode && lhs.artist == rhs.artist &&
           lhs.artist_unicode == rhs.artist_unicode &&
           lhs.map_path == rhs.map_path &&
           lhs.main_audio_path == rhs.main_audio_path &&
           lhs.song_file_hint == rhs.song_file_hint &&
           lhs.main_cover_path == rhs.main_cover_path &&
           lhs.cover_path == rhs.cover_path &&
           lhs.cover_type == rhs.cover_type &&
           lhs.video_starttime == rhs.video_starttime &&
           lhs.bgxoffset == rhs.bgxoffset && lhs.bgyoffset == rhs.bgyoffset &&
           lhs.version == rhs.version && lhs.author == rhs.author &&
           lhs.preference_bpm == rhs.preference_bpm &&
           lhs.track_count == rhs.track_count &&
           lhs.bgm_track_count == rhs.bgm_track_count &&
           lhs.map_length == rhs.map_length;
}

/// @brief 在提交玩家轨道数变化前验证全部自动采样的绝对轨道迁移。
/// @param ctx 当前会话上下文。
/// @param oldTrackCount 当前玩家轨道数。
/// @param newTrackCount 目标玩家轨道数。
/// @param error 验证失败时写入的用户可读原因。
/// @return 全部采样都能保持 BGM 相对轨道且目标索引可表示时返回 true。
bool validateSampleTrackCountMigration(const MMM::Logic::SessionContext& ctx,
                                       std::int32_t oldTrackCount,
                                       std::int32_t newTrackCount,
                                       std::string& error)
{
    if ( oldTrackCount <= 0 || newTrackCount <= 0 ) {
        error =
            fmt::format("无法将玩家轨道数从 {} 调整为 {}：玩家轨道数必须为正数",
                        oldTrackCount,
                        newTrackCount);
        return false;
    }

    const auto oldTrackCountUnsigned =
        static_cast<std::uint32_t>(oldTrackCount);
    const auto sampleView =
        ctx.sampleRegistry.view<const MMM::Logic::SampleComponent>();
    for ( const auto entity : sampleView ) {
        const auto& sample =
            sampleView.get<const MMM::Logic::SampleComponent>(entity);
        if ( sample.m_track < oldTrackCountUnsigned ) {
            error = fmt::format(
                "无法将玩家轨道数从 {} 调整为 {}：自动采样轨道 {} "
                "落入玩家轨道区",
                oldTrackCount,
                newTrackCount,
                sample.m_track);
            return false;
        }

        const auto bgmTrack = static_cast<std::uint64_t>(sample.m_track) -
                              static_cast<std::uint64_t>(oldTrackCountUnsigned);
        const auto migratedTrack =
            static_cast<std::uint64_t>(newTrackCount) + bgmTrack;
        if ( migratedTrack > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::uint32_t>::max()) ) {
            error = fmt::format(
                "无法将玩家轨道数从 {} 调整为 {}：自动采样轨道 {} "
                "迁移后超出可表示范围",
                oldTrackCount,
                newTrackCount,
                sample.m_track);
            return false;
        }
    }
    return true;
}

/// @brief 将已成功保存的谱面基础信息同步到项目谱面入口。
/// @param metadata 已成功写入谱面文件的基础元数据。
/// @return 项目入口的名称发生变化时返回 true。
bool syncSavedMetadataToProjectEntry(const MMM::BaseMapMeta& metadata)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return false;

    const auto savedMapPath = resolveCurrentProjectPath(metadata.map_path);
    for ( auto& entry : project->m_beatmaps ) {
        const auto entryPath =
            project->m_projectRoot / MMM::Config::utf8ToPath(entry.m_filePath);
        std::error_code pathError;
        const bool      isSavedEntry =
            std::filesystem::exists(entryPath, pathError) && !pathError &&
            std::filesystem::equivalent(entryPath, savedMapPath, pathError) &&
            !pathError;
        if ( !isSavedEntry ) continue;

        if ( entry.m_name == metadata.version ) return false;

        entry.m_name = metadata.version;
        XINFO("BeatmapSession: Synced saved name '{}' to project entry",
              entry.m_name);
        return true;
    }
    return false;
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

/// @brief 构建 EX Rhythm Master VI 上架用 Malody mode_ext。
/// @return 固定的 mode_ext JSON 对象。
nlohmann::json makeMalodyStoreModeExtJson()
{
    nlohmann::json modeExt = nlohmann::json::object();
    modeExt["bar_begin"]   = 0;
    modeExt["freenote"]    = "请访问商店或官网下载最新EX Rhythm Master VI皮肤";
    modeExt["skinid"]      = 6091;
    return modeExt;
}

/// @brief 判断路径是否为 Malody MC 谱面。
/// @param path 待检查路径。
/// @return 扩展名为 .mc 时返回 true。
bool isMalodyChartPath(const std::filesystem::path& path)
{
    return MMM::packageExtensionEquals(
        MMM::Config::pathToUtf8(path.extension()), ".mc");
}

/// @brief 临时写入上架用 mode_ext 到谱面元数据。
/// @param beatMap 待导出的谱面。
void applyMalodyStoreModeExtMetadata(MMM::BeatMap& beatMap)
{
    beatMap.m_metadata
        .map_properties[MMM::MapMetadataType::MALODY]["mode_ext"] =
        makeMalodyStoreModeExtJson().dump();
}

/// @brief 在 MC JSON 文本中替换上架用 mode_ext。
/// @param inputBytes 输入文件字节。
/// @param outBytes 输出文件字节。
/// @return 替换成功时返回 true。
bool patchMalodyStoreModeExtBytes(const std::vector<std::uint8_t>& inputBytes,
                                  std::vector<std::uint8_t>&       outBytes)
{
    std::string text(inputBytes.begin(), inputBytes.end());
    auto        fileData = nlohmann::json::parse(text, nullptr, false, true);
    if ( fileData.is_discarded() || !fileData.is_object() ) {
        return false;
    }

    if ( !fileData.contains("meta") || !fileData["meta"].is_object() ) {
        fileData["meta"] = nlohmann::json::object();
    }
    fileData["meta"]["mode_ext"] = makeMalodyStoreModeExtJson();
    const std::string outputText = fileData.dump(4);
    outBytes.assign(outputText.begin(), outputText.end());
    return true;
}

/// @brief 在已写出的 MC 文件中替换上架用 mode_ext。
/// @param path MC 文件路径。
/// @return 替换成功时返回 true。
bool patchMalodyStoreModeExtFile(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> inputBytes;
    std::vector<std::uint8_t> outputBytes;
    if ( !readPackageSourceFile(path, inputBytes) ) {
        return false;
    }
    if ( !patchMalodyStoreModeExtBytes(inputBytes, outputBytes) ) {
        return false;
    }
    return writePackageOutputFile(
        path,
        outputBytes.empty() ? nullptr : outputBytes.data(),
        outputBytes.size());
}

/// @brief 保存谱面，并仅在 MC 导出期间临时应用用户选择的模式与 mode_ext。
/// @param beatMap 待保存谱面。
/// @param outputPath 输出路径。
/// @param malodyExportMode MC 导出时临时覆盖的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否写入上架 mode_ext。
/// @return 是否保存成功。
bool saveBeatmapWithMalodyExportOptions(
    MMM::BeatMap& beatMap, const std::filesystem::path& outputPath,
    std::optional<MMM::MalodyMode> malodyExportMode,
    bool                           addStoreModeExtForMalodyExport)
{
    refreshCurrentProjectSongFileHint(beatMap);
    const bool shouldAddStoreModeExt =
        addStoreModeExtForMalodyExport &&
        (!malodyExportMode || *malodyExportMode == MMM::MalodyMode::Slide);
    if ( !isMalodyChartPath(outputPath) ||
         (!malodyExportMode && !shouldAddStoreModeExt) ) {
        return beatMap.saveToFile(outputPath);
    }

    auto previousPropsIt =
        beatMap.m_metadata.map_properties.find(MMM::MapMetadataType::MALODY);
    using MalodyPropertyMap =
        decltype(beatMap.m_metadata.map_properties)::mapped_type;
    std::optional<MalodyPropertyMap> previousProps;
    if ( previousPropsIt != beatMap.m_metadata.map_properties.end() ) {
        previousProps = previousPropsIt->second;
    }

    auto& props =
        beatMap.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
    if ( malodyExportMode ) {
        props["mode"] = std::to_string(MMM::malodyModeValue(*malodyExportMode));
    }
    if ( shouldAddStoreModeExt ) {
        applyMalodyStoreModeExtMetadata(beatMap);
    }

    bool ok = beatMap.saveToFile(outputPath);
    if ( ok && shouldAddStoreModeExt ) {
        ok = patchMalodyStoreModeExtFile(outputPath);
    }

    if ( previousProps ) {
        beatMap.m_metadata.map_properties[MMM::MapMetadataType::MALODY] =
            std::move(*previousProps);
    } else {
        beatMap.m_metadata.map_properties.erase(MMM::MapMetadataType::MALODY);
    }
    return ok;
}

/// @brief 读取源文件，并按需在 MC 字节中替换上架 mode_ext。
/// @param sourcePath 源文件路径。
/// @param addStoreModeExtForMalodyExport 是否写入上架 mode_ext。
/// @param outBytes 输出文件字节。
/// @return 是否读取成功。
bool readPackageSourceFileWithOptionalMalodyStoreModeExt(
    const std::filesystem::path& sourcePath,
    bool addStoreModeExtForMalodyExport, std::vector<std::uint8_t>& outBytes)
{
    std::vector<std::uint8_t> inputBytes;
    if ( !readPackageSourceFile(sourcePath, inputBytes) ) {
        return false;
    }
    if ( !addStoreModeExtForMalodyExport || !isMalodyChartPath(sourcePath) ) {
        outBytes = std::move(inputBytes);
        return true;
    }
    return patchMalodyStoreModeExtBytes(inputBytes, outBytes);
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
/// @param malodyExportMode MC 转换产物临时使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为 MC 转换产物写入上架 mode_ext。
/// @return 是否转换成功。
bool convertPackageBeatmapFile(const std::filesystem::path&   sourcePath,
                               const std::filesystem::path&   outputPath,
                               const MMM::BaseMapMeta*        metadataOverride,
                               std::optional<MMM::MalodyMode> malodyExportMode,
                               bool addStoreModeExtForMalodyExport)
{
    auto beatMap = MMM::BeatMap::loadFromFile(sourcePath);
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) return false;
    if ( metadataOverride ) {
        beatMap.m_baseMapMetadata = *metadataOverride;
    }
    beatMap.m_baseMapMetadata.map_path = outputPath;
    return saveBeatmapWithMalodyExportOptions(
        beatMap, outputPath, malodyExportMode, addStoreModeExtForMalodyExport);
}

/// @brief 读取转换后的目标谱面字节。
/// @param sourcePath 来源谱面路径。
/// @param projectOutputPath 保存到项目中时使用的目标路径。
/// @param outputExtension 目标谱面扩展名。
/// @param saveToProject 是否将转换产物留在项目目录中。
/// @param metadataOverride 转换时覆盖的基础谱面元数据；为空则使用源谱面元数据。
/// @param malodyExportMode MC 转换产物临时使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为 MC 转换产物写入上架 mode_ext。
/// @param outBytes 输出文件字节。
/// @return 是否成功读取转换结果。
bool readConvertedPackageBeatmapBytes(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& projectOutputPath,
    const std::string& outputExtension, bool saveToProject,
    const MMM::BaseMapMeta*        metadataOverride,
    std::optional<MMM::MalodyMode> malodyExportMode,
    bool addStoreModeExtForMalodyExport, std::vector<std::uint8_t>& outBytes)
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

    if ( !convertPackageBeatmapFile(sourcePath,
                                    conversionPath,
                                    metadataOverride,
                                    malodyExportMode,
                                    addStoreModeExtForMalodyExport) ) {
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
/// @param malodyExportMode MCZ 包内 MC 谱面统一使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为写出的 MC 谱面写入上架
/// mode_ext。
/// @return 是否打包成功。
bool writeBeatmapPackage(
    const std::filesystem::path&          projectRoot,
    const std::filesystem::path&          outputPath,
    const std::vector<std::string>&       selectedRelativePaths,
    const MMM::PackageSupportedFileTypes& packageTypes,
    const std::vector<MMM::Logic::PackageBeatmapMetadataOverride>&
         metadataOverrides,
    bool saveConvertedBeatmapsToProject, bool includeLegacyImdBeatmapsInPackage,
    std::optional<MMM::MalodyMode> malodyExportMode,
    bool                           addStoreModeExtForMalodyExport)
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
    const bool addStoreModeExtToMc =
        addStoreModeExtForMalodyExport &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    const auto packageMalodyExportMode =
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz")
            ? malodyExportMode
            : std::nullopt;
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
            auto       targetArchivePath   = relativePath;
            const bool shouldConvertSource = shouldConvertPackageBeatmapSource(
                extension, packageBeatmapExtension);
            if ( shouldConvertSource ) {
                targetArchivePath.replace_extension(packageBeatmapExtension);
            }
            const bool shouldReencode =
                shouldConvertSource || packageMalodyExportMode.has_value();

            const auto metadataIt = metadataOverrideMap.find(relativePathKey);
            const MMM::BaseMapMeta* metadataOverride =
                metadataIt == metadataOverrideMap.end() ? nullptr
                                                        : &metadataIt->second;

            if ( !hasPackageArchivePath(archivedNames, targetArchivePath) ) {
                if ( shouldReencode ) {
                    const auto projectOutputPath =
                        (projectRoot / targetArchivePath).lexically_normal();
                    if ( !readConvertedPackageBeatmapBytes(
                             sourcePath,
                             projectOutputPath,
                             packageBeatmapExtension,
                             saveConvertedBeatmapsToProject &&
                                 shouldConvertSource,
                             metadataOverride,
                             packageMalodyExportMode,
                             addStoreModeExtToMc,
                             fileBytes) ) {
                        XERROR("PackBeatmap: failed to convert source file: {}",
                               relativeUtf8);
                        success = false;
                        break;
                    }
                } else if (
                    !readPackageSourceFileWithOptionalMalodyStoreModeExt(
                        sourcePath, addStoreModeExtToMc, fileBytes) ) {
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
                                 std::nullopt,
                                 false,
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
                               std::is_same_v<T, CmdUpdateTimelineEvents> ||
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
                    case EditTool::Layout:
                        toolName = TR("ui.status.tool.layout").pStr;
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
                    if ( !arg.isScrubbing ) {
                        const auto timeText = formatStatusTime(arg.time);
                        m_ctx->lastActionMessage =
                            fmt::format("{} {} {}",
                                        TR("ui.status.category.playback"),
                                        TR("ui.status.playback.seek"),
                                        timeText);
                    }
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
                } else if constexpr ( std::is_same_v<T,
                                                     CmdUpdateBgmTrackCount> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.project"),
                                    TR("ui.status.project.bgm_track_count"),
                                    arg.bgmTrackCount);
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
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                               std::is_same_v<T,
                                              CmdMarkBeatmapMetadataDirty> ) {
                    this->handleCommand(arg);
                }
                // --- Playback 处理的命令 ---
                else if constexpr (
                    std::is_same_v<T, CmdSetPlayState> ||
                    std::is_same_v<T, CmdSeek> ||
                    std::is_same_v<T, CmdSetPlaybackSpeed> ||
                    std::is_same_v<T, CmdSetKeySoundTrackMute> ||
                    std::is_same_v<T, CmdSetKeySoundTrackGain> ||
                    std::is_same_v<T, CmdSetKeySoundEffectGroupGain> ||
                    std::is_same_v<T, CmdSetBgmKeySoundAreaMute> ||
                    std::is_same_v<T, CmdScroll> ||
                    std::is_same_v<T, CmdPanCanvas> ) {
                    m_playback->handleCommand(arg);
                }
                // --- Interaction 处理的命令 ---
                else if constexpr (
                    std::is_same_v<T, CmdSetHoveredEntity> ||
                    std::is_same_v<T, CmdSelectEntity> ||
                    std::is_same_v<T, CmdStartDrag> ||
                    std::is_same_v<T, CmdUpdateDrag> ||
                    std::is_same_v<T, CmdEndDrag> ||
                    std::is_same_v<T, CmdCreateAudioSample> ||
                    std::is_same_v<T, CmdUpdateAudioSampleProperties> ||
                    std::is_same_v<T, CmdUpdateObjectSampleVolume> ||
                    std::is_same_v<T, CmdChangeTool> ||
                    std::is_same_v<T, CmdSetMousePosition> ||
                    std::is_same_v<T, CmdUpdateTrackCount> ||
                    std::is_same_v<T, CmdUpdateBgmTrackCount> ||
                    std::is_same_v<T, CmdSetBrushNoteColor> ||
                    std::is_same_v<T, CmdSetBrushNotePalette> ||
                    std::is_same_v<T, CmdSetBrushAudioResource> ||
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
                    std::is_same_v<T, CmdUpdateTimelineEvents> ||
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
    const bool disablePolylineEditing =
        m_ctx->lastConfig.settings.enablePolylineEditing &&
        !cmd.config.settings.enablePolylineEditing;
    const bool disableBmsEditing =
        m_ctx->lastConfig.settings.enableBmsEditing &&
        !cmd.config.settings.enableBmsEditing;
    m_ctx->lastConfig = cmd.config;
    if ( disablePolylineEditing ) {
        auto view =
            m_ctx->noteRegistry.view<NoteComponent, InteractionComponent>();
        for ( const auto entity : view ) {
            const auto& note = view.get<NoteComponent>(entity);
            if ( SessionUtils::isNoteEditable(note, cmd.config.settings) ) {
                continue;
            }
            auto& interaction      = view.get<InteractionComponent>(entity);
            interaction.isSelected = false;
            interaction.isHovered  = false;
            interaction.isDragging = false;
            interaction.isCut      = false;
            interaction.hoveredPart =
                static_cast<std::uint8_t>(HoverPart::None);
            interaction.hoveredSubIndex = -1;
            m_ctx->selectedNoteEntities.erase(entity);
        }

        if ( m_ctx->hoveredObjectKind == ChartObjectKind::PlayerNote &&
             m_ctx->hoveredEntity != entt::null &&
             m_ctx->noteRegistry.valid(m_ctx->hoveredEntity) &&
             m_ctx->noteRegistry.all_of<NoteComponent>(m_ctx->hoveredEntity) &&
             !SessionUtils::isNoteEditable(
                 m_ctx->noteRegistry.get<const NoteComponent>(
                     m_ctx->hoveredEntity),
                 cmd.config.settings) ) {
            m_ctx->hoveredEntity   = entt::null;
            m_ctx->hoveredPart     = static_cast<std::int32_t>(HoverPart::None);
            m_ctx->hoveredSubIndex = -1;
        }
    }
    if ( disableBmsEditing ) {
        auto view = m_ctx->sampleRegistry.view<InteractionComponent>();
        for ( const auto entity : view ) {
            auto& interaction      = view.get<InteractionComponent>(entity);
            interaction.isSelected = false;
            interaction.isHovered  = false;
            interaction.isDragging = false;
            interaction.isCut      = false;
            interaction.hoveredPart =
                static_cast<std::uint8_t>(HoverPart::None);
            interaction.hoveredSubIndex = -1;
        }
        m_ctx->selectedSampleEntities.clear();
        if ( m_ctx->hoveredObjectKind == ChartObjectKind::AudioSample ) {
            m_ctx->hoveredEntity     = entt::null;
            m_ctx->hoveredObjectKind = ChartObjectKind::PlayerNote;
            m_ctx->hoveredPart     = static_cast<std::int32_t>(HoverPart::None);
            m_ctx->hoveredSubIndex = -1;
        }
        if ( m_ctx->brushState.createsAudioSample ) {
            m_ctx->brushState.isActive           = false;
            m_ctx->brushState.createsAudioSample = false;
        }
    }
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
        auto& camera             = m_ctx->cameras[cmd.cameraId];
        camera.horizontalOffsetX = resizeCanvasHorizontalOffset(
            camera.horizontalOffsetX, camera.viewportWidth, cmd.width);
        camera.viewportWidth  = cmd.width;
        camera.viewportHeight = cmd.height;
    }
}

void BeatmapSession::handleCommand(const CmdLoadBeatmap& cmd)
{
    if ( m_metadataAutoSavePending && !flushPendingMetadataAutoSave() ) {
        XERROR(
            "BeatmapSession: cannot replace beatmap because pending "
            "metadata could not be saved");
        return;
    }
    m_metadataAutoSavePending         = false;
    m_metadataAutoSaveTimerNeedsReset = false;
    SessionUtils::loadBeatmap(*m_ctx, cmd.beatmap);
    if ( m_ctx->currentBeatmap ) {
        publishBeatmapLoadDiagnostics(*m_ctx->currentBeatmap);
    }
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
        const bool hadPendingMetadataAutoSave = m_metadataAutoSavePending;
        /// @brief 保存失败时恢复尾随任务，避免后续打包读取旧文件。
        const auto restorePendingMetadataAutoSave = [&]() {
            if ( !hadPendingMetadataAutoSave ) return;
            m_metadataAutoSavePending         = true;
            m_metadataAutoSaveTimerNeedsReset = true;
        };
        m_metadataAutoSavePending         = false;
        m_metadataAutoSaveTimerNeedsReset = false;
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
            restorePendingMetadataAutoSave();
            return;
        }

        m_ctx->m_needsTimingsSync = true;
        m_ctx->m_needsNotesSync   = true;
        SessionUtils::syncBeatmap(*m_ctx);
        SessionUtils::ensureHitEvents(*m_ctx);
        refreshCurrentProjectSongFileHint(*m_ctx->currentBeatmap);

        bool ok = m_ctx->currentBeatmap->saveToFile(savePath);
        if ( !ok ) {
            XERROR("SaveBeatmap: failed to save to {}",
                   Config::pathToUtf8(savePath));
            Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
                .path     = Config::pathToUtf8(savePath),
                .success  = false,
                .isExport = false,
            });
            restorePendingMetadataAutoSave();
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
        if ( syncSavedMetadataToProjectEntry(
                 m_ctx->currentBeatmap->m_baseMapMetadata) ) {
            EditorEngine::instance().saveProject();
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
        bool ok       = saveBeatmapWithMalodyExportOptions(
            *m_ctx->currentBeatmap,
            savePath,
            cmd.malodyExportMode,
            cmd.addStoreModeExtForMalodyExport);
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
    if ( !EditorEngine::instance().saveDirtyBeatmapsForPackaging() ) {
        XERROR("PackBeatmap: dirty beatmaps could not be saved");
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

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
                            cmd.includeLegacyImdBeatmapsInPackage,
                            cmd.malodyExportMode,
                            cmd.addStoreModeExtForMalodyExport);
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
        const auto oldMetadata = m_ctx->currentBeatmap->m_baseMapMetadata;
        auto       updatedMeta = cmd.baseMeta;
        normalizeCurrentProjectMetadataPaths(updatedMeta);
        updatedMeta.track_count     = std::max(1, updatedMeta.track_count);
        updatedMeta.bgm_track_count = m_ctx->bgmTrackCount;
        if ( baseMapMetadataEqual(oldMetadata, updatedMeta) ) return;

        // 所有元数据编辑入口最终都在此处转入可撤销的原子改键操作，避免 UI
        // 直接覆盖 track_count 后丢失自动采样的 BGM 相对轨道。
        if ( m_ctx->trackCount != updatedMeta.track_count ) {
            std::string migrationError;
            if ( !validateSampleTrackCountMigration(*m_ctx,
                                                    m_ctx->trackCount,
                                                    updatedMeta.track_count,
                                                    migrationError) ) {
                m_ctx->lastActionMessage = migrationError;
                XERROR("BeatmapSession: {}", migrationError);
                return;
            }

            m_interaction->handleCommand(
                CmdUpdateTrackCount{ updatedMeta.track_count });
            if ( m_ctx->trackCount != updatedMeta.track_count ) {
                if ( m_ctx->lastActionMessage.empty() ) {
                    m_ctx->lastActionMessage =
                        "玩家轨道数变更未能完成，元数据保持不变";
                }
                XERROR("BeatmapSession: {}", m_ctx->lastActionMessage);
                return;
            }
        }

        m_ctx->currentBeatmap->m_baseMapMetadata = updatedMeta;
        m_ctx->actionStack.markDirty();
        m_metadataAutoSavePending         = true;
        m_metadataAutoSaveTimerNeedsReset = true;
        XINFO("BeatmapSession: Metadata updated for {}",
              m_ctx->currentBeatmap->m_baseMapMetadata.name);
        if ( oldMetadata.map_path != updatedMeta.map_path ) {
            m_ctx->isAudioTimelineDescriptorDirty   = true;
            m_ctx->isAudioTimelineActivationPending = true;
        }

        // 如果关键渲染参数发生变化，刷新 ScrollCache
        if ( oldMetadata.preference_bpm != updatedMeta.preference_bpm ||
             oldMetadata.track_count != updatedMeta.track_count ) {
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

        // 路径或资源类型改变时，按图片/视频分支重新探测尺寸。
        if ( oldMetadata.main_cover_path != updatedMeta.main_cover_path ||
             oldMetadata.cover_type != updatedMeta.cover_type ) {
            SessionUtils::updateBackgroundSize(
                *m_ctx,
                updatedMeta,
                EditorEngine::instance().getCurrentProject());
        }
    }
}

/// @brief 标记 UI 直接修改的扩展元数据，并安排一次尾随自动保存。
void BeatmapSession::handleCommand(const CmdMarkBeatmapMetadataDirty& cmd)
{
    (void)cmd;
    if ( !m_ctx->currentBeatmap ) return;

    m_ctx->actionStack.markDirty();
    m_metadataAutoSavePending         = true;
    m_metadataAutoSaveTimerNeedsReset = true;
}

}  // namespace MMM::Logic
