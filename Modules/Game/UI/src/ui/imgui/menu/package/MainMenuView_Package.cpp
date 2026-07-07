#include "ui/imgui/menu/MainMenuView.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 将 ASCII 字符串转换为小写，用于扩展名判断。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 将下一项控件放到当前内容区域的水平中心。
/// @param itemWidth 控件宽度。
/// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
void centerNextItem(float itemWidth)
{
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > itemWidth ) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - itemWidth) * 0.5f);
    }
}

/// @brief 绘制水平居中的按钮。
/// @param label 按钮文本和 ImGui ID。
/// @param size 按钮尺寸。
/// @return 按钮被点击时返回 true。
/// @warning UI 绘制路径：只调整游标并调用 ImGui::Button。
bool drawCenteredButton(const char* label, ImVec2 size)
{
    centerNextItem(size.x);
    return ::MMM::UI::FeedbackButton(label, size);
}

/// @brief 估算 Checkbox 绘制指定标签时需要的宽度。
/// @param label Checkbox 标签文本。
/// @return 当前字体和样式下的控件宽度。
/// @warning UI 绘制路径：只读取当前 ImGui 样式和字体测量结果。
float getCheckboxDisplayWidth(const char* label)
{
    const ImGuiStyle& style     = ImGui::GetStyle();
    const ImVec2      labelSize = ImGui::CalcTextSize(label, nullptr, true);
    float             width     = ImGui::GetFrameHeight();
    if ( labelSize.x > 0.0f ) {
        width += style.ItemInnerSpacing.x + labelSize.x;
    }
    return width;
}

/// @brief 若下一控件还能放进当前行，则把光标移动到同一行。
/// @param nextItemWidth 下一控件预计宽度。
/// @warning UI 绘制路径：只根据上一控件位置和内容边界决定是否调用 SameLine。
void sameLineIfItemFits(float nextItemWidth)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float nextItemX   = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
    const float contentMaxX =
        ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    if ( nextItemX + nextItemWidth <= contentMaxX ) {
        ImGui::SameLine();
    }
}

/// @brief 替换文件名中不适合作为普通文件名的路径分隔字符。
std::string sanitizePackageFileNamePart(std::string value)
{
    if ( value.empty() ) return "map";
    std::replace(value.begin(), value.end(), '/', '_');
    std::replace(value.begin(), value.end(), '\\', '_');
    return value;
}

/// @brief 获取打包输出对话框默认打开路径，优先使用当前项目根目录。
/// @param settings 编辑器设置，用于无项目时回退到通用文件选择器路径。
/// @return UTF-8 编码的默认目录路径。
std::string getPackagePickerDefaultPath(const Config::EditorSettings& settings)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        return Config::pathToUtf8(project->m_projectRoot);
    }

    return settings.lastFilePickerPath.empty() ? std::string(".")
                                               : settings.lastFilePickerPath;
}

/// @brief 获取打包格式显示名称。
/// @param type 打包格式。
/// @return 用户界面显示的格式名称。
std::string getPackageTypeDisplayName(PackageFileType type)
{
    const auto& types = getPackageSupportedFileTypes(type);
    switch ( type ) {
    case PackageFileType::Mcz:
        return "Malody Chart Package (" +
               std::string(types.m_packageExtension) + ")";
    case PackageFileType::Osz:
        return "osu! Beatmap Package (" +
               std::string(types.m_packageExtension) + ")";
    case PackageFileType::Mpk:
        return "MusicMapMaker Package (" +
               std::string(types.m_packageExtension) + ")";
    }
    return "MusicMapMaker Package (.mpk)";
}

/// @brief 取得打包格式扩展名。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
std::string getPackageExtension(PackageFileType type)
{
    return std::string(getPackageSupportedFileTypes(type).m_packageExtension);
}

/// @brief 取得打包格式要求的谱面文件扩展名。
/// @param type 打包格式。
/// @return 带前导点的谱面扩展名。
std::string getPackageBeatmapExtension(PackageFileType type)
{
    const auto& types = getPackageSupportedFileTypes(type);
    if ( types.m_beatmapExtensions.empty() ) return ".mmm";
    return std::string(types.m_beatmapExtensions.front());
}

/// @brief 判断打包格式是否需要显示保存转换谱面选项。
/// @param type 打包格式。
/// @return 需要显示时返回 true。
bool shouldShowConvertedBeatmapSaveOption(PackageFileType type)
{
    return type == PackageFileType::Mcz || type == PackageFileType::Osz;
}

/// @brief 判断谱面源是否需要转换为当前目标包的谱面格式。
/// @param type 目标打包格式。
/// @param relativePath 项目相对谱面路径。
/// @return 谱面源需要转换时返回 true。
bool shouldConvertPackageCandidateBeatmap(PackageFileType    type,
                                          const std::string& relativePath)
{
    const auto  extension = toLowerAscii(Config::pathToUtf8(
        Config::utf8ToPath(relativePath).lexically_normal().extension()));
    const auto& types     = getPackageSupportedFileTypes(type);
    return isPackageBeatmapSourceExtensionSupported(types, extension) &&
           !isPackageResourceExtensionSupported(
               types, PackageResourceType::Beatmap, extension);
}

/// @brief 判断当前已选候选中是否存在需要转换的谱面源。
/// @param type 目标打包格式。
/// @param candidateFiles 当前候选文件缓存。
/// @return 存在需要转换的已选谱面源时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
bool hasSelectedPackageBeatmapSourceRequiringConversion(
    PackageFileType                          type,
    const std::vector<PackageCandidateFile>& candidateFiles)
{
    return std::any_of(candidateFiles.begin(),
                       candidateFiles.end(),
                       [&](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  shouldConvertPackageCandidateBeatmap(
                                      type, file.relativePath);
                       });
}

/// @brief 判断打包格式是否需要显示旧 IMD 兼容谱面选项。
/// @param type 打包格式。
/// @return 需要显示时返回 true。
bool shouldShowLegacyImdPackageOption(PackageFileType type)
{
    return type == PackageFileType::Mcz;
}

