#include "logic/ProjectStorage.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Logic
{
namespace
{

/// @brief 当前分片项目存储格式版本。
/// 与 Project 的业务字段演进分开，表示这里的文件布局协议。
constexpr int SPLIT_STORAGE_FORMAT_VERSION = 1;

/// @brief 隐藏项目配置目录名。
/// 所有分片以项目根目录为基准，不依赖应用当前工作目录。
constexpr std::string_view STORAGE_DIRECTORY_NAME = ".mmm";

/// @brief 分片格式入口文件名。
/// 保存时最后写入；读取时用它判定项目是否采用分片布局。
constexpr std::string_view MANIFEST_FILE_NAME = "manifest.json";

/// @brief 项目元数据与排除项文件名。
/// 排除列表与元数据一起保存，避免资源扫描恢复用户主动排除的项。
constexpr std::string_view PROJECT_FILE_NAME = "project.json";

/// @brief 项目设置文件名。
/// 工作区从此分片中拆出，防止同一状态同时存在两个写入来源。
constexpr std::string_view SETTINGS_FILE_NAME = "settings.json";

/// @brief 音频资源文件名。
/// 保存资源描述，不在这里复制音频媒体本体。
constexpr std::string_view AUDIO_RESOURCES_FILE_NAME = "audio_resources.json";

/// @brief 谱面入口文件名。
/// 保存项目中的谱面引用，谱面内容仍由各自文件持有。
constexpr std::string_view BEATMAPS_FILE_NAME = "beatmaps.json";

/// @brief 项目级草稿轨数据文件名。
/// 较早的分片项目可以缺失此文件，加载时按空组处理。
constexpr std::string_view DRAFT_LANES_FILE_NAME = "draft_lanes.json";

/// @brief 一般项目工作区文件名。
/// 音频工具专有字段另存，其他工作区字段沿用领域序列化结果。
constexpr std::string_view WORKSPACE_FILE_NAME = "workspace.json";

/// @brief 项目音频工具状态与布局文件名。
/// 同时保存工具的选择状态、试听偏好与摆放信息。
constexpr std::string_view AUDIO_TOOL_FILE_NAME = "project_audio_tool.json";

/// @brief 旧版根目录项目配置文件名。
/// 新布局载入失败时仍可回退读取，迁移后的删除由单独接口执行。
constexpr std::string_view LEGACY_PROJECT_FILE_NAME = "mmm_project.json";

/// @brief 读取一个 JSON 文件并要求其顶层是对象。
/// @param path 要读取的完整分片路径。
/// @param output 接收解析结果；失败时可能已被 discarded 或非对象值替换。
/// @param errorMessage 失败时写入带具体分片路径的原因。
/// @return 文件可读且顶层为对象时为 true，字段契约由调用方继续检查。
/// @warning 同步文件 IO，只能在项目加载等低频路径调用。
/// 此 helper 只验证对象外形，不负责 Project 内部字段的类型转换。
bool readJsonObject(const std::filesystem::path& path, nlohmann::json& output,
                    std::string& errorMessage)
{
    std::ifstream file(path);
    // 使用 filesystem::path 打开文件，日志才转 UTF-8，避免路径编码来回转换。
    if ( !file.is_open() ) {
        errorMessage = "无法打开配置分片：" + Config::pathToUtf8(path);
        // 缺失与不可读都返回失败，可选分片由上层在调用前决定是否跳过。
        return false;
    }
    // 禁用 JSON 语法解析异常，格式错误经返回状态统一交给加载结果处理。
    // 顶层数组或标量不是配置对象，即使语法正确也不能进入字段合并。
    output = nlohmann::json::parse(file, nullptr, false);
    if ( output.is_discarded() || !output.is_object() || file.bad() ) {
        // 解析结果与底层读取错误都检查，避免把损坏的输入当作有效分片。
        errorMessage = "无法解析配置分片：" + Config::pathToUtf8(path);
        return false;
    }
    return true;
}

/// @brief 先写临时文件，再重命名或复制替换目标分片。
/// @param path 目标路径，父目录由 save 预先创建。
/// @param value 本分片的完整对象，不对旧内容做增量合并。
/// @param errorMessage 失败时接收创建、写入或替换阶段的错误。
/// @return 本分片写入并替换成功时为 true。
/// @warning 同步文件 IO；固定 .tmp 名称要求同一路径的保存串行执行。
/// 重命名失败时退回覆盖复制，因此不承诺所有平台上均原子替换。
/// 临时文件名从完整目标名追加后缀，不会误写另一个职责的 JSON 分片。
bool writeJsonAtomically(const std::filesystem::path& path,
                         const nlohmann::json& value, std::string& errorMessage)
{
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    // 临时文件和目标位于同一目录，优先让重命名在同一文件系统内完成。
    {
        std::ofstream file(tempPath, std::ios::trunc);
        // 截断的是临时文件；生成 JSON 期间不直接破坏已有目标内容。
        if ( !file.is_open() ) {
            errorMessage =
                "无法创建配置临时文件：" + Config::pathToUtf8(tempPath);
            return false;
        }
        // 格式化输出便于人工维护分片，末尾换行保持文本工具兼容。
        file << std::setw(4) << value << '\n';
        if ( !file.good() ) {
            // 写出失败时不继续 rename，已有目标文件仍由上一次保存保留。
            errorMessage =
                "无法写入配置临时文件：" + Config::pathToUtf8(tempPath);
            return false;
        }
    }

    std::error_code replaceError;
    // 流已离开作用域，替换操作不再与仍打开的输出流共用临时文件。
    std::filesystem::rename(tempPath, path, replaceError);
    if ( !replaceError ) return true;

    // 某些平台无法 rename 覆盖已有文件，沿用复制覆盖的兼容路径。
    // 此回退不是多文件事务，也不尝试恢复已经被部分覆盖的目标。
    std::error_code copyError;
    std::filesystem::copy_file(
        tempPath,
        path,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    std::error_code removeError;
    // 复制尝试结束后清理临时文件；返回成败以目标复制结果为准。
    std::filesystem::remove(tempPath, removeError);
    // 临时文件删除失败不会把已经成功覆盖的目标重新判为未保存。
    if ( copyError ) {
        // 报告最终替换错误，而不是掩盖回退结果的首次重命名错误。
        errorMessage = "无法替换配置分片：" + Config::pathToUtf8(path) +
                       "，错误：" + copyError.message();
        return false;
    }
    return true;
}

/// @brief 从分片内容组装与旧版 Project JSON 相同的结构。
/// @param projectJson 元数据及资源排除列表。
/// @param settingsJson 设置的值副本，函数向其重新装入工作区。
/// @param audioResourcesJson 音频资源描述数组的包装对象。
/// @param beatmapsJson 谱面入口数组的包装对象。
/// @param draftLanesJson 草稿轨分组，兼容没有该字段的旧配置。
/// @param workspaceJson 工作区副本，接收音频工具专有字段。
/// @param audioToolJson 音频工具状态分片，缺失字段使用明确默认值。
/// @return 可直接传给 Project 反序列化器的旧顶层 JSON 形状。
/// @pre 必填顶层字段由 loadSplit 检查，字段值的转换遵循领域序列化契约。
/// 可选字段仅在缺失时使用默认值，不把显式保存的值改成默认配置。
nlohmann::json assembleProjectJson(const nlohmann::json& projectJson,
                                   nlohmann::json        settingsJson,
                                   const nlohmann::json& audioResourcesJson,
                                   const nlohmann::json& beatmapsJson,
                                   const nlohmann::json& draftLanesJson,
                                   nlohmann::json        workspaceJson,
                                   const nlohmann::json& audioToolJson)
{
    // 拆分只改变磁盘布局；先把工具状态还原进工作区，再装入设置。
    // 音频工具字段采用独立分片为准，覆盖工作区中可能残留的同名旧值。
    workspaceJson["m_projectAudioToolSelectedResourceId"] = audioToolJson.value(
        "m_projectAudioToolSelectedResourceId", std::string{});
    workspaceJson["m_projectAudioToolBrushVolume"] =
        audioToolJson.value("m_projectAudioToolBrushVolume", 1.0F);
    // 选择试听和窗口打开状态默认关闭，不在旧项目加载后产生额外操作。
    workspaceJson["m_projectAudioToolPreviewEffectOnSelection"] =
        audioToolJson.value("m_projectAudioToolPreviewEffectOnSelection",
                            false);
    workspaceJson["m_projectAudioToolOpen"] =
        audioToolJson.value("m_projectAudioToolOpen", false);
    workspaceJson["m_projectAudioToolPlacements"] = audioToolJson.value(
        "m_projectAudioToolPlacements", nlohmann::json::array());
    // 摆放列表保持资源 ID 与位置的原始关系，不在存储层重新排布工具界面。
    // 使用值副本合并，不会修改读取阶段保留的其他分片对象。
    settingsJson["m_workspace"] = std::move(workspaceJson);

    // 必填集合使用 at 读取已校验字段，可选新增字段则提供空集合默认值。
    // 原始顶层结构还会交给迁移调用者，不能仅返回反序列化后的 Project。
    return nlohmann::json{
        { "m_metadata", projectJson.at("m_metadata") },
        { "m_settings", std::move(settingsJson) },
        { "m_audioResources", audioResourcesJson.at("m_audioResources") },
        { "m_beatmaps", beatmapsJson.at("m_beatmaps") },
        { "m_draftLaneGroups",
          draftLanesJson.value("m_draftLaneGroups", nlohmann::json::array()) },
        { "m_excludedBeatmapPaths",
          projectJson.value("m_excludedBeatmapPaths",
                            std::vector<std::string>{}) },
        { "m_excludedAudioPaths",
          projectJson.value("m_excludedAudioPaths",
                            std::vector<std::string>{}) },
    };
}

}  // namespace

/// @brief 计算项目隐藏配置目录，不触碰文件系统。
/// @param projectRoot 调用方选定的项目根目录。
/// @return 根目录下的 .mmm 路径，是否存在由读写入口判断。
std::filesystem::path ProjectStorage::storageDirectory(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / STORAGE_DIRECTORY_NAME;
}

/// @brief 计算分片布局的入口文件位置。
/// @param projectRoot 与所有分片共用的项目根目录。
/// @return .mmm/manifest.json，不创建目录或文件。
std::filesystem::path ProjectStorage::manifestPath(
    const std::filesystem::path& projectRoot)
{
    return storageDirectory(projectRoot) / MANIFEST_FILE_NAME;
}

/// @brief 计算兼容旧布局的根目录配置路径。
/// @param projectRoot 包含旧 mmm_project.json 的目录。
/// @return 不带 .mmm 子目录的旧配置位置。
std::filesystem::path ProjectStorage::legacyProjectFilePath(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / LEGACY_PROJECT_FILE_NAME;
}

/// @brief 检查分片入口或旧文件是否为可识别的常规文件。
/// @param projectRoot 待识别的项目目录，不递归搜索子目录。
/// @return 发现任一入口时为 true；不保证配置内容可以成功加载。
/// @warning 低频文件系统查询，不能在逐帧界面分支重复执行。
bool ProjectStorage::hasProjectConfiguration(
    const std::filesystem::path& projectRoot)
{
    std::error_code filesystemError;
    if ( std::filesystem::is_regular_file(manifestPath(projectRoot),
                                          filesystemError) &&
         !filesystemError ) {
        // 入口存在即可识别项目，内容损坏留给 load 返回具体错误。
        return true;
    }
    filesystemError.clear();
    // 新入口不存在或查询失败仍尝试旧路径，不把第一次查询状态带到回退。
    return std::filesystem::is_regular_file(legacyProjectFilePath(projectRoot),
                                            filesystemError) &&
           !filesystemError;
}

/// @brief 按新布局优先的策略选择配置来源。
/// @param projectRoot 要打开的项目根目录。
/// @return 成功结果或所选来源的错误；完全没有入口时返回默认空结果。
/// @warning 同步加载和 JSON 分配，应在项目切换路径执行。
ProjectStorage::LoadResult ProjectStorage::load(
    const std::filesystem::path& projectRoot) const
{
    std::error_code filesystemError;
    const bool      splitExists = std::filesystem::is_regular_file(
                                 manifestPath(projectRoot), filesystemError) &&
                             !filesystemError;
    filesystemError.clear();
    const bool legacyExists =
        std::filesystem::is_regular_file(legacyProjectFilePath(projectRoot),
                                         filesystemError) &&
        !filesystemError;
    // 两次存在性查询彼此独立；回退只在旧入口确实可识别时尝试。

    if ( splitExists ) {
        // 同时存在两种布局时先读分片；成功后不再用旧文件覆盖新数据。
        auto splitResult = loadSplit(projectRoot);
        // 没有旧入口就保留分片错误，调用者可据此区分损坏与无配置。
        if ( splitResult.m_success || !legacyExists ) return splitResult;
        XWARN(
            // 回退成功也保留新布局失败的诊断，避免用户误以为读取了最新分片。
            "Failed to load split project storage, falling back to legacy "
            "file: {}",
            splitResult.m_errorMessage);
    }
    if ( legacyExists ) return loadLegacy(projectRoot);
    // 不存在配置不是解析失败，留空错误供上层决定是否建立新项目。
    return {};
}

/// @brief 读取必需分片，补兼容默认值并构造项目。
/// @param projectRoot 新布局所在项目根目录。
/// @return source 为 Split 的结果，只有完整组装后才置成功标志。
/// 失败只丢弃内存候选，不修改或清理任何已存在的配置文件。
ProjectStorage::LoadResult ProjectStorage::loadSplit(
    const std::filesystem::path& projectRoot) const
{
    LoadResult result;
    // 在 IO 前记录尝试的来源，使失败结果仍能用于诊断具体布局。
    result.m_source      = Source::Split;
    const auto directory = storageDirectory(projectRoot);

    nlohmann::json manifestJson;
    nlohmann::json projectJson;
    nlohmann::json settingsJson;
    // 每类配置独立接收，尚未验证前不往共享 Project 实例逐项写入。
    nlohmann::json audioResourcesJson;
    nlohmann::json beatmapsJson;
    nlohmann::json draftLanesJson{
        // 草稿轨分片为后加功能，缺失时提供可反序列化的空分组。
        { "m_draftLaneGroups", nlohmann::json::array() },
    };
    nlohmann::json workspaceJson;
    nlohmann::json audioToolJson;
    // 按固定文件名读取，不从 manifest 拼接外部路径或递归发现分片。
    // 短路链保留首个失败原因，避免后续错误覆盖实际最早损坏的位置。
    if ( !readJsonObject(directory / MANIFEST_FILE_NAME,
                         manifestJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / PROJECT_FILE_NAME,
                         projectJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / SETTINGS_FILE_NAME,
                         settingsJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / AUDIO_RESOURCES_FILE_NAME,
                         audioResourcesJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / BEATMAPS_FILE_NAME,
                         beatmapsJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / WORKSPACE_FILE_NAME,
                         workspaceJson,
                         result.m_errorMessage) ||
         !readJsonObject(directory / AUDIO_TOOL_FILE_NAME,
                         audioToolJson,
                         result.m_errorMessage) ) {
        return result;
    }

    const auto draftLanesPath = directory / DRAFT_LANES_FILE_NAME;
    // 可选文件不存在时使用空组；存在但解析损坏仍拒绝整个新布局。
    std::error_code draftLanesError;
    if ( std::filesystem::is_regular_file(draftLanesPath, draftLanesError) &&
         !draftLanesError &&
         !readJsonObject(
             draftLanesPath, draftLanesJson, result.m_errorMessage) ) {
        return result;
    }

    // 核心数组检查只约束容器形状，元素字段仍交由 Project 的转换逻辑读取。
    // 存储版本与必填集合形状先检查，避免把其他 JSON 对象误当作项目。
    // manifest 缺少版本使用零，明确落入不支持分支。
    if ( manifestJson.value("format_version", 0) !=
             SPLIT_STORAGE_FORMAT_VERSION ||
         !projectJson.contains("m_metadata") ||
         !audioResourcesJson.contains("m_audioResources") ||
         !audioResourcesJson["m_audioResources"].is_array() ||
         !beatmapsJson.contains("m_beatmaps") ||
         !beatmapsJson["m_beatmaps"].is_array() ) {
        result.m_errorMessage = "项目分片格式无效或版本不受支持";
        return result;
    }

    // 保留拼装后的原始字段，迁移层可区分“未存储”与领域默认值。
    result.m_serializedProject = assembleProjectJson(projectJson,
                                                     std::move(settingsJson),
                                                     audioResourcesJson,
                                                     beatmapsJson,
                                                     draftLanesJson,
                                                     std::move(workspaceJson),
                                                     audioToolJson);
    result.m_project           = result.m_serializedProject.get<Project>();
    // 领域对象和原始 JSON 同时保留，运行时使用前者，字段级迁移使用后者。
    // 成功标志最后设置，调用者不能消费还未反序列化完成的项目。
    result.m_success = true;
    return result;
}

/// @brief 加载旧顶层项目对象，不在读取过程中实施磁盘迁移。
/// @param projectRoot 包含旧单文件配置的项目目录。
/// @return source 为 Legacy 的候选，字段检查通过后才生成领域对象。
ProjectStorage::LoadResult ProjectStorage::loadLegacy(
    const std::filesystem::path& projectRoot) const
{
    LoadResult result;
    result.m_source = Source::Legacy;
    // 原始对象直接保留，不先拆分再重组，以便上层识别旧字段缺省情况。
    if ( !readJsonObject(legacyProjectFilePath(projectRoot),
                         result.m_serializedProject,
                         result.m_errorMessage) ) {
        return result;
    }
    // 旧格式必须有设置及两种资源集合，缺少核心字段不能作为有效回退。
    if ( !result.m_serializedProject.contains("m_metadata") ||
         !result.m_serializedProject.contains("m_settings") ||
         !result.m_serializedProject.contains("m_audioResources") ||
         !result.m_serializedProject["m_audioResources"].is_array() ||
         !result.m_serializedProject.contains("m_beatmaps") ||
         !result.m_serializedProject["m_beatmaps"].is_array() ) {
        result.m_errorMessage = "旧项目配置结构无效";
        return result;
    }

    result.m_project = result.m_serializedProject.get<Project>();
    // 旧文件的成功读取不代表已经迁移；来源标志仍明确保持 Legacy。
    result.m_success = true;
    return result;
}

/// @brief 把项目值快照拆分为按职责组织的配置文件。
/// @param project 调用方稳定持有的项目数据，保存期间不得并发修改。
/// @param projectRoot 保存目录，允许 .mmm 尚未创建。
/// @param errorMessage 开始时清空，失败时接收首次失败的原因。
/// @return 所有数据分片及 manifest 均成功替换时为 true。
/// @warning 同步写盘与序列化路径，不在逻辑 update 或渲染回调执行。
/// 保存不删除旧单文件；迁移调用者须在确认成功后单独执行清理。
bool ProjectStorage::save(const Project&               project,
                          const std::filesystem::path& projectRoot,
                          std::string&                 errorMessage) const
{
    errorMessage.clear();
    // 清掉调用者上一次失败留下的诊断，成功返回时不会携带陈旧错误。
    const auto      directory = storageDirectory(projectRoot);
    std::error_code createDirectoryError;
    std::filesystem::create_directories(directory, createDirectoryError);
    // 目录准备失败时立即退出，不进入分片生成或覆盖流程。
    if ( createDirectoryError ) {
        errorMessage = "无法创建项目配置目录：" +
                       Config::pathToUtf8(directory) + "，错误：" +
                       createDirectoryError.message();
        return false;
    }

    nlohmann::json settingsJson = project.m_settings;
    // 先按领域类型序列化，再剥离另存字段，未拆分的新增设置自然保留。
    settingsJson.erase("m_workspace");
    nlohmann::json workspaceJson = project.m_settings.m_workspace;
    // 工具状态只保留在独立分片中，避免两份值在手工修改后出现冲突。
    workspaceJson.erase("m_projectAudioToolSelectedResourceId");
    workspaceJson.erase("m_projectAudioToolBrushVolume");
    workspaceJson.erase("m_projectAudioToolPreviewEffectOnSelection");
    workspaceJson.erase("m_projectAudioToolOpen");
    workspaceJson.erase("m_projectAudioToolPlacements");
    // erase 只影响临时 JSON，不修改传入 Project 中正在使用的工作区配置。

    const auto&          audioTool = project.m_settings.m_workspace;
    const nlohmann::json manifestJson{
        // 入口仅声明布局协议，分片路径由固定常量决定。
        { "format_version", SPLIT_STORAGE_FORMAT_VERSION },
        { "storage", "split" },
    };
    const nlohmann::json projectJson{
        // 资源排除列表必须随项目持久化，不能从当前扫描结果重新推断。
        { "m_metadata", project.m_metadata },
        { "m_excludedBeatmapPaths", project.m_excludedBeatmapPaths },
        { "m_excludedAudioPaths", project.m_excludedAudioPaths },
    };
    const nlohmann::json audioResourcesJson{
        // 集合仍使用旧 Project 字段名，读取端可以直接还原序列化结构。
        { "m_audioResources", project.m_audioResources },
    };
    const nlohmann::json beatmapsJson{
        // 保持谱面入口的领域顺序，不按磁盘遍历次序重新排序。
        { "m_beatmaps", project.m_beatmaps },
    };
    const nlohmann::json draftLanesJson{
        // 新保存总是写出草稿轨分片，空组也是明确的项目状态。
        { "m_draftLaneGroups", project.m_draftLaneGroups },
    };
    const nlohmann::json audioToolJson{
        // 与 assembleProjectJson 对应，专有工作区字段在这里集中拆出。
        { "m_projectAudioToolSelectedResourceId",
          audioTool.m_projectAudioToolSelectedResourceId },
        { "m_projectAudioToolBrushVolume",
          audioTool.m_projectAudioToolBrushVolume },
        { "m_projectAudioToolPreviewEffectOnSelection",
          audioTool.m_projectAudioToolPreviewEffectOnSelection },
        { "m_projectAudioToolOpen", audioTool.m_projectAudioToolOpen },
        { "m_projectAudioToolPlacements",
          audioTool.m_projectAudioToolPlacements },
    };

    // 各分片独立替换，首个失败立即返回，已成功替换的前序分片不回滚。
    // 不在这里更新活动项目或 UI 状态，调用者根据返回值处理保存反馈。
    if ( !writeJsonAtomically(
             directory / PROJECT_FILE_NAME, projectJson, errorMessage) ||
         !writeJsonAtomically(
             directory / SETTINGS_FILE_NAME, settingsJson, errorMessage) ||
         !writeJsonAtomically(directory / AUDIO_RESOURCES_FILE_NAME,
                              audioResourcesJson,
                              errorMessage) ||
         !writeJsonAtomically(
             directory / BEATMAPS_FILE_NAME, beatmapsJson, errorMessage) ||
         !writeJsonAtomically(
             directory / DRAFT_LANES_FILE_NAME, draftLanesJson, errorMessage) ||
         !writeJsonAtomically(
             directory / WORKSPACE_FILE_NAME, workspaceJson, errorMessage) ||
         !writeJsonAtomically(
             directory / AUDIO_TOOL_FILE_NAME, audioToolJson, errorMessage) ) {
        // 分片失败后不触碰 manifest，首次未完成保存不会建立新入口。
        return false;
    }

    // 首次保存时，入口最后出现可避免把未写齐的新目录识别成完整项目。
    // 对已有分片项目仍不是组级事务，入口不承担所有文件版本一致性保证。
    return writeJsonAtomically(
        directory / MANIFEST_FILE_NAME, manifestJson, errorMessage);
}

/// @brief 清除调用方确认迁移完成的旧配置入口。
/// @param projectRoot 只删除此目录中的 mmm_project.json。
/// @param errorMessage 开始时清空，存在性检查或删除失败时写入原因。
/// @return 原文件已不存在或本次移除成功时为 true。
/// @pre 调用者已确认新布局保存成功；本函数不重新检查分片内容。
/// 删除与保存分成两个接口，使读取旧项目本身不会产生不可逆的磁盘变化。
/// @warning 低频删除操作，不递归清理目录、不删除媒体或谱面资源。
bool ProjectStorage::removeLegacyProjectFile(
    const std::filesystem::path& projectRoot, std::string& errorMessage) const
{
    errorMessage.clear();
    const auto      legacyPath = legacyProjectFilePath(projectRoot);
    std::error_code filesystemError;
    const bool exists = std::filesystem::exists(legacyPath, filesystemError) &&
                        !filesystemError;
    // 目标固定为旧配置入口，不根据配置内容或资源列表扩展删除范围。
    if ( filesystemError ) {
        // 查询失败不能解释成文件不存在，否则会误报迁移清理成功。
        errorMessage = "无法检查旧项目配置：" + filesystemError.message();
        return false;
    }
    // 允许成功迁移后重复调用，已清理的项目无需再报告错误。
    if ( !exists ) return true;

    if ( !std::filesystem::remove(legacyPath, filesystemError) ||
         filesystemError ) {
        // 删除未发生也算失败，保留路径便于上层提示用户处理权限或状态变化。
        errorMessage = "无法移除旧项目配置：" + Config::pathToUtf8(legacyPath);
        if ( filesystemError ) {
            // 仅在系统给出具体错误时追加原因，路径信息始终保留。
            errorMessage += "，错误：" + filesystemError.message();
        }
        return false;
    }
    return true;
}

}  // namespace MMM::Logic