/// @brief 判断打包转换前是否需要让用户补充目标谱面元数据。
/// @param type 目标打包格式。
/// @param extension 来源文件扩展名。
/// @return 需要补充元数据时返回 true。
bool shouldPreparePackageBeatmapMetadataEdit(PackageFileType    type,
                                             const std::string& extension)
{
    switch ( type ) {
    case PackageFileType::Mcz: return packageExtensionEquals(extension, ".imd");
    case PackageFileType::Osz:
        return packageExtensionInList(PACKAGE_BEATMAP_SOURCE_EXTENSIONS,
                                      extension) &&
               !packageExtensionEquals(extension, ".osu") &&
               !packageExtensionEquals(extension, ".mmm");
    case PackageFileType::Mpk: return false;
    }
    return false;
}

/// @brief 构建打包元数据补充窗口的提示文本。
/// @param type 目标打包格式。
/// @return 用户界面提示文本。
std::string makePackageMetadataEditPrompt(PackageFileType type)
{
    const std::string targetExtension = getPackageBeatmapExtension(type);
    switch ( type ) {
    case PackageFileType::Mcz:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充 Malody 包需要的元数据：";
    case PackageFileType::Osz:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充 osu! 包需要的元数据：";
    case PackageFileType::Mpk:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充目标格式需要的元数据：";
    }
    return "这些谱面将转换为 " + targetExtension +
           "。请补充目标格式需要的元数据：";
}

/// @brief 取得原生文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 不带前导点的扩展名。
const char* getNativePackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return "mcz";
    case PackageFileType::Osz: return "osz";
    case PackageFileType::Mpk: return "mpk";
    }
    return "mpk";
}

/// @brief 取得统一文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
const char* getUnifiedPackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return ".mcz";
    case PackageFileType::Osz: return ".osz";
    case PackageFileType::Mpk: return ".mpk";
    }
    return ".mpk";
}

/// @brief 判断扩展名是否为打包产物扩展名。
/// @param extension 待检查扩展名。
/// @return 是否应从候选资源列表中排除。
bool isPackageArchiveExtension(const std::string& extension)
{
    return findPackageSupportedFileTypes(extension) != nullptr ||
           packageExtensionEquals(extension, ".zip");
}

/// @brief 将文本写入固定大小输入缓存。
/// @param buffer 目标输入缓存。
/// @param text 源文本。
template<std::size_t N>
void copyToPackageInputBuffer(std::array<char, N>& buffer,
                              std::string_view     text)
{
    buffer.fill('\0');
    const std::size_t count = std::min(text.size(), buffer.size() - 1);
    std::copy_n(text.begin(), count, buffer.begin());
}

/// @brief 从固定大小输入缓存读取文本。
/// @param buffer 输入缓存。
/// @return 缓存中的 C 字符串文本。
template<std::size_t N>
std::string packageInputBufferText(const std::array<char, N>& buffer)
{
    return std::string(buffer.data());
}

/// @brief 根据扩展名推断资源分类显示文本。
/// @param types 当前打包格式规则。
/// @param extension 待检查扩展名。
/// @return 资源分类显示文本，空字符串表示不符合规则。
std::string getPackageCandidateTypeLabel(const PackageSupportedFileTypes& types,
                                         const std::string& extension)
{
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        if ( !isPackageResourceExtensionSupported(
                 types, PackageResourceType::Beatmap, extension) ) {
            return "谱面源";
        }
        return "谱面";
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return "音频";
    }
    if ( types.m_allowAllVideoFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Video,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Video, extension) ) {
        return "视频";
    }
    if ( types.m_allowAllImageFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Image,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Image, extension) ) {
        return "图片";
    }
    return {};
}

/// @brief 根据扩展名推断资源分类。
/// @param types 当前打包格式规则。
/// @param extension 待检查扩展名。
/// @return 可打包资源分类；不符合规则时返回空。
std::optional<PackageResourceType> getPackageCandidateResourceType(
    const PackageSupportedFileTypes& types, const std::string& extension)
{
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        return PackageResourceType::Beatmap;
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return PackageResourceType::Audio;
    }
    if ( types.m_allowAllVideoFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Video,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Video, extension) ) {
        return PackageResourceType::Video;
    }
    if ( types.m_allowAllImageFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Image,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Image, extension) ) {
        return PackageResourceType::Image;
    }
    return std::nullopt;
}

/// @brief 规范化项目相对路径为 UTF-8 通用分隔符形式。
/// @param path 项目相对路径。
/// @return 规范化后的路径。
std::string normalizePackageRelativeUtf8(const std::string& path)
{
    if ( path.empty() ) return {};
    return Config::pathToUtf8Generic(
        Config::utf8ToPath(path).lexically_normal());
}

/// @brief 判断项目相对路径是否会逃逸项目根目录。
/// @param path 项目相对路径。
/// @return 路径逃逸根目录时返回 true。
bool packageDependencyPathEscapesRoot(const std::filesystem::path& path)
{
    const auto normalized = path.lexically_normal();
    for ( const auto& part : normalized ) {
        if ( part == ".." ) return true;
    }
    return normalized.is_absolute();
}

/// @brief 解析项目相对路径为文件系统路径。
/// @param projectRoot 项目根目录。
/// @param path 项目相对或绝对路径。
/// @return 规范化后的文件系统路径。
std::filesystem::path resolvePackageProjectPath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }
    return (projectRoot / path).lexically_normal();
}

/// @brief 将文件系统路径转为项目相对路径。
/// @param projectRoot 项目根目录。
/// @param path 文件系统路径。
/// @return 项目相对路径；无法转换时返回空。
std::filesystem::path makePackageProjectRelativePath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( path.empty() ) return {};
    if ( path.is_relative() ) {
        const auto relativePath = path.lexically_normal();
        return packageDependencyPathEscapesRoot(relativePath)
                   ? std::filesystem::path{}
                   : relativePath;
    }

    std::error_code filesystemError;
    const auto root = std::filesystem::absolute(projectRoot, filesystemError);
    if ( filesystemError ) return {};

    auto relativePath = std::filesystem::relative(path, root, filesystemError);
    if ( filesystemError || relativePath.empty() ) return {};
    relativePath = relativePath.lexically_normal();
    return packageDependencyPathEscapesRoot(relativePath)
               ? std::filesystem::path{}
               : relativePath;
}

/// @brief 将文件系统路径转为 UTF-8 项目相对路径。
/// @param projectRoot 项目根目录。
/// @param path 文件系统路径。
/// @return UTF-8 项目相对路径；无法转换时返回空。
std::string makePackageProjectRelativeUtf8(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    const auto relativePath = makePackageProjectRelativePath(projectRoot, path);
    if ( relativePath.empty() ) return {};
    return Config::pathToUtf8Generic(relativePath);
}

/// @brief 在项目根目录和谱面目录之间解析谱面 metadata 资源路径。
/// @param projectRoot 项目根目录。
/// @param mapDirectory 谱面文件所在目录。
/// @param path metadata 中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
/// @return 可访问优先的资源路径。
std::filesystem::path resolvePackageMetadataResourcePath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& mapDirectory,
    const std::filesystem::path& path, bool preferProjectRoot)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    const auto projectPath = resolvePackageProjectPath(projectRoot, path);
    const auto mapPath     = (mapDirectory / path).lexically_normal();

    std::error_code filesystemError;
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, filesystemError) ) {
            return projectPath;
        }
        filesystemError.clear();
        if ( std::filesystem::exists(mapPath, filesystemError) ) {
            return mapPath;
        }
        return projectPath;
    }

    if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
    filesystemError.clear();
    if ( std::filesystem::exists(projectPath, filesystemError) ) {
        return projectPath;
    }
    return mapPath;
}

/// @brief 向依赖列表追加一个去重后的项目相对路径。
/// @param dependencies 依赖路径列表。
/// @param relativePath 项目相对路径。
void appendUniquePackageDependency(std::vector<std::string>& dependencies,
                                   const std::string&        relativePath)
{
    if ( relativePath.empty() ) return;
    if ( std::find(dependencies.begin(), dependencies.end(), relativePath) !=
         dependencies.end() ) {
        return;
    }
    dependencies.push_back(relativePath);
}

/// @brief 谱面打包扫描结果。
struct PackageBeatmapInfo {
    /// @brief 谱面引用的资源路径列表。
    std::vector<std::string> dependencyRelativePaths;

    /// @brief 是否包含 Flick 或折线。
    bool hasStoreModeExtEligibleElements{ false };
};

/// @brief 将单个 metadata 资源路径解析并追加到依赖列表。
/// @param dependencies 依赖路径列表。
/// @param projectRoot 项目根目录。
/// @param mapDirectory 谱面文件所在目录。
/// @param path metadata 中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
void appendPackageMetadataDependency(std::vector<std::string>&    dependencies,
                                     const std::filesystem::path& projectRoot,
                                     const std::filesystem::path& mapDirectory,
                                     const std::filesystem::path& path,
                                     bool preferProjectRoot)
{
    if ( path.empty() ) return;
    const auto resolved = resolvePackageMetadataResourcePath(
        projectRoot, mapDirectory, path, preferProjectRoot);
    appendUniquePackageDependency(
        dependencies, makePackageProjectRelativeUtf8(projectRoot, resolved));
}

/// @brief 查找项目中指定谱面条目。
/// @param project 项目。
/// @param beatmapRelativePath UTF-8 项目相对谱面路径。
/// @return 找到时返回谱面条目，否则返回空。
const Project::BeatmapEntry* findPackageBeatmapEntry(
    const Project& project, const std::string& beatmapRelativePath)
{
    const auto normalizedBeatmapPath =
        normalizePackageRelativeUtf8(beatmapRelativePath);
    const auto it = std::find_if(
        project.m_beatmaps.begin(),
        project.m_beatmaps.end(),
        [&](const Project::BeatmapEntry& entry) {
            return normalizePackageRelativeUtf8(entry.m_filePath) ==
                   normalizedBeatmapPath;
        });
    return it == project.m_beatmaps.end() ? nullptr : &(*it);
}

/// @brief 追加项目谱面条目绑定的主音轨资源路径。
/// @param dependencies 依赖路径列表。
/// @param project 项目。
/// @param entry 项目谱面条目。
void appendPackageBeatmapEntryAudioDependency(
    std::vector<std::string>& dependencies, const Project& project,
    const Project::BeatmapEntry& entry)
{
    if ( entry.m_audioTrackId.empty() ) return;
    const auto audioIt =
        std::find_if(project.m_audioResources.begin(),
                     project.m_audioResources.end(),
                     [&](const AudioResource& resource) {
                         return resource.m_id == entry.m_audioTrackId;
                     });
    if ( audioIt == project.m_audioResources.end() ) return;
    appendUniquePackageDependency(
        dependencies, normalizePackageRelativeUtf8(audioIt->m_path));
}

/// @brief 收集一个谱面打包时需要的资源和元素信息。
/// @param project 当前项目。
/// @param beatmapRelativePath UTF-8 项目相对谱面路径。
/// @return 谱面依赖和元素扫描结果。
PackageBeatmapInfo collectPackageBeatmapInfo(
    const Project& project, const std::string& beatmapRelativePath)
{
    PackageBeatmapInfo result;
    const auto         normalizedBeatmapPath =
        normalizePackageRelativeUtf8(beatmapRelativePath);
    if ( normalizedBeatmapPath.empty() ) return result;

    if ( const auto* entry =
             findPackageBeatmapEntry(project, normalizedBeatmapPath) ) {
        appendPackageBeatmapEntryAudioDependency(
            result.dependencyRelativePaths, project, *entry);
    }

    const auto relativePath = Config::utf8ToPath(normalizedBeatmapPath);
    const auto mapPath =
        resolvePackageProjectPath(project.m_projectRoot, relativePath);
    auto beatMap = BeatMap::loadFromFile(mapPath);

    const auto mapDirectory      = mapPath.parent_path();
    auto       mapExtension      = Config::pathToUtf8(mapPath.extension());
    mapExtension                 = toLowerAscii(mapExtension);
    const bool preferProjectRoot = packageExtensionEquals(mapExtension, ".mmm");
    const auto& meta             = beatMap.m_baseMapMetadata;

    result.hasStoreModeExtEligibleElements =
        !beatMap.m_noteData.flicks.empty() ||
        !beatMap.m_noteData.polylines.empty();

    appendPackageMetadataDependency(result.dependencyRelativePaths,
                                    project.m_projectRoot,
                                    mapDirectory,
                                    meta.main_audio_path,
                                    preferProjectRoot);
    appendPackageMetadataDependency(result.dependencyRelativePaths,
                                    project.m_projectRoot,
                                    mapDirectory,
                                    meta.main_cover_path,
                                    preferProjectRoot);
    appendPackageMetadataDependency(result.dependencyRelativePaths,
                                    project.m_projectRoot,
                                    mapDirectory,
                                    meta.cover_path,
                                    preferProjectRoot);

    result.dependencyRelativePaths.erase(
        std::remove(result.dependencyRelativePaths.begin(),
                    result.dependencyRelativePaths.end(),
                    normalizedBeatmapPath),
        result.dependencyRelativePaths.end());
    return result;
}
}  // namespace

/// @brief 按当前打包目标格式规范化输出包路径。
/// @param path 文件选择器返回的输出路径。
/// @return 补齐目标打包扩展名后的输出路径。
std::string MainMenuView::applyPackSelectedFormatToPath(
    const std::string& path) const
{
    if ( path.empty() ) return path;

    std::filesystem::path outputPath = Config::utf8ToPath(path);
    outputPath.replace_extension(
        getPackageExtension(m_package.selectedFileType));
    return Config::pathToUtf8(outputPath);
}

/// @brief 请求打包当前已选择的项目文件。
/// @param path 输出包路径。
void MainMenuView::requestPackBeatmapTo(std::string path)
{
    path = applyPackSelectedFormatToPath(path);
    if ( path.empty() || m_package.pendingRelativePaths.empty() ) {
        m_statusMessage      = "没有可打包的已选文件";
        m_statusMessageTimer = 3.0f;
        return;
    }

    dispatchCommand(Logic::CmdPackBeatmap{
        .exportPath                   = path,
        .selectedProjectRelativePaths = m_package.pendingRelativePaths,
        .saveConvertedBeatmapsToProject =
            shouldShowConvertedBeatmapSaveOption(m_package.selectedFileType) &&
            hasSelectedPackageBeatmapSourceRequiringConversion(
                m_package.selectedFileType, m_package.candidateFiles) &&
            m_package.saveConvertedBeatmapsToProject,
        .includeLegacyImdBeatmapsInPackage =
            shouldShowLegacyImdPackageOption(m_package.selectedFileType) &&
            m_package.includeLegacyImdBeatmaps,
        .addStoreModeExtForMalodyExport =
            hasSelectedPackageStoreModeExtCandidates() &&
            Config::AppConfig::instance()
                .getEditorSettings()
                .autoAddStoreModeExtForMalodyExport,
        .metadataOverrides = m_package.pendingMetadataOverrides,
    });
    m_package.pendingRelativePaths.clear();
    m_package.pendingMetadataOverrides.clear();
}

/// @brief 渲染打包目标格式选择弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderPackageFormatPickerPopup(float dpiScale)
{
    constexpr const char* popupId = "选择打包格式###PackageFormatPickerModal";
    if ( m_package.showFormatPicker ) {
        ImGui::OpenPopup(popupId);
        m_package.showFormatPicker = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool            hasSelection = false;
    PackageFileType selectedType = m_package.selectedFileType;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(380.0f * dpiScale, 0.0f)) ) {
            ImGui::TextUnformatted("选择目标打包格式：");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(320.0f * dpiScale, 0.0f);
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Mcz).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Mcz;
                hasSelection = true;
            }
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Osz).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Osz;
                hasSelection = true;
            }
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Mpk).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Mpk;
                hasSelection = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if ( drawCenteredButton(TR("ui.common.cancel").data(),
                                    ImVec2(120.0f * dpiScale, 0.0f)) ) {
                ImGui::CloseCurrentPopup();
            }

            if ( hasSelection ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if ( hasSelection ) {
        m_package.selectedFileType = selectedType;
        if ( !shouldShowConvertedBeatmapSaveOption(selectedType) ) {
            m_package.saveConvertedBeatmapsToProject = false;
        }
        if ( !shouldShowLegacyImdPackageOption(selectedType) ) {
            m_package.includeLegacyImdBeatmaps = false;
        }
        rebuildPackageCandidateFiles();
        m_package.showFileSelectionWindow = true;
    }
}

/// @brief 渲染打包文件复选列表窗口。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI
/// 热路径：打包选择弹窗可见时每帧执行；只读取候选缓存，不访问文件系统。
void MainMenuView::renderPackageFileSelectionWindow(float dpiScale)
{
    constexpr const char* popupId = "选择打包文件###PackageFileSelectionModal";
    if ( m_package.showFileSelectionWindow ) {
        ImGui::OpenPopup(popupId);
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool requestOutputPicker = false;
    bool closePopup          = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(760.0f * dpiScale, 540.0f * dpiScale),
                              false) ) {
            const auto selectedCount = static_cast<int>(
                std::count_if(m_package.candidateFiles.begin(),
                              m_package.candidateFiles.end(),
                              [](const PackageCandidateFile& file) {
                                  return file.selected;
                              }));
            ImGui::TextWrapped(
                "目标格式：%s  已选择：%d / %d",
                getPackageTypeDisplayName(m_package.selectedFileType).c_str(),
                selectedCount,
                static_cast<int>(m_package.candidateFiles.size()));

            const ImVec2 selectButtonSize(88.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("全选", selectButtonSize) ) {
                for ( auto& file : m_package.candidateFiles ) {
                    file.selected = true;
                }
                syncPackageDependencySelection();
            }
            sameLineIfItemFits(selectButtonSize.x);
            if ( ::MMM::UI::FeedbackButton("全不选", selectButtonSize) ) {
                for ( auto& file : m_package.candidateFiles ) {
                    file.selected = false;
                }
                syncPackageDependencySelection();
            }
            if ( shouldShowConvertedBeatmapSaveOption(
                     m_package.selectedFileType) ) {
                const std::string saveConvertedLabel =
                    "保存转换出的 " +
                    getPackageBeatmapExtension(m_package.selectedFileType) +
                    " 到项目中";
                sameLineIfItemFits(
                    getCheckboxDisplayWidth(saveConvertedLabel.c_str()));
                const bool canSaveConvertedBeatmaps =
                    hasSelectedPackageBeatmapSourceRequiringConversion(
                        m_package.selectedFileType, m_package.candidateFiles);
                if ( !canSaveConvertedBeatmaps ) {
                    m_package.saveConvertedBeatmapsToProject = false;
                    ImGui::BeginDisabled();
                }
                ::MMM::UI::FeedbackCheckbox(
                    saveConvertedLabel.c_str(),
                    &m_package.saveConvertedBeatmapsToProject);
                if ( !canSaveConvertedBeatmaps ) {
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered(
                             ImGuiHoveredFlags_AllowWhenDisabled) ) {
                        ImGui::SetTooltip(
                            "仅当已选谱面源需要转换为目标包谱面格式时可用。");
                    }
                }
            }
            if ( shouldShowLegacyImdPackageOption(
                     m_package.selectedFileType) ) {
                constexpr const char* legacyImdLabel =
                    "同时打包兼容旧皮肤的 .imd";
                sameLineIfItemFits(getCheckboxDisplayWidth(legacyImdLabel));
                ::MMM::UI::FeedbackCheckbox(
                    legacyImdLabel, &m_package.includeLegacyImdBeatmaps);
            }
            const bool hasAnyStoreModeExtCandidates =
                hasPackageStoreModeExtCandidates();
            const bool hasSelectedStoreModeExtCandidates =
                hasSelectedPackageStoreModeExtCandidates();
            if ( hasAnyStoreModeExtCandidates ) {
                constexpr const char* storeModeExtLabel =
                    "自动添加上架皮肤 mode_ext";
                sameLineIfItemFits(getCheckboxDisplayWidth(storeModeExtLabel));
                auto& settings =
                    Config::AppConfig::instance().getEditorSettings();
                bool addStoreModeExt =
                    settings.autoAddStoreModeExtForMalodyExport;
                if ( !hasSelectedStoreModeExtCandidates ) {
                    ImGui::BeginDisabled();
                }
                if ( ::MMM::UI::FeedbackCheckbox(storeModeExtLabel,
                                                 &addStoreModeExt) ) {
                    settings.autoAddStoreModeExtForMalodyExport =
                        addStoreModeExt;
                    Config::AppConfig::instance().save();
                }
                if ( !hasSelectedStoreModeExtCandidates ) {
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered(
                             ImGuiHoveredFlags_AllowWhenDisabled) ) {
                        ImGui::SetTooltip(
                            "%s",
                            "当前未选中含 Flick/折线的谱面，打包时不会写入 "
                            "mode_ext。");
                    }
                } else if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s",
                                      "打包 MCZ 时会替换所有写出的 .mc 的 "
                                      "mode_ext。");
                }
            }
            const bool hasMissingDependencies =
                hasSelectedPackageMissingDependencies();
            if ( hasMissingDependencies ) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "%s",
                    "所选谱面缺少或当前格式不支持其引用的图片/音频资源。");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImGuiStyle& style = ImGui::GetStyle();
            const float       footerReserveHeight =
                ImGui::GetFrameHeightWithSpacing() +
                style.ItemSpacing.y * 4.0f + 2.0f * dpiScale;
            const float listHeight = std::max(
                48.0f * dpiScale,
                ImGui::GetContentRegionAvail().y - footerReserveHeight);

            if ( ImGui::BeginChild("PackageCandidateFilesChild",
                                   ImVec2(0.0f, listHeight),
                                   true) ) {
                if ( m_package.candidateFiles.empty() ) {
                    ImGui::TextUnformatted(
                        "没有找到符合当前打包格式规则的文件。");
                } else {
                    constexpr ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY;
                    if ( ImGui::BeginTable("PackageCandidateFilesTable",
                                           3,
                                           tableFlags,
                                           ImVec2(0.0f, 0.0f)) ) {
                        ImGui::TableSetupColumn(
                            "打包",
                            ImGuiTableColumnFlags_WidthFixed,
                            64.0f * dpiScale);
                        ImGui::TableSetupColumn(
                            "类型",
                            ImGuiTableColumnFlags_WidthFixed,
                            72.0f * dpiScale);
                        ImGui::TableSetupColumn(
                            "文件", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for ( std::size_t index = 0;
                              index < m_package.candidateFiles.size();
                              ++index ) {
                            auto& file = m_package.candidateFiles[index];
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            const bool dependencyLocked =
                                file.requiredBySelectedBeatmaps > 0 &&
                                file.resourceType !=
                                    PackageResourceType::Beatmap;
                            bool selected = file.selected;
                            if ( dependencyLocked ) ImGui::BeginDisabled();
                            if ( ::MMM::UI::FeedbackCheckbox(
                                     "##PackageFileSelected", &selected) ) {
                                setPackageCandidateSelected(index, selected);
                            }
                            if ( dependencyLocked ) ImGui::EndDisabled();
                            if ( dependencyLocked &&
                                 ImGui::IsItemHovered(
                                     ImGuiHoveredFlags_AllowWhenDisabled) ) {
                                ImGui::SetTooltip(
                                    "该资源被已选中谱面引用，不能单独取消。");
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(file.typeLabel.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(file.relativePath.c_str());
                            if ( file.selected &&
                                 file.resourceType ==
                                     PackageResourceType::Beatmap &&
                                 !file.missingDependencyRelativePaths
                                      .empty() ) {
                                ImGui::SameLine();
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                    "%s",
                                    "(缺少依赖)");
                                if ( ImGui::IsItemHovered() ) {
                                    std::string tooltip = "未能绑定以下资源：";
                                    for (
                                        const auto& missingPath :
                                        file.missingDependencyRelativePaths ) {
                                        tooltip += "\n";
                                        tooltip += missingPath;
                                    }
                                    ImGui::SetTooltip("%s", tooltip.c_str());
                                }
                            }
                            ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool   canPack = selectedCount > 0 && !hasMissingDependencies;
            const ImVec2 footerButtonSize(120.0f * dpiScale, 0.0f);
            const float  footerButtonRowWidth =
                footerButtonSize.x * 2.0f + style.ItemSpacing.x;
            centerNextItem(footerButtonRowWidth);
            if ( !canPack ) ImGui::BeginDisabled();
            if ( ::MMM::UI::FeedbackButton("打包到...", footerButtonSize) ) {
                m_package.pendingRelativePaths =
                    collectSelectedPackageRelativePaths();
                if ( preparePackageBeatmapMetadataEdits(
                         m_package.pendingRelativePaths) ) {
                    m_package.showBeatmapMetadataWindow = true;
                } else {
                    requestOutputPicker = true;
                }
                closePopup = true;
            }
            if ( !canPack ) ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           footerButtonSize) ) {
                closePopup = true;
                m_package.pendingRelativePaths.clear();
            }

            if ( closePopup ) {
                m_package.showFileSelectionWindow = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if ( requestOutputPicker ) {
        openPackageOutputFilePicker();
    }
}

/// @brief 为选中的谱面准备打包转换前的元数据补充项。
/// @param selectedRelativePaths 当前已选的项目相对路径列表。
/// @return 需要展示补充窗口时返回 true。
bool MainMenuView::preparePackageBeatmapMetadataEdits(
    const std::vector<std::string>& selectedRelativePaths)
{
    m_package.beatmapMetadataEdits.clear();
    m_package.pendingMetadataOverrides.clear();

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return false;

    for ( const auto& relativePathUtf8 : selectedRelativePaths ) {
        const auto relativePath =
            Config::utf8ToPath(relativePathUtf8).lexically_normal();
        const auto extension =
            toLowerAscii(Config::pathToUtf8(relativePath.extension()));
        if ( !shouldPreparePackageBeatmapMetadataEdit(
                 m_package.selectedFileType, extension) ) {
            continue;
        }

        const auto sourcePath =
            (project->m_projectRoot / relativePath).lexically_normal();
        BeatMap beatMap = BeatMap::loadFromFile(sourcePath);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) continue;

        PackageBeatmapMetadataEdit edit;
        edit.relativePath = Config::pathToUtf8Generic(relativePath);
        edit.baseMeta     = beatMap.m_baseMapMetadata;

        if ( edit.baseMeta.title.empty() ) {
            edit.baseMeta.title = edit.baseMeta.title_unicode.empty()
                                      ? Config::pathToUtf8(relativePath.stem())
                                      : edit.baseMeta.title_unicode;
        }
        if ( edit.baseMeta.title_unicode.empty() ) {
            edit.baseMeta.title_unicode = edit.baseMeta.title;
        }
        if ( edit.baseMeta.artist.empty() ) {
            edit.baseMeta.artist = project->m_metadata.m_artist;
        }
        if ( edit.baseMeta.artist_unicode.empty() ) {
            edit.baseMeta.artist_unicode = edit.baseMeta.artist;
        }
        if ( edit.baseMeta.author.empty() ) {
            edit.baseMeta.author = project->m_metadata.m_mapper;
        }
        if ( edit.baseMeta.version.empty() ||
             edit.baseMeta.version == "unknown" ) {
            auto entryIt =
                std::find_if(project->m_beatmaps.begin(),
                             project->m_beatmaps.end(),
                             [&](const MMM::Project::BeatmapEntry& entry) {
                                 return entry.m_filePath == edit.relativePath;
                             });
            edit.baseMeta.version =
                entryIt != project->m_beatmaps.end() && !entryIt->m_name.empty()
                    ? entryIt->m_name
                    : "default";
        }

        copyToPackageInputBuffer(edit.titleBuffer, edit.baseMeta.title);
        copyToPackageInputBuffer(edit.titleUnicodeBuffer,
                                 edit.baseMeta.title_unicode);
        copyToPackageInputBuffer(edit.artistBuffer, edit.baseMeta.artist);
        copyToPackageInputBuffer(edit.artistUnicodeBuffer,
                                 edit.baseMeta.artist_unicode);
        copyToPackageInputBuffer(edit.creatorBuffer, edit.baseMeta.author);
        copyToPackageInputBuffer(edit.versionBuffer, edit.baseMeta.version);
        m_package.beatmapMetadataEdits.push_back(std::move(edit));
    }

    return !m_package.beatmapMetadataEdits.empty();
}

/// @brief 从补充窗口缓存收集打包元数据覆盖项。
/// @return 元数据覆盖项列表。
std::vector<Logic::PackageBeatmapMetadataOverride>
MainMenuView::collectPackageMetadataOverridesFromEdits()
{
    std::vector<Logic::PackageBeatmapMetadataOverride> overrides;
    overrides.reserve(m_package.beatmapMetadataEdits.size());
    for ( auto& edit : m_package.beatmapMetadataEdits ) {
        edit.baseMeta.title = packageInputBufferText(edit.titleBuffer);
        edit.baseMeta.title_unicode =
            packageInputBufferText(edit.titleUnicodeBuffer);
        edit.baseMeta.artist = packageInputBufferText(edit.artistBuffer);
        edit.baseMeta.artist_unicode =
            packageInputBufferText(edit.artistUnicodeBuffer);
        edit.baseMeta.author  = packageInputBufferText(edit.creatorBuffer);
        edit.baseMeta.version = packageInputBufferText(edit.versionBuffer);

        overrides.push_back(Logic::PackageBeatmapMetadataOverride{
            .relativePath = edit.relativePath,
            .baseMeta     = edit.baseMeta,
        });
    }
    return overrides;
}

/// @brief 渲染打包前补充目标谱面元数据的窗口。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderPackageBeatmapMetadataWindow(float dpiScale)
{
    constexpr const char* popupId =
        "补充谱面元数据###PackageBeatmapMetadataModal";
    if ( m_package.showBeatmapMetadataWindow ) {
        ImGui::OpenPopup(popupId);
        m_package.showBeatmapMetadataWindow = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool requestOutputPicker = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(720.0f * dpiScale, 520.0f * dpiScale),
                              false) ) {
            const std::string prompt =
                makePackageMetadataEditPrompt(m_package.selectedFileType);
            ImGui::TextUnformatted(prompt.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImGuiStyle& style = ImGui::GetStyle();
            const float       footerReserveHeight =
                ImGui::GetFrameHeightWithSpacing() +
                style.ItemSpacing.y * 4.0f + 2.0f * dpiScale;
            const float editHeight = std::max(
                120.0f * dpiScale,
                ImGui::GetContentRegionAvail().y - footerReserveHeight);

            if ( ImGui::BeginChild("PackageBeatmapMetadataEditChild",
                                   ImVec2(0.0f, editHeight),
                                   true) ) {
                for ( std::size_t index = 0;
                      index < m_package.beatmapMetadataEdits.size();
                      ++index ) {
                    auto& edit = m_package.beatmapMetadataEdits[index];
                    ImGui::PushID(static_cast<int>(index));
                    if ( ::MMM::UI::FeedbackCollapsingHeader(
                             edit.relativePath.c_str(),
                             ImGuiTreeNodeFlags_DefaultOpen) ) {
                        if ( ImGui::BeginTable(
                                 "PackageBeatmapMetadataFields",
                                 2,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_NoSavedSettings) ) {
                            ImGui::TableSetupColumn(
                                "字段",
                                ImGuiTableColumnFlags_WidthFixed,
                                112.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "值", ImGuiTableColumnFlags_WidthStretch);

                            auto inputRow = [](const char* label,
                                               const char* id,
                                               auto&       buffer) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(label);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1.0f);
                                ImGui::InputText(
                                    id, buffer.data(), buffer.size());
                            };

                            inputRow("Title", "##Title", edit.titleBuffer);
                            inputRow("TitleOrg",
                                     "##TitleUnicode",
                                     edit.titleUnicodeBuffer);
                            inputRow("Artist", "##Artist", edit.artistBuffer);
                            inputRow("ArtistOrg",
                                     "##ArtistUnicode",
                                     edit.artistUnicodeBuffer);
                            inputRow(
                                "Creator", "##Creator", edit.creatorBuffer);
                            inputRow(
                                "Version", "##Version", edit.versionBuffer);

                            ImGui::EndTable();
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            const float  buttonRowWidth =
                buttonSize.x * 2.0f + style.ItemSpacing.x;
            centerNextItem(buttonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续打包", buttonSize) ) {
                m_package.pendingMetadataOverrides =
                    collectPackageMetadataOverridesFromEdits();
                m_package.beatmapMetadataEdits.clear();
                requestOutputPicker = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                m_package.pendingRelativePaths.clear();
                m_package.pendingMetadataOverrides.clear();
                m_package.beatmapMetadataEdits.clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    if ( requestOutputPicker ) {
        openPackageOutputFilePicker();
    }
}

/// @brief 按当前目标打包格式重建候选文件列表。
/// @warning 低频 UI
/// 路径：打开打包选择窗口或切换格式时执行；会扫描项目目录并读取谱面元数据。
void MainMenuView::rebuildPackageCandidateFiles()
{
    m_package.candidateFiles.clear();

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return;

    const auto& types =
        getPackageSupportedFileTypes(m_package.selectedFileType);
    const auto& projectRoot = project->m_projectRoot;

    std::error_code filesystemError;
    if ( !std::filesystem::exists(projectRoot, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(projectRoot, filesystemError) ||
         filesystemError ) {
        m_statusMessage      = "扫描项目文件失败";
        m_statusMessageTimer = 3.0f;
        return;
    }

    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(
        projectRoot, directoryOptions, filesystemError);
    const std::filesystem::recursive_directory_iterator endIterator;
    if ( filesystemError ) {
        m_statusMessage      = "扫描项目文件失败";
        m_statusMessageTimer = 3.0f;
        return;
    }

    while ( iterator != endIterator ) {
        const auto& entry = *iterator;
        if ( entry.is_regular_file(filesystemError) && !filesystemError ) {
            const auto path = entry.path();
            const auto extension =
                toLowerAscii(Config::pathToUtf8(path.extension()));
            const auto typeLabel =
                getPackageCandidateTypeLabel(types, extension);
            const auto resourceType =
                getPackageCandidateResourceType(types, extension);
            std::error_code relativeError;
            const auto      relativePath =
                std::filesystem::relative(path, projectRoot, relativeError);
            if ( !typeLabel.empty() && resourceType &&
                 !isPackageArchiveExtension(extension) && !relativeError ) {
                m_package.candidateFiles.push_back(PackageCandidateFile{
                    .relativePath = Config::pathToUtf8Generic(relativePath),
                    .typeLabel    = typeLabel,
                    .resourceType = *resourceType,
                    .selected     = true,
                });
            }
        }
        filesystemError.clear();

        iterator.increment(filesystemError);
        if ( filesystemError ) {
            m_statusMessage      = "扫描项目文件失败";
            m_statusMessageTimer = 3.0f;
            break;
        }
    }

    std::sort(
        m_package.candidateFiles.begin(),
        m_package.candidateFiles.end(),
        [](const PackageCandidateFile& lhs, const PackageCandidateFile& rhs) {
            if ( lhs.typeLabel != rhs.typeLabel ) {
                return lhs.typeLabel < rhs.typeLabel;
            }
            return lhs.relativePath < rhs.relativePath;
        });

    std::unordered_map<std::string, std::size_t> candidateIndexByPath;
    candidateIndexByPath.reserve(m_package.candidateFiles.size());
    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        m_package.candidateFiles[index].relativePath =
            normalizePackageRelativeUtf8(
                m_package.candidateFiles[index].relativePath);
        candidateIndexByPath[m_package.candidateFiles[index].relativePath] =
            index;
    }

    for ( auto& file : m_package.candidateFiles ) {
        if ( file.resourceType != PackageResourceType::Beatmap ) continue;

        auto beatmapInfo =
            collectPackageBeatmapInfo(*project, file.relativePath);
        file.dependencyRelativePaths =
            std::move(beatmapInfo.dependencyRelativePaths);
        file.hasStoreModeExtEligibleElements =
            beatmapInfo.hasStoreModeExtEligibleElements;
        file.missingDependencyRelativePaths.clear();
        for ( const auto& dependencyPath : file.dependencyRelativePaths ) {
            if ( candidateIndexByPath.find(dependencyPath) ==
                 candidateIndexByPath.end() ) {
                file.missingDependencyRelativePaths.push_back(dependencyPath);
            }
        }
    }

    syncPackageDependencySelection();
    if ( !hasSelectedPackageBeatmapSourceRequiringConversion(
             m_package.selectedFileType, m_package.candidateFiles) ) {
        m_package.saveConvertedBeatmapsToProject = false;
    }
}

/// @brief 设置候选文件选中状态，并同步谱面绑定资源。
/// @param index 候选文件索引。
/// @param selected 是否选中。
/// @warning UI 热路径：打包选择弹窗可见时由用户操作触发；只更新候选列表缓存。
void MainMenuView::setPackageCandidateSelected(std::size_t index, bool selected)
{
    if ( index >= m_package.candidateFiles.size() ) return;

    auto& file = m_package.candidateFiles[index];
    if ( !selected && file.requiredBySelectedBeatmaps > 0 &&
         file.resourceType != PackageResourceType::Beatmap ) {
        return;
    }

    file.selected = selected;
    syncPackageDependencySelection();
    if ( !hasSelectedPackageBeatmapSourceRequiringConversion(
             m_package.selectedFileType, m_package.candidateFiles) ) {
        m_package.saveConvertedBeatmapsToProject = false;
    }
}

/// @brief 根据当前已选中谱面重新计算依赖资源锁定状态。
/// @warning UI 热路径：打包选择弹窗可见时由用户操作触发；按候选数量线性更新。
void MainMenuView::syncPackageDependencySelection()
{
    std::vector<bool> wasLocked;
    wasLocked.reserve(m_package.candidateFiles.size());
    for ( auto& file : m_package.candidateFiles ) {
        wasLocked.push_back(file.requiredBySelectedBeatmaps > 0);
        file.requiredBySelectedBeatmaps = 0;
    }

    std::unordered_map<std::string, std::size_t> candidateIndexByPath;
    candidateIndexByPath.reserve(m_package.candidateFiles.size());
    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        candidateIndexByPath[m_package.candidateFiles[index].relativePath] =
            index;
    }

    for ( const auto& file : m_package.candidateFiles ) {
        if ( !file.selected ||
             file.resourceType != PackageResourceType::Beatmap ) {
            continue;
        }

        for ( const auto& dependencyPath : file.dependencyRelativePaths ) {
            const auto dependencyIt = candidateIndexByPath.find(dependencyPath);
            if ( dependencyIt == candidateIndexByPath.end() ) continue;
            auto& dependencyFile =
                m_package.candidateFiles[dependencyIt->second];
            dependencyFile.requiredBySelectedBeatmaps++;
        }
    }

    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        auto& file = m_package.candidateFiles[index];
        if ( file.requiredBySelectedBeatmaps > 0 ) {
            file.selected = true;
        } else if ( index < wasLocked.size() && wasLocked[index] &&
                    file.resourceType != PackageResourceType::Beatmap ) {
            file.selected = false;
        }
    }
}

/// @brief 判断当前选中谱面是否存在未能绑定的依赖资源。
/// @return 存在缺失依赖时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
bool MainMenuView::hasSelectedPackageMissingDependencies() const
{
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  !file.missingDependencyRelativePaths.empty();
                       });
}

/// @brief 判断当前 MCZ 候选列表是否存在可写入上架 mode_ext 的谱面。
/// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
bool MainMenuView::hasPackageStoreModeExtCandidates() const
{
    if ( m_package.selectedFileType != PackageFileType::Mcz ) {
        return false;
    }
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  file.hasStoreModeExtEligibleElements;
                       });
}

/// @brief 判断当前选中的 MCZ 谱面是否需要显示上架 mode_ext 选项。
/// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
bool MainMenuView::hasSelectedPackageStoreModeExtCandidates() const
{
    if ( m_package.selectedFileType != PackageFileType::Mcz ) {
        return false;
    }
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  file.hasStoreModeExtEligibleElements;
                       });
}

/// @brief 收集当前已勾选的项目相对文件路径。
/// @return 已勾选的项目相对文件路径列表。
/// @warning UI 热路径低频分支：点击确认打包时执行；只遍历候选缓存。
std::vector<std::string>
MainMenuView::collectSelectedPackageRelativePaths() const
{
    std::vector<std::string> selectedPaths;
    selectedPaths.reserve(m_package.candidateFiles.size());
    for ( const auto& file : m_package.candidateFiles ) {
        if ( file.selected ) {
            selectedPaths.push_back(file.relativePath);
        }
    }
    return selectedPaths;
}

/// @brief 生成当前打包目标格式的默认输出文件名。
/// @return 默认输出文件名。
std::string MainMenuView::makePackageDefaultFileName() const
{
    std::string baseName = "map";
    auto*       project  = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        baseName = Config::pathToUtf8(project->m_projectRoot.filename());
    }
    if ( baseName.empty() ) baseName = "map";
    return sanitizePackageFileNamePart(baseName) +
           getPackageExtension(m_package.selectedFileType);
}

/// @brief 打开谱面打包流程。
void MainMenuView::openPackFilePicker()
{
    m_package.candidateFiles.clear();
    m_package.pendingRelativePaths.clear();
    m_package.pendingMetadataOverrides.clear();
    m_package.beatmapMetadataEdits.clear();
    m_package.showFileSelectionWindow   = false;
    m_package.showBeatmapMetadataWindow = false;
    m_package.showFormatPicker          = true;
    if ( !shouldShowLegacyImdPackageOption(m_package.selectedFileType) ) {
        m_package.includeLegacyImdBeatmaps = false;
    }
}

/// @brief 打开打包输出路径选择器。
void MainMenuView::openPackageOutputFilePicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    const std::string defaultFileName = makePackageDefaultFileName();
    const std::string defaultPath     = getPackagePickerDefaultPath(config);
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t* outPath = nullptr;
        const char*  packageFilter =
            getNativePackageOutputFilterText(m_package.selectedFileType);
        nfdu8filteritem_t filters[1] = { { "Beatmap Package", packageFilter } };
        nfdresult_t       result     = NFD_SaveDialogU8(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());

        if ( result == NFD_OKAY ) {
            requestPackBeatmapTo(outPath);
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = defaultPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultFileName;
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
        const char* packageFilter =
            getUnifiedPackageOutputFilterText(m_package.selectedFileType);
        ImGuiFileDialog::Instance()->OpenDialog(
            "PackFilePicker", TR("ui.file.pack"), packageFilter, fdConfig);
    }
}

}  // namespace MMM::UI
