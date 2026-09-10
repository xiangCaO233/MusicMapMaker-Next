#include "logic/ProjectCommandService.h"
#include "logic/ProjectResourceService.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace
{

/// @brief 为单次测试创建并清理隔离的项目目录。
/// @note 文件只写入系统临时目录，不污染源码测试资源或用户项目。
/// @note 目录创建失败通过空 path 表示，测试必须在写文件前检查。
/// @note 对象独占目录清理责任，禁止复制导致多个析构操作删除同一目录。
class ScopedTestProjectDirectory
{
public:
    /// @brief 创建唯一测试目录。
    ScopedTestProjectDirectory()
    {
        std::error_code filesystemError;
        const auto      baseDirectory =
            std::filesystem::temp_directory_path(filesystemError);
        if ( filesystemError ) return;

        // 单调时钟计数用于区分测试作用域生成的目录名。
        // 目录本身仍由文件系统创建结果确认，名称生成不等于创建成功。
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = baseDirectory /
                 ("mmm-project-audio-reference-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, filesystemError);
        // 失败时清空所有权路径，让调用方以统一空值结束准备。
        // 避免在部分准备失败后把未确认归属的路径当作有效测试项目。
        if ( filesystemError ) m_path.clear();
    }

    /// @brief 清理本测试创建的隔离目录。
    ~ScopedTestProjectDirectory()
    {
        // 只有成功记录的测试目录才进入递归清理。
        // 析构用 error_code 接收失败，不能在退出测试作用域时抛出异常。
        if ( m_path.empty() ) return;
        std::error_code filesystemError;
        std::filesystem::remove_all(m_path, filesystemError);
    }

    // 清理责任不能复制，路径观察引用仅在当前对象存活时使用。
    ScopedTestProjectDirectory(const ScopedTestProjectDirectory&) = delete;
    ScopedTestProjectDirectory& operator=(const ScopedTestProjectDirectory&) =
        delete;

    /// @brief 获取测试项目根目录。
    /// @return 测试目录路径。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 本测试拥有的隔离项目目录。
    std::filesystem::path m_path;
};

/// @brief 在单个测试作用域内覆盖并恢复默认 Creator。
/// @note 覆盖进程内配置单例，测试之间必须串行使用该夹具。
/// @note 提前返回也会析构恢复原值，避免某个用例失败污染后续用例。
class ScopedDefaultCreator
{
public:
    /// @brief 写入测试 Creator 并保留原始设置。
    /// @param creator 测试期间使用的默认 Creator。
    explicit ScopedDefaultCreator(std::string creator)
        : m_original(MMM::Config::AppConfig::instance()
                         .getEditorSettings()
                         .defaultCreator)
    {
        // 原值已在成员初始化中按值保存，再安装当前用例的默认作者。
        // 这里不执行配置持久化，作用仅限测试进程。
        MMM::Config::AppConfig::instance().getEditorSettings().defaultCreator =
            std::move(creator);
    }

    /// @brief 恢复测试前的默认 Creator。
    ~ScopedDefaultCreator()
    {
        // 恢复内容而非固定写空，兼容启动测试前已有的用户默认作者。
        // 移动保存值即可释放临时字符串，不依赖测试是否正常走到末尾。
        MMM::Config::AppConfig::instance().getEditorSettings().defaultCreator =
            std::move(m_original);
    }

    ScopedDefaultCreator(const ScopedDefaultCreator&)            = delete;
    ScopedDefaultCreator& operator=(const ScopedDefaultCreator&) = delete;

private:
    /// @brief 测试前的默认 Creator。
    std::string m_original;
};

/// @brief 创建目录扫描所需的最小音频占位文件。
/// @param path 待创建文件路径。
/// @param byteCount 需要写入的占位字节数。
/// @return 文件成功创建时返回 true。
/// @note 占位内容不是可解码音频，只用于目录存在性、扩展名和文件大小相关测试。
/// @note byteCount 可显式改变候选大小，不用于模拟音频时长。
bool createAudioPlaceholder(const std::filesystem::path& path,
                            std::size_t                  byteCount = 1U)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    // 按二进制模式写入零字节，避免平台文本换行转换改变文件大小。
    // 流状态反映打开或写入失败，测试不会把创建失败当作业务分类错误。
    std::ofstream stream(path, std::ios::binary);
    for ( std::size_t index = 0; index < byteCount; ++index ) {
        stream.put('\0');
    }
    return stream.good();
}

/// @brief 读取测试文本文件并保留全部原始内容。
/// @param path 待读取文件。
/// @param output 文件内容。
/// @return 完整读取成功时返回 true。
/// @note 原始字节回读用于断言受保护操作没有改写谱面，不做换行规范化。
/// @note 打开失败时 output 保留原值，调用者必须先检查返回值。
bool readTextFile(const std::filesystem::path& path, std::string& output)
{
    std::ifstream stream(path, std::ios::binary);
    if ( !stream.is_open() ) return false;
    // 一次读取完整文件，保留空文件和任意换行格式。
    // 正常读到 EOF 属于成功，不能只以 good 判断完整读取。
    output.assign(std::istreambuf_iterator<char>(stream),
                  std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

/// @brief 按 ID 查找项目音频资源。
/// @param project 待查询项目。
/// @param id 资源 ID。
/// @return 找到时返回资源地址，否则返回空。
/// @note 按稳定 ID 精确匹配，不替代被测服务的路径兼容解析。
/// @note 返回项目资源向量元素的观察指针，修改或扩容向量后须重新查询。
const MMM::AudioResource* findResource(const MMM::Project& project,
                                       const std::string&  id)
{
    // 夹具查询只用于定位断言对象，避免复用被测路径解析实现掩盖同一错误。
    // 重复 ID 时沿用向量首项，测试输入应明确是否要覆盖冲突场景。
    const auto iterator = std::find_if(project.m_audioResources.begin(),
                                       project.m_audioResources.end(),
                                       [&](const MMM::AudioResource& resource) {
                                           return resource.m_id == id;
                                       });
    return iterator == project.m_audioResources.end() ? nullptr : &*iterator;
}

/// @brief 保存带歌曲提示、Note 绑定和自动采样的测试谱面。
/// @param path 谱面保存路径。
/// @param songHint 歌曲文件提示。
/// @param noteAudioRef 可选 Note 绑定资源。
/// @param sampleAudioRef 可选自动采样资源。
/// @return 保存成功时返回 true。
/// @note 引用字符串原样保存，用于覆盖稳定 ID、项目路径和旧版文件名形式。
/// @note 音频占位是否有效不参与谱面保存，此助手不加载或播放声音。
bool saveReferenceBeatmap(const std::filesystem::path& path,
                          const std::filesystem::path& songHint,
                          const std::string&           noteAudioRef,
                          const std::string&           sampleAudioRef)
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name    = "Reference";
    beatmap.m_baseMapMetadata.version = "Reference";
    // 固定四条玩家轨和一条 BGM 轨，使采样轨号四具有明确领域语义。
    // 用例改变资源身份时无需同时改变轨道配置。
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_baseMapMetadata.song_file_hint  = songHint;

    // 空引用表示不创建绑定音符，避免误把空绑定统计成资源使用。
    // 非空绑定保存独立实例音量，后续变更测试可以检查引用与参数是否保留。
    if ( !noteAudioRef.empty() ) {
        MMM::Note note;
        note.setSampleBinding(MMM::AudioSampleBinding{ noteAudioRef, 0.75F });
        beatmap.m_noteData.notes.push_back(std::move(note));
    }
    // 自动采样与 Note 绑定分别建立，允许同一资源以不同引用类别出现。
    // BGM 轨字段使用统一编号，不是从零开始的局部 BGM 索引。
    if ( !sampleAudioRef.empty() ) {
        MMM::AudioSampleEvent sample;
        sample.m_track           = 4;
        sample.m_audioResourceId = sampleAudioRef;
        beatmap.m_audioSamples.push_back(std::move(sample));
    }
    // 保存前同步谱面内部派生状态，使被测扫描读取正式文件格式。
    // 返回保存结果，把夹具构造失败与资源服务断言失败区分开。
    beatmap.sync();
    return beatmap.saveToFile(path);
}

/// @brief 验证旧单主音轨字段只读兼容且当前项目不再写出该字段。
/// @return 兼容行为正确时返回 true。
/// @note 单独验证序列化迁移方向：旧字段可读，当前格式不继续写出。
/// @note 该用例只操作内存 JSON，不需要真实音频或项目目录。
/// @note 旧字段值必须原样读入，兼容读取不能只接受键却丢弃内容。
/// @note 输出检查以键是否存在为准，空旧值也不应继续占据当前格式。
bool testLegacyBeatmapEntryIsReadOnly()
{
    // 构造带旧单主音轨字段的输入，证明读取兼容仍保留原始引用。
    // 若直接构造当前类型再回读，会漏测旧字段反序列化入口。
    const nlohmann::json legacyJson{ { "m_name", "Legacy" },
                                     { "m_filePath", "Legacy.mmm" },
                                     { "m_audioTrackId", "legacy.ogg" } };
    const auto           entry = legacyJson.get<MMM::Project::BeatmapEntry>();
    if ( entry.m_audioTrackId != "legacy.ogg" ) {
        XERROR("Legacy BeatmapEntry audio track ID was not readable");
        return false;
    }

    // 把已读入的旧值重新序列化，验证输出策略不依赖字段是否为空。
    // 检查键不存在，而不是只检查值被改为空字符串。
    const nlohmann::json currentJson = entry;
    if ( currentJson.contains("m_audioTrackId") ) {
        XERROR("Current BeatmapEntry still persisted m_audioTrackId");
        return false;
    }
    return true;
}

/// @brief 验证目录扫描优先使用谱面引用并为无引用项目推断主音轨。
/// @return 扫描分类行为正确时返回 true。
/// @note 同时覆盖 Note 绑定、歌曲提示、自动采样和未引用文件四类输入。
/// @note 扫描结果由夹具显式提供，验证资源分类而非目录遍历器本身。
/// @note 冲突引用通过同一资源的歌曲提示和 Note 绑定共同构造，确保 Effect
/// 约束优先。
/// @note 无引用回退使用独立项目实例，避免已有 Main 改变后续候选选择。
/// @note 文件大小不同的候选都必须保留，回退分类不是资源过滤操作。
bool testReferenceAwareDirectoryScan()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto sharedAudio = directory.path() / "audio" / "shared.wav";
    const auto mainAudio   = directory.path() / "audio" / "main.ogg";
    const auto sampleAudio = directory.path() / "audio" / "sample.wav";
    const auto unusedAudio = directory.path() / "audio" / "unused.wav";
    if ( !createAudioPlaceholder(sharedAudio) ||
         !createAudioPlaceholder(mainAudio) ||
         !createAudioPlaceholder(sampleAudio) ||
         !createAudioPlaceholder(unusedAudio) ) {
        XERROR("Failed to create project audio placeholders");
        return false;
    }

    // shared 同时被歌曲提示和 Note 绑定引用，用来确定冲突分类优先级。
    // main 仅作为歌曲提示；sample 仅作为自动采样；unused 没有谱面引用。
    const auto conflictMap = directory.path() / "Conflict.mmm";
    const auto mainMap     = directory.path() / "Main.mmm";
    if ( !saveReferenceBeatmap(
             conflictMap, "audio/shared.wav", "shared.wav", "sample.wav") ||
         !saveReferenceBeatmap(mainMap, "audio/main.ogg", "", "") ) {
        XERROR("Failed to save reference-aware scan beatmaps");
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    // 显式设置成功扫描及确定的文件列表，避免依赖真实目录枚举顺序。
    // buildInitialResources 仍从落盘谱面读取引用，不直接注入推断结果。
    MMM::Logic::ProjectDirectoryScanner::ScanResult scanResult;
    scanResult.m_success      = true;
    scanResult.m_beatmapFiles = { conflictMap, mainMap };
    scanResult.m_audioFiles   = {
        sharedAudio, mainAudio, sampleAudio, unusedAudio
    };
    MMM::Logic::ProjectResourceService{}.buildInitialResources(project,
                                                               scanResult);

    const auto* shared = findResource(project, "shared.wav");
    const auto* main   = findResource(project, "main.ogg");
    const auto* sample = findResource(project, "sample.wav");
    const auto* unused = findResource(project, "unused.wav");
    // 先确认四个资源都被创建，再检查每类引用产生的 Main 或 Effect。
    // shared 应被 Note 绑定归为 Effect，不能被歌曲提示强行推成 Main。
    if ( !shared || !main || !sample || !unused ||
         shared->m_type != MMM::AudioTrackType::Effect ||
         main->m_type != MMM::AudioTrackType::Main ||
         sample->m_type != MMM::AudioTrackType::Effect ||
         unused->m_type != MMM::AudioTrackType::Effect ) {
        XERROR("Reference-aware audio type inference was incorrect");
        return false;
    }
    // 资源分类完成后不应回填旧单主音轨字段。
    // 该断言覆盖新扫描写入路径，补充前一用例仅验证 JSON 序列化的边界。
    if ( std::any_of(project.m_beatmaps.begin(),
                     project.m_beatmaps.end(),
                     [](const MMM::Project::BeatmapEntry& entry) {
                         return !entry.m_audioTrackId.empty();
                     }) ) {
        XERROR("Directory scan still authored legacy m_audioTrackId values");
        return false;
    }

    // 隔离一个完全没有谱面引用的项目，验证单候选主音轨回退。
    // 沿用同一占位文件不代表沿用前一个项目的资源表或分类状态。
    MMM::Project noMainProject;
    noMainProject.m_projectRoot = directory.path();
    MMM::Logic::ProjectDirectoryScanner::ScanResult noMainScan;
    noMainScan.m_success    = true;
    noMainScan.m_audioFiles = { unusedAudio };
    MMM::Logic::ProjectResourceService{}.buildInitialResources(noMainProject,
                                                               noMainScan);
    if ( noMainProject.m_audioResources.size() != 1U ||
         noMainProject.m_audioResources.front().m_type !=
             MMM::AudioTrackType::Main ) {
        XERROR("Single unreferenced audio was not selected as Main");
        return false;
    }

    // 用相同扩展名且不同字节数排除扩展名优先级的干扰。
    // 候选大小可确定比较结果，不依赖真实音频时长或解码器。
    const auto smallerAudio = directory.path() / "fallback" / "smaller.ogg";
    const auto largerAudio  = directory.path() / "fallback" / "larger.ogg";
    if ( !createAudioPlaceholder(smallerAudio, 8U) ||
         !createAudioPlaceholder(largerAudio, 64U) ) {
        XERROR("Failed to create fallback Main audio candidates");
        return false;
    }

    // 多个无引用候选单独建立项目，验证选择最大文件作为 Main。
    // 较小文件必须仍保留为 Effect，不能因未获选而被丢弃。
    MMM::Project fallbackProject;
    fallbackProject.m_projectRoot = directory.path();
    MMM::Logic::ProjectDirectoryScanner::ScanResult fallbackScan;
    fallbackScan.m_success    = true;
    fallbackScan.m_audioFiles = { smallerAudio, largerAudio };
    MMM::Logic::ProjectResourceService{}.buildInitialResources(fallbackProject,
                                                               fallbackScan);

    const auto* smaller = findResource(fallbackProject, "smaller.ogg");
    const auto* larger  = findResource(fallbackProject, "larger.ogg");
    if ( !smaller || !larger ||
         smaller->m_type != MMM::AudioTrackType::Effect ||
         larger->m_type != MMM::AudioTrackType::Main ) {
        XERROR("Largest unreferenced audio was not selected as Main");
        return false;
    }
    return true;
}

/// @brief 验证单项和批量资源解析均保留跨匹配方式的首项语义。
/// @return 路径匹配前项优先于后续精确 ID 项时返回 true。
/// @note 单项与批量解析均按资源表顺序保留第一个跨匹配模式命中。
/// @note 仅使用内存路径，项目根不需要实际存在；此用例不检查文件访问。
/// @note 后项的精确 ID
/// 与前项的项目相对路径相同，匹配类别不能改变资源顺序优先级。
/// @note 批量结果保留输入引用数量和顺序，相同资源命中也不能合并输出项。
bool testAudioResolutionPreservesCrossModeFirstMatch()
{
    MMM::Project project;
    project.m_projectRoot = "/tmp/mmm-audio-resolution-order";
    // 前项通过路径命中，后项通过精确 ID 命中相同引用。
    // 预期仍返回前项，用来防止索引优化把精确 ID 固定提升到最高优先级。
    project.m_audioResources = {
        MMM::AudioResource{
            .m_id   = "foo.wav",
            .m_path = "audio/foo.wav",
            .m_type = MMM::AudioTrackType::Effect,
        },
        MMM::AudioResource{
            .m_id   = "audio/foo.wav",
            .m_path = "audio/exact-id.wav",
            .m_type = MMM::AudioTrackType::Main,
        },
    };

    const auto* single =
        MMM::Logic::ProjectResourceService::findAudioResourceForReference(
            project, "charts/test.mmm", "audio/foo.wav");
    // 同时使用项目相对与谱面相对形式，二者应归到同一个首项。
    // 字符串视图借用字面量，批量服务不需要取得引用字符串所有权。
    const std::vector<std::string_view> references{
        "audio/foo.wav",
        "../audio/foo.wav",
    };
    const auto batch =
        MMM::Logic::ProjectResourceService::resolveAudioResourceReferences(
            project, "charts/test.mmm", references);
    // 比较对象地址而非只比较 ID，确保命中确实来自资源表前项。
    // 先检查批量结果长度再访问下标，避免失败时断言自身越界。
    if ( single != &project.m_audioResources.front() || batch.size() != 2U ||
         batch[0] != &project.m_audioResources.front() ||
         batch[1] != &project.m_audioResources.front() ) {
        XERROR("Audio resolution changed cross-mode first-match semantics");
        return false;
    }
    return true;
}

/// @brief 验证项目根谱面的越根旧路径仍回退到资源文件名。
/// @return 自定义 ID 资源能够通过旧版 map-relative filename 回退解析。
/// @note 旧路径越过项目根时仍可按文件名兼容命中项目内资源。
/// @note 自定义资源 ID 与文件名不同，避免精确 ID 匹配掩盖路径回退。
/// @note 回退匹配仍指向项目资源表中的对象，不是把项目外路径注册为新资源。
/// @note 扫描同步检验 Note 引用能够参与类型索引，补充查询返回地址的单项断言。
bool testRootBeatmapEscapedLegacyReferenceFallback()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    // 谱面放在项目根，../foo.wav 按纯相对路径解释会落到项目外。
    // 项目内实际文件仍为 foo.wav，专门验证旧版文件名兼容回退。
    const auto mapPath   = directory.path() / "chart.mmm";
    const auto audioPath = directory.path() / "foo.wav";
    if ( !createAudioPlaceholder(audioPath) ||
         !saveReferenceBeatmap(mapPath, {}, "../foo.wav", "") ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "chart", "chart.mmm", {} });
    // 初始故意设为 Main，后续 Note 引用索引应把它恢复成 Effect。
    // 解析成功与同步分类都要成立，不能只在单项查询入口支持旧路径。
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "custom-resource-id",
        .m_path = "foo.wav",
        .m_type = MMM::AudioTrackType::Main,
    });

    // 引用类别明确为 Note 绑定，资源类型推断必须尊重该语义。
    // 同一引用分别进入匹配谓词、单项查询和批量查询，检查三个入口的一致性。
    const MMM::Logic::BeatmapAudioReference escapedReference{
        "chart.mmm",
        "../foo.wav",
        MMM::Logic::BeatmapAudioReferenceKind::NoteSampleBinding,
    };
    const auto* single =
        MMM::Logic::ProjectResourceService::findAudioResourceForReference(
            project, "chart.mmm", "../foo.wav");
    // 单元素批量请求与单项查询并列，要求兼容回退在两种入口均可用。
    // 返回地址必须属于既有资源表，不允许批量入口临时生成替代对象。
    const std::vector<std::string_view> references{ "../foo.wav" };
    const auto                          batch =
        MMM::Logic::ProjectResourceService::resolveAudioResourceReferences(
            project, "chart.mmm", references);
    if ( !MMM::Logic::ProjectResourceService::audioReferenceMatchesResource(
             project, escapedReference, project.m_audioResources.front()) ||
         single != &project.m_audioResources.front() || batch.size() != 1U ||
         batch.front() != &project.m_audioResources.front() ) {
        XERROR("Root beatmap escaped legacy reference fallback was lost");
        return false;
    }

    MMM::Logic::ProjectDirectoryScanner::ScanResult scanResult;
    scanResult.m_success      = true;
    scanResult.m_beatmapFiles = { mapPath };
    scanResult.m_audioFiles   = { audioPath };
    // 同步从实际保存的谱面读取旧路径，检验引用分类索引也实现相同回退。
    // 资源数量未增加，关注既有资源类型更新及 changed 状态。
    const auto syncResult =
        MMM::Logic::ProjectResourceService{}.syncDirectoryResources(project,
                                                                    scanResult);
    if ( !syncResult.m_changed || project.m_audioResources.front().m_type !=
                                      MMM::AudioTrackType::Effect ) {
        XERROR("Escaped Note reference was absent from the type index");
        return false;
    }
    return true;
}

/// @brief 验证大批量资源仅通过一次性引用索引恢复 Note 绑定类型。
///
/// 该用例同时覆盖稳定 ID、项目相对路径、谱面相对路径、Windows 分隔符
/// 和旧版 basename。资源与引用数量相同，可发现批量引用遗漏；
/// 本用例没有调用计数或耗时门槛，不能独立证明索引实现的复杂度。
/// @return 全部资源均按对应 Note 引用恢复为 Effect 时返回 true。
/// @note 使用不同文件名避免批量测试退化为重复解析同一个资源。
/// @note 初始全部标为 Main，最终应由对应 Note 引用统一纠正为 Effect。
/// @note 每个文件有唯一编号，使失败日志能够定位具体引用形式。
/// @note 数量断言同时覆盖重分类任务和最终资源表，防止只改类型却遗漏注册请求。
bool testBulkReferenceIndexPreservesCompatibility()
{
    // 数量足以让五种引用形式重复交错，覆盖批量结果的完整性。
    // 该常量不是性能通过阈值，用例没有计时断言。
    constexpr std::size_t RESOURCE_COUNT = 256U;

    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto      mapPath = directory.path() / "maps" / "BulkReferences.mmm";
    std::error_code filesystemError;
    std::filesystem::create_directories(mapPath.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name            = "BulkReferences";
    beatmap.m_baseMapMetadata.version         = "BulkReferences";
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;

    MMM::Project scannedProject;
    scannedProject.m_projectRoot = directory.path();
    scannedProject.m_beatmaps.push_back(MMM::Project::BeatmapEntry{
        "BulkReferences",
        "maps/BulkReferences.mmm",
        {},
    });
    MMM::Logic::ProjectDirectoryScanner::ScanResult scanResult;
    scanResult.m_success = true;
    scanResult.m_beatmapFiles.push_back(mapPath);
    scanResult.m_audioFiles.reserve(RESOURCE_COUNT);

    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto filename     = "sample-" + std::to_string(index) + ".wav";
        const auto relativePath = "audio/" + filename;
        const auto audioPath    = directory.path() / relativePath;
        if ( !createAudioPlaceholder(audioPath) ) {
            return false;
        }
        scanResult.m_audioFiles.push_back(audioPath);

        // 旧 basename 组沿用文件名 ID，其他组故意使用与路径不同的稳定 ID。
        // 防止所有路径分支被精确 ID 匹配偶然覆盖。
        const bool useLegacyBasename = index % 5U == 3U;
        const auto resourceId = useLegacyBasename
                                    ? filename
                                    : "stable-sample-" + std::to_string(index);

        std::string reference;
        // 五组分别覆盖稳定 ID、项目相对、谱面相对、旧目录文件名和反斜杠路径。
        // 引用原样写进谱面，兼容转换必须由被测服务执行。
        switch ( index % 5U ) {
        case 0U: reference = resourceId; break;
        case 1U: reference = relativePath; break;
        case 2U: reference = "../audio/" + filename; break;
        case 3U: reference = "legacy/draft/" + filename; break;
        default: reference = "audio\\" + filename; break;
        }

        MMM::Note note;
        note.setSampleBinding(
            MMM::AudioSampleBinding{ std::move(reference), 1.0F });
        beatmap.m_noteData.notes.push_back(std::move(note));

        // 资源表与 Note 引用按同一索引对应，但初始类型故意错误。
        // 同步应复用身份并纠正类型，而不是再创建一批不同资源。
        scannedProject.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = resourceId,
            .m_path = relativePath,
            .m_type = MMM::AudioTrackType::Main,
        });
    }

    beatmap.sync();
    if ( !beatmap.saveToFile(mapPath) ) return false;

    const auto result =
        MMM::Logic::ProjectResourceService{}.syncDirectoryResources(
            scannedProject, scanResult);
    // 同时检查变化标志、需要注册的 Effect 数量和最终资源数量。
    // 仅检查某一个资源类型会漏掉批量索引丢项或重复创建。
    if ( !result.m_changed ||
         result.m_effectResourcesToRegister.size() != RESOURCE_COUNT ||
         scannedProject.m_audioResources.size() != RESOURCE_COUNT ) {
        return false;
    }
    // 对最终资源逐项验证，首个未纠正项的 ID 用于定位哪种兼容形式失败。
    // 结果不依赖资源表是否保留与原引用相同的排列顺序。
    const auto invalidResource =
        std::find_if(scannedProject.m_audioResources.begin(),
                     scannedProject.m_audioResources.end(),
                     [](const MMM::AudioResource& resource) {
                         return resource.m_type != MMM::AudioTrackType::Effect;
                     });
    if ( invalidResource != scannedProject.m_audioResources.end() ) {
        XERROR("Bulk audio reference index missed resource: {}",
               invalidResource->m_id);
        return false;
    }
    return true;
}

/// @brief 验证大批量目录同步通过一次性路径索引复用已有资源。
///
/// 输入混合当前相对路径、旧项目目录前缀、绝对路径和词法冗余路径，
/// 检查批量资源的归一化与身份保留；本用例不单独证明运行复杂度。
/// @return 全部资源配置和稳定 ID 均被复用且同步无需写回时返回 true。
/// @note 各资源保留不同实例配置值，用于发现匹配后重建默认配置的错误。
/// @note 同步无变化既要不改资源数量，也要不产生额外 Effect 注册任务。
/// @note 既有存储路径可以保留旧形式，匹配归一化不要求自动写回规范路径。
/// @note 不同音量为各资源提供配置指纹，稳定 ID 正确也不能掩盖配置串配。
bool testBulkDirectorySyncReusesNormalizedResources()
{
    constexpr std::size_t RESOURCE_COUNT = 256U;

    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    MMM::Logic::ProjectDirectoryScanner::ScanResult scanResult;
    scanResult.m_success = true;
    scanResult.m_audioFiles.reserve(RESOURCE_COUNT);
    project.m_audioResources.reserve(RESOURCE_COUNT);

    // 旧路径可能包含项目目录名这一额外前缀，当前资源表需兼容去重。
    // 使用实际临时目录末级名称，避免硬编码目录名与根路径不一致。
    const auto projectFolder =
        MMM::Config::pathToUtf8(directory.path().filename());
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto filename  = "sync-" + std::to_string(index) + ".wav";
        const auto audioPath = directory.path() / "audio" / filename;
        if ( !createAudioPlaceholder(audioPath) ) return false;
        scanResult.m_audioFiles.push_back(audioPath);

        std::string storedPath;
        // 同一个磁盘文件分别用当前相对、旧前缀、绝对和含 .. 的词法路径表示。
        // 这些表示应匹配已有资源，而不是被当成四种新文件身份。
        switch ( index % 4U ) {
        case 0U: storedPath = "audio/" + filename; break;
        case 1U: storedPath = projectFolder + "/audio/" + filename; break;
        case 2U: storedPath = MMM::Config::pathToUtf8(audioPath); break;
        default: storedPath = "audio/drafts/../" + filename; break;
        }

        MMM::AudioTrackConfig config;
        // 每项音量编码自己的索引，既检查保留配置也能发现资源顺序错配。
        // 期望值在断言处独立重算，不从可能已经被修改的配置字段生成。
        config.volume = static_cast<float>(index + 1U) /
                        static_cast<float>(RESOURCE_COUNT + 1U);
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id     = "stable-sync-" + std::to_string(index),
            .m_path   = std::move(storedPath),
            .m_type   = MMM::AudioTrackType::Effect,
            .m_config = std::move(config),
        });
    }

    const auto result =
        MMM::Logic::ProjectResourceService{}.syncDirectoryResources(project,
                                                                    scanResult);
    // 路径兼容命中已有资源应保持只读，不把归一化查询变成自动写回。
    // Effect 注册队列也应为空，避免无变化同步重新注册整批音频。
    if ( result.m_changed || !result.m_effectResourcesToRegister.empty() ||
         project.m_audioResources.size() != RESOURCE_COUNT ) {
        XERROR("Bulk directory sync recreated existing audio resources");
        return false;
    }

    // 逐位置检查稳定 ID 和对应音量，覆盖身份及用户配置的联合保留。
    // 数量相同不能证明资源没有被重建或互相串配。
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto& resource       = project.m_audioResources[index];
        const auto  expectedVolume = static_cast<float>(index + 1U) /
                                     static_cast<float>(RESOURCE_COUNT + 1U);
        // ID 与期望音量共同按同一索引校验，识别内容相同数量下的错位复用。
        // 音量计算在夹具与断言使用相同有界表达式，避免引入不相关舍入差异。
        if ( resource.m_id != "stable-sync-" + std::to_string(index) ||
             resource.m_config.volume != expectedVolume ) {
            XERROR("Bulk directory sync lost resource configuration at {}",
                   index);
            return false;
        }
    }
    return true;
}

/// @brief 验证资源改类型和删除时不会破坏谱面引用。
/// @return 引用约束正确时返回 true。
/// @note 类型修改和删除使用不同保护规则，不能把所有引用类别一律视为阻塞。
/// @note 通过真实保存的谱面建立磁盘引用，未向命令直接注入参考答案。
/// @note Note 绑定限制 Effect 转 Main，自动采样仅在资源删除时构成阻塞。
/// @note 歌曲提示不被当作强依赖，仍可允许类型变更和资源表移除。
/// @note 该用例不启用源文件删除，物理文件处理由独立用例覆盖。
bool testAudioReferenceMutationGuards()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto sharedAudio = directory.path() / "shared.wav";
    const auto sampleAudio = directory.path() / "sample.wav";
    const auto hintAudio   = directory.path() / "hint.wav";
    const auto unusedAudio = directory.path() / "unused.wav";
    if ( !createAudioPlaceholder(sharedAudio) ||
         !createAudioPlaceholder(sampleAudio) ||
         !createAudioPlaceholder(hintAudio) ||
         !createAudioPlaceholder(unusedAudio) ) {
        return false;
    }

    // 同一谱面分别提供歌曲提示、Note 绑定和自动采样引用。
    // 四个初始 Effect 资源中还有一个未引用对照，用于区分保护拒绝与普遍失败。
    const auto mapPath = directory.path() / "References.mmm";
    if ( !saveReferenceBeatmap(
             mapPath, "hint.wav", "shared.wav", "sample.wav") ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "References", "References.mmm", {} });
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "shared.wav",
                            .m_path = "shared.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "sample.wav",
                            .m_path = "sample.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "hint.wav",
                            .m_path = "hint.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "unused.wav",
                            .m_path = "unused.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    // 首先尝试把 Note 绑定依赖的 Effect 改为 Main，预期引用约束拒绝。
    // 随后继续使用同一个项目测试其他引用类别，验证失败没有污染资源表。
    MMM::Logic::ProjectCommandService service;
    auto                              updateResult =
        service.updateAudioResource(project,
                                    MMM::Logic::CmdUpdateAudioResource{
                                        "shared.wav",
                                        MMM::AudioTrackType::Main,
                                    });
    // 同时检查拒绝状态、具体阻塞谱面路径以及资源仍为 Effect。
    // 只有错误提示而实际已改变资源类型，也必须判定失败。
    if ( updateResult.m_updated ||
         updateResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "References.mmm" } ||
         findResource(project, "shared.wav")->m_type !=
             MMM::AudioTrackType::Effect ) {
        XERROR("Effect-to-Main Note reference guard failed");
        return false;
    }

    updateResult =
        service.updateAudioResource(project,
                                    MMM::Logic::CmdUpdateAudioResource{
                                        "sample.wav",
                                        MMM::AudioTrackType::Main,
                                    });
    if ( !updateResult.m_updated ||
         findResource(project, "sample.wav")->m_type !=
             MMM::AudioTrackType::Main ) {
        XERROR("Automatic sample incorrectly blocked Effect-to-Main update");
        return false;
    }

    updateResult =
        service.updateAudioResource(project,
                                    MMM::Logic::CmdUpdateAudioResource{
                                        "hint.wav",
                                        MMM::AudioTrackType::Main,
                                    });
    if ( !updateResult.m_updated ||
         !updateResult.m_blockingBeatmapPaths.empty() ) {
        XERROR("song_file_hint incorrectly blocked a type update");
        return false;
    }

    // Note 绑定与自动采样都是真实资源依赖，删除时都应受到保护。
    // 此前采样资源已成功改成 Main，说明删除门禁不能只针对 Effect 类型。
    const auto removeBound = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "shared.wav" });
    const auto removeSample = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "sample.wav" });
    if ( removeBound.m_removed || removeSample.m_removed ||
         removeBound.m_blockingBeatmapPaths.empty() ||
         removeSample.m_blockingBeatmapPaths.empty() ) {
        XERROR("Referenced audio resource removal was not blocked");
        return false;
    }

    // 歌曲提示属于兼容元数据，不阻止从项目资源表移除对应资源。
    // 这里检查逻辑资源移除，不要求删除磁盘源音频。
    const auto removeHint = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "hint.wav" });
    if ( !removeHint.m_removed || !removeHint.m_blockingBeatmapPaths.empty() ) {
        XERROR("song_file_hint incorrectly blocked resource removal");
        return false;
    }

    // 未引用对照必须成功移除，防止实现把所有删除请求一律拒绝。
    // 再次按稳定 ID 查询为空，验证实际资源表状态与返回标志一致。
    const auto removeUnused = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "unused.wav" });
    if ( !removeUnused.m_removed ||
         findResource(project, "unused.wav") != nullptr ) {
        XERROR("Unreferenced audio resource could not be removed");
        return false;
    }
    return true;
}

/// @brief 验证打开会话的内存谱面引用会补充磁盘扫描结果。
/// @return 未落盘的 Note 和自动采样引用都能阻止破坏性操作时返回 true。
/// @note 项目没有落盘谱面，保护只能来自显式传入的打开会话引用集合。
/// @note 阻塞路径应保留打开谱面的相对名称，供上层定位尚未保存的依赖。
/// @note 打开谱面路径只是引用来源标识，不需要对应磁盘文件。
/// @note 传入集合按引用类别保留语义，不能只按资源 ID 去重后丢失 Note 约束。
bool testOpenBeatmapReferencesSupplementDiskGuards()
{
    MMM::Project project;
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "live-note",
                            .m_path = "audio/live-note.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "live-sample",
                            .m_path = "audio/live-sample.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    // 内存谱面同时持有 Note 和采样引用，两种类别各验证一种破坏性操作。
    // 不保存该谱面，避免磁盘扫描偶然补足缺失的内存引用处理。
    MMM::BeatMap openBeatmap;
    MMM::Note    note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "live-note", 1.0F });
    openBeatmap.m_noteData.notes.push_back(std::move(note));
    openBeatmap.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_audioResourceId = "live-sample" });

    // 先通过正式收集入口提取引用，再传入命令服务。
    // 这既验证收集类别，也验证命令把打开会话引用合入磁盘结果。
    const auto openReferences =
        MMM::Logic::ProjectResourceService::collectBeatmapAudioReferences(
            openBeatmap, "charts/OpenOnly.mmm");
    MMM::Logic::ProjectCommandService service;
    const auto                        updateResult =
        service.updateAudioResource(project,
                                    MMM::Logic::CmdUpdateAudioResource{
                                        "live-note",
                                        MMM::AudioTrackType::Main,
                                    },
                                    openReferences);
    // Note 绑定禁止 Effect 转 Main，阻塞路径必须精确指向打开谱面。
    // 仅检查布尔拒绝不能发现错误来源或丢失诊断路径。
    if ( updateResult.m_updated ||
         updateResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "charts/OpenOnly.mmm" } ) {
        XERROR("Open-session Note binding did not block Effect-to-Main");
        return false;
    }

    // 自动采样允许绑定 Main，但不允许删除所引用资源。
    // 因此这里用移除命令验证第二类内存依赖，而不是重复类型修改测试。
    const auto removeResult = service.removeAudioResource(
        project,
        MMM::Logic::CmdRemoveAudioResource{ "live-sample" },
        openReferences);
    if ( removeResult.m_removed ||
         removeResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "charts/OpenOnly.mmm" } ) {
        XERROR("Open-session sample did not block resource removal");
        return false;
    }
    return true;
}

/// @brief 验证删除源音频时先执行谱面引用和文件系统安全校验。
/// @return 被引用文件保留、未引用文件原子删除且缺失文件不丢资源时返回 true。
/// @note deleteSource 标志显式启用源文件删除，磁盘只包含夹具创建的占位文件。
/// @note 成功路径验证文件与资源表共同改变，失败路径验证资源条目仍保留。
/// @note 缺失文件分支预先保留资源记录，用来区分物理删除失败与资源不存在。
/// @note 被保护文件存在性检查与资源表检查缺一不可。
bool testPhysicalAudioDeletionHonorsReferences()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto usedPath   = directory.path() / "audio" / "used.wav";
    const auto unusedPath = directory.path() / "audio" / "unused.wav";
    if ( !createAudioPlaceholder(usedPath) ||
         !createAudioPlaceholder(unusedPath) ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    // 第三项只存在资源记录而没有源文件，用于触发物理删除失败路径。
    // 它与被引用拒绝、未引用成功分别覆盖三个不同结果。
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "used-resource",
                            .m_path = "audio/used.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "unused-resource",
                            .m_path = "audio/unused.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "missing-resource",
                            .m_path = "audio/missing.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    // 通过未保存会话的采样依赖保护 used 文件。
    // 源文件真实存在，拒绝应由引用门禁而非文件缺失触发。
    const std::vector<MMM::Logic::BeatmapAudioReference> openReferences{
        MMM::Logic::BeatmapAudioReference{
            "charts/OpenOnly.mmm",
            "used-resource",
            MMM::Logic::BeatmapAudioReferenceKind::AudioSampleEvent,
        },
    };
    MMM::Logic::ProjectCommandService service;
    const auto blockedResult = service.removeAudioResource(
        project,
        MMM::Logic::CmdRemoveAudioResource{ "used-resource", true },
        openReferences);
    // 拒绝后既要保留磁盘文件，也要保留资源表记录及阻塞路径。
    // 任一层已被删除都意味着保护发生得太晚。
    if ( blockedResult.m_removed ||
         blockedResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "charts/OpenOnly.mmm" } ||
         !std::filesystem::exists(usedPath) ||
         findResource(project, "used-resource") == nullptr ) {
        XERROR("Referenced source audio was physically deleted");
        return false;
    }

    // 未引用资源启用物理删除，成功后源文件和资源表记录都应消失。
    // 还要求错误消息为空，避免成功状态与失败诊断矛盾。
    const auto removedResult = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "unused-resource", true });
    if ( !removedResult.m_removed || !removedResult.m_errorMessage.empty() ||
         std::filesystem::exists(unusedPath) ||
         findResource(project, "unused-resource") != nullptr ) {
        XERROR("Unreferenced source audio was not deleted atomically");
        return false;
    }

    // 文件缺失不能被当作成功删除并顺便丢弃资源配置。
    // 保留条目并返回错误，使调用方仍有机会修正路径或重试。
    const auto missingResult = service.removeAudioResource(
        project,
        MMM::Logic::CmdRemoveAudioResource{ "missing-resource", true });
    // 缺失源文件必须给出错误，但资源仍可由 ID 查回。
    // 该分支确保文件系统失败不会被吞掉后继续执行逻辑删除。
    if ( missingResult.m_removed || missingResult.m_errorMessage.empty() ||
         findResource(project, "missing-resource") == nullptr ) {
        XERROR("Missing source audio removed its project resource entry");
        return false;
    }
    return true;
}

/// @brief 验证内存谱面移动引用重映射会报告匹配并稳定改写字段。
/// @return Note、自动采样和歌曲提示全部按约定更新时返回 true。
/// @note 纯内存重映射不要求项目根存在，不验证文件移动本身。
/// @note 对象绑定改为稳定 ID，歌曲提示和主音频路径继续保存路径。
/// @note 两处元数据字段按提示类别计数，但分别计入实际改写总量。
/// @note
/// 对象引用规范化与路径字段更新在同一调用完成，不能只返回匹配数而不更新内容。
bool testInMemoryAudioReferenceRemapResult()
{
    MMM::Project project;
    project.m_projectRoot = "/tmp/mmm-open-reference-remap";

    // 资源 ID 故意不同于旧路径，便于区分稳定身份与物理位置。
    // 移动后身份不变，只有路径类字段应指向新的位置。
    const MMM::AudioResource previousResource{
        .m_id   = "stable-audio-id",
        .m_path = "old/song.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };

    MMM::BeatMap beatmap;
    // 两个元数据音频字段都使用旧路径，统计应计为两处提示类引用。
    // Note 与自动采样各提供一处引用，合计四处实际变更。
    beatmap.m_baseMapMetadata.song_file_hint  = "old/song.wav";
    beatmap.m_baseMapMetadata.main_audio_path = "old/song.wav";
    MMM::Note note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "old/song.wav", 0.75F });
    beatmap.m_noteData.notes.push_back(std::move(note));
    beatmap.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_audioResourceId = "old/song.wav" });

    // 直接调用内存谱面入口，修改结果与分类计数在同一次操作中核对。
    // 不通过再次加载磁盘文件掩盖内存会话未及时更新的问题。
    const auto result = MMM::Logic::ProjectResourceService::
        remapBeatmapAudioReferencesAfterMove(
            project, beatmap, "Open.mmm", previousResource, "new/song.wav");
    // 分别核对每类匹配数、总改写数和便捷状态谓词。
    // 最终逐字段断言防止计数正确但写入了错误的 ID 或路径。
    if ( result.m_noteBindingReferenceCount != 1U ||
         result.m_audioSampleReferenceCount != 1U ||
         result.m_songFileHintReferenceCount != 2U ||
         result.m_changedReferenceCount != 4U || !result.referencesResource() ||
         !result.changed() ||
         beatmap.m_noteData.notes.front()
                 .getSampleBinding()
                 ->m_audioResourceId != "stable-audio-id" ||
         beatmap.m_audioSamples.front().m_audioResourceId !=
             "stable-audio-id" ||
         beatmap.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("new/song.wav") ||
         beatmap.m_baseMapMetadata.main_audio_path !=
             std::filesystem::path("new/song.wav") ) {
        XERROR("In-memory moved audio references were not remapped safely");
        return false;
    }
    return true;
}

/// @brief 验证保存提示保留有效引用并回退到最早 Main 自动采样。
/// @return 提示选择不增删采样且旧单音轨字段始终清空时返回 true。
/// @note 连续覆盖已有有效提示、提示失效但有 Main 采样、没有 Main
/// 可回退三种状态。
/// @note 该入口准备保存元数据，本用例只检查内存状态，不执行文件写入。
/// @note 有效提示优先规则不要求目标为 Main，只有无效提示才触发 Main 回退。
/// @note 本组时间排列足以区分最早 Main
/// 与首个采样，但不独立证明偏移决定顺序的边界。
/// @note 数量断言只证明不增删采样，不等于逐字段证明所有采样参数不变。
bool testSongFileHintSaveSemantics()
{
    MMM::Project project;
    project.m_projectRoot    = "/tmp/mmm-song-file-hint";
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "effect-hint",
                            .m_path = "audio/effect.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "main-late",
                            .m_path = "audio/late.ogg",
                            .m_type = MMM::AudioTrackType::Main },
        MMM::AudioResource{ .m_id   = "main-early",
                            .m_path = "audio/early.ogg",
                            .m_type = MMM::AudioTrackType::Main },
    };

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path        = "charts/Hint.mmm";
    beatmap.m_baseMapMetadata.song_file_hint  = "audio/effect.wav";
    beatmap.m_baseMapMetadata.main_audio_path = "legacy-main.ogg";
    // 把 Effect 放在更早位置，确保回退不是无条件选取所有采样中的最早项。
    // 两个 Main 的排列与时间先后不同，可发现仅取列表首项的错误。
    beatmap.m_audioSamples = {
        MMM::AudioSampleEvent{ .m_timestamp       = -1000.0,
                               .m_audioResourceId = "effect-hint" },
        MMM::AudioSampleEvent{ .m_timestamp       = 1000.0,
                               .m_offsetMs        = -100,
                               .m_audioResourceId = "main-late" },
        MMM::AudioSampleEvent{ .m_timestamp       = 800.0,
                               .m_offsetMs        = -300,
                               .m_audioResourceId = "main-early" },
    };
    // 保存刷新只选择提示，不应额外物化或删除采样。
    // 每个分支都保留数量断言，避免元数据更新偷偷改变时间线。
    const auto sampleCount = beatmap.m_audioSamples.size();

    // 已有提示即使指向 Effect 也应优先保留，不能被 Main 回退覆盖。
    // 旧 main_audio_path 同时清空，确保兼容字段不会继续充当第二音频来源。
    auto result =
        MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
            project, beatmap, beatmap.m_baseMapMetadata.map_path);
    // 不仅检查写回路径，还检查来源枚举与解析出的资源 ID。
    // 路径相同可能由错误分支偶然得到，来源信息用于验证选择流程。
    if ( result.m_source !=
             MMM::Logic::BeatmapSongFileHintSource::ExistingHint ||
         result.m_audioResourceId != "effect-hint" ||
         beatmap.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/effect.wav") ||
         !beatmap.m_baseMapMetadata.main_audio_path.empty() ||
         beatmap.m_audioSamples.size() != sampleCount ) {
        XERROR("Valid song_file_hint was not preserved on save");
        return false;
    }

    // 主动使已有提示失效，强制进入 Main 采样选择分支。
    // 保持同一采样集合，避免更换夹具时掩盖两种选择规则的区别。
    beatmap.m_baseMapMetadata.song_file_hint  = "audio/missing.ogg";
    beatmap.m_baseMapMetadata.main_audio_path = "legacy-main.ogg";
    result = MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
        project, beatmap, beatmap.m_baseMapMetadata.map_path);
    if ( result.m_source !=
             MMM::Logic::BeatmapSongFileHintSource::EarliestMainSample ||
         result.m_audioResourceId != "main-early" ||
         beatmap.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/early.ogg") ||
         !beatmap.m_baseMapMetadata.main_audio_path.empty() ||
         beatmap.m_audioSamples.size() != sampleCount ) {
        XERROR("Invalid song_file_hint did not select earliest Main sample");
        return false;
    }

    // 把全部资源改为 Effect，清除最后一个合法 Main 回退来源。
    // 再次使用无效提示，应返回 None 并清空过时路径而非保留上次选择。
    for ( auto& resource : project.m_audioResources ) {
        resource.m_type = MMM::AudioTrackType::Effect;
    }
    beatmap.m_baseMapMetadata.song_file_hint = "audio/missing-again.ogg";
    result = MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
        project, beatmap, beatmap.m_baseMapMetadata.map_path);
    if ( result.m_source != MMM::Logic::BeatmapSongFileHintSource::None ||
         !beatmap.m_baseMapMetadata.song_file_hint.empty() ||
         beatmap.m_audioSamples.size() != sampleCount ) {
        XERROR("Stale song_file_hint remained without a Main sample fallback");
        return false;
    }
    return true;
}

/// @brief 验证模板自动采样按 BGM 相对轨道迁移到不同 Key 数的新谱面。
/// @return 4K 到 6K 的首轨映射、空轨保留和轨道扩展均正确时返回 true。
/// @note 自动采样采用玩家轨数之后的统一编号，复制时保留 BGM 局部偏移。
/// @note 玩家 Note 的轨号保持原值，不随 BGM 起点整体平移。
/// @note 源统一编号四和六分别是 BGM 局部零和二，目标六键下应得到六和八。
/// @note 声明五轨时保留空尾轨，声明不足时按最高实际局部轨补到三轨。
/// @note 回读验证使用第一次创建结果，第二次扩展分支主要验证命令返回的内存内容。
bool testTemplateAudioSampleTrackRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 4;
    source->m_baseMapMetadata.bgm_track_count = 5;
    // 源采样占 BGM 第一和第三轨，中间空轨不能在复制时压缩掉。
    // 声明五条 BGM 轨进一步检查尾部未占用轨是否仍保留。
    source->m_audioSamples = {
        MMM::AudioSampleEvent{ .m_track = 4, .m_audioResourceId = "first-bgm" },
        MMM::AudioSampleEvent{ .m_track = 6, .m_audioResourceId = "third-bgm" },
    };
    // 独立玩家音符作为对照，区分采样重映射与全体物件错误平移。
    // 它在新六键谱面仍位于第四条玩家轨。
    MMM::Note sourceNote;
    sourceNote.m_track = 3;
    source->m_noteData.notes.push_back(std::move(sourceNote));
    source->sync();

    MMM::Logic::CmdCreateBeatmap preserveCommand;
    preserveCommand.baseMeta.name    = "TemplatePreserve";
    preserveCommand.baseMeta.version = "TemplatePreserve";
    // 目标玩家轨数从四改为六，采样统一轨号应同时增加二。
    // 目标请求的 BGM 数较小，模板声明的五条轨不能被该值截断。
    preserveCommand.baseMeta.track_count        = 6;
    preserveCommand.baseMeta.bgm_track_count    = 1;
    preserveCommand.templateBeatmap             = source;
    preserveCommand.templateOptions.copyObjects = true;

    // 复制选项明确包含物件，自动采样应与玩家物件一起复制。
    // 如果关闭物件复制，这组轨号断言就不能验证模板重映射。
    const auto preserveResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          preserveCommand);
    // 先确认返回对象存在，再访问采样下标与元数据。
    // 两条采样分别检查首轨和稀疏轨，避免只验证统一平移的单个点。
    if ( !preserveResult.m_created || !preserveResult.m_beatmap ||
         preserveResult.m_beatmap->m_audioSamples.size() != 2 ||
         preserveResult.m_beatmap->m_audioSamples[0].m_track != 6 ||
         preserveResult.m_beatmap->m_audioSamples[1].m_track != 8 ||
         preserveResult.m_beatmap->m_baseMapMetadata.bgm_track_count != 5 ||
         preserveResult.m_beatmap->m_noteData.notes.size() != 1 ||
         preserveResult.m_beatmap->m_noteData.notes.front().m_track != 3 ) {
        XERROR("Template BGM-relative sample tracks were not preserved");
        return false;
    }

    // 重新从磁盘加载，验证内存重映射结果已经进入正式保存文件。
    // 只检查返回对象无法发现序列化仍写出旧轨号的错误。
    const auto persisted =
        MMM::BeatMap::loadFromFile(directory.path() / "TemplatePreserve.mmm");
    if ( persisted.m_audioSamples.size() != 2 ||
         persisted.m_audioSamples[0].m_track != 6 ||
         persisted.m_audioSamples[1].m_track != 8 ||
         persisted.m_baseMapMetadata.bgm_track_count != 5 ) {
        XERROR("Remapped template sample tracks were not persisted");
        return false;
    }

    // 让模板声明小于实际第三轨占用，验证根据有效采样补足轨数。
    // 第二次使用新文件名，避免覆盖第一次保留空轨的持久化结果。
    source->m_baseMapMetadata.bgm_track_count  = 1;
    MMM::Logic::CmdCreateBeatmap expandCommand = preserveCommand;
    expandCommand.baseMeta.name                = "TemplateExpand";
    expandCommand.baseMeta.version             = "TemplateExpand";
    const auto expandResult = MMM::Logic::ProjectCommandService{}.createBeatmap(
        project, expandCommand);
    // 扩展分支同时检查轨数三与统一轨号六、八。
    // 只增加轨数但未平移采样，仍会把旧编号误解释成玩家轨或错误 BGM 轨。
    if ( !expandResult.m_created || !expandResult.m_beatmap ||
         expandResult.m_beatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         expandResult.m_beatmap->m_audioSamples.size() != 2 ||
         expandResult.m_beatmap->m_audioSamples[0].m_track != 6 ||
         expandResult.m_beatmap->m_audioSamples[1].m_track != 8 ) {
        XERROR("Template BGM track count did not expand for sparse samples");
        return false;
    }
    return true;
}

/// @brief 验证 IMD 风格的未标记 Polyline 子物件不会被重复复制。
/// @return 新谱面只保留一份折线子物件且引用顺序正确时返回 true。
/// @note 构造引用已建立但子标志未设置的输入，复制必须同时识别引用关系。
/// @note 同时验证数量、指针归属及保存回读，避免只修正标志却重复生成对象。
/// @note 引用地址比较以新谱面分类容器为基准，不能借用模板原容器。
/// @note 源子标志的前置断言保证测试持续覆盖未标记输入，而非初始化已修正的输入。
/// @note 落盘部分验证数量与子列表长度，指针身份只在内存复制结果中断言。
bool testImdStyleTemplatePolylineCopyAvoidsDuplicateChildren()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 4;

    // 额外普通点击不属于折线，复制去重不能误删独立对象。
    // 时间和轨道与后续折线不同，便于区分两类来源。
    MMM::Note standaloneNote;
    standaloneNote.m_timestamp = 500.0;
    standaloneNote.m_track     = 3;
    source->m_noteData.notes.push_back(std::move(standaloneNote));

    // 首段位于零轨，随后 Flick 从零轨转到一轨，再由第二段继续持续。
    // 相同转折时间配合不同类型，便于检查通用子列表的真实顺序。
    MMM::Hold firstHold;
    firstHold.m_timestamp = 1000.0;
    firstHold.m_duration  = 500.0;
    firstHold.m_track     = 0;
    source->m_noteData.holds.push_back(std::move(firstHold));

    MMM::Flick turnFlick;
    turnFlick.m_timestamp = 1500.0;
    turnFlick.m_track     = 0;
    turnFlick.m_dtrack    = 1;
    source->m_noteData.flicks.push_back(std::move(turnFlick));

    MMM::Hold secondHold;
    secondHold.m_timestamp = 1500.0;
    secondHold.m_duration  = 500.0;
    secondHold.m_track     = 1;
    source->m_noteData.holds.push_back(std::move(secondHold));

    // 父折线引用已存在的两个 Hold 和一个 Flick，顺序构成持续、横移、持续。
    // 分类子容器与通用子列表共同指向这些对象，没有创建新的几何副本。
    MMM::Polyline polyline;
    polyline.m_timestamp = 1000.0;
    polyline.m_track     = 0;
    polyline.m_subNotes.push_back(source->m_noteData.holds[0]);
    polyline.m_subHolds.push_back(source->m_noteData.holds[0]);
    polyline.m_subNotes.push_back(source->m_noteData.flicks[0]);
    polyline.m_subFlicks.push_back(source->m_noteData.flicks[0]);
    polyline.m_subNotes.push_back(source->m_noteData.holds[1]);
    polyline.m_subHolds.push_back(source->m_noteData.holds[1]);
    source->m_noteData.polylines.push_back(std::move(polyline));
    // 同步派生状态后再检查未标记前提，确保被测输入与正式模板状态一致。
    // 源结构若已被同步自动修正，测试应明确失败而不是继续获得伪通过。
    source->sync();

    // 先断言夹具确实保留未标记的 IMD 风格状态。
    // 否则初始化若自动设置子标志，测试会变成普通模板复制而失去回归覆盖。
    if ( source->m_noteData.holds[0].m_isSubNote ||
         source->m_noteData.holds[1].m_isSubNote ||
         source->m_noteData.flicks[0].m_isSubNote ) {
        XERROR("IMD-style template fixture unexpectedly marked sub-notes");
        return false;
    }

    MMM::Logic::CmdCreateBeatmap command;
    command.baseMeta.name               = "ImdTemplatePolyline";
    command.baseMeta.version            = "ImdTemplatePolyline";
    command.baseMeta.track_count        = 4;
    command.templateBeatmap             = source;
    command.templateOptions.copyObjects = true;

    const auto result =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project, command);
    if ( !result.m_created || !result.m_beatmap ) {
        XERROR("IMD-style Polyline template creation failed");
        return false;
    }

    // 四类容器数量联合检查，确保既复制独立点击也只保留一份每个子对象。
    // 仅检查父折线数会漏掉作为独立 Hold 或 Flick 重复加入的副本。
    const auto& copied = *result.m_beatmap;
    if ( copied.m_noteData.notes.size() != 1U ||
         copied.m_noteData.holds.size() != 2U ||
         copied.m_noteData.flicks.size() != 1U ||
         copied.m_noteData.polylines.size() != 1U ) {
        XERROR(
            "IMD-style Polyline children were duplicated during template copy");
        return false;
    }

    // 地址断言要求新父引用新谱面自己的分类容器元素。
    // 数值相同仍可能借用了源模板对象，必须检查实际归属及子标志。
    const auto& copiedPolyline = copied.m_noteData.polylines.front();
    if ( copiedPolyline.m_subNotes.size() != 3U ||
         &copiedPolyline.m_subNotes[0].get() != &copied.m_noteData.holds[0] ||
         &copiedPolyline.m_subNotes[1].get() != &copied.m_noteData.flicks[0] ||
         &copiedPolyline.m_subNotes[2].get() != &copied.m_noteData.holds[1] ||
         !copied.m_noteData.holds[0].m_isSubNote ||
         !copied.m_noteData.flicks[0].m_isSubNote ||
         !copied.m_noteData.holds[1].m_isSubNote ) {
        XERROR("Copied Polyline child references or flags are invalid");
        return false;
    }

    // 落盘回读后再次检查容器及子列表数量，覆盖保存过程重新展开造成重复的风险。
    // 源模板与返回对象的指针关系不作为序列化后身份的期望。
    const auto persisted = MMM::BeatMap::loadFromFile(
        directory.path() / "ImdTemplatePolyline.mmm");
    if ( persisted.m_noteData.notes.size() != 1U ||
         persisted.m_noteData.holds.size() != 2U ||
         persisted.m_noteData.flicks.size() != 1U ||
         persisted.m_noteData.polylines.size() != 1U ||
         persisted.m_noteData.polylines.front().m_subNotes.size() != 3U ) {
        XERROR("IMD-style Polyline template fix was not persisted");
        return false;
    }
    return true;
}

/// @brief 验证非法模板物件不会产生部分谱面、文件或项目资源。
/// @return 越界玩家列、落入玩家区的采样和轨道溢出均被原子拒绝时返回 true。
/// @note 三类非法输入使用不同目标文件名，共用已有项目记录作为不变基线。
/// @note 拒绝必须同时保持返回对象为空、项目资源不变及目标文件不存在。
/// @note 采样先合法后非法的排列检验整体预检，不能边复制边提交前缀。
/// @note 玩家音符越界与采样编号溢出分别覆盖有界轨域和整数表示范围。
/// @note 已有项目条目作为保留基线，失败后项目不能被清空或追加半成品。
bool testInvalidTemplateObjectTracksAreRejectedAtomically()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Existing", "Existing.mmm", {} });

    // 先放一个合法采样再放落在玩家域的非法采样。
    // 这能发现处理前缀后才校验后续输入、留下部分新谱面的错误。
    auto playerLaneSource = std::make_shared<MMM::BeatMap>();
    playerLaneSource->m_baseMapMetadata.track_count     = 4;
    playerLaneSource->m_baseMapMetadata.bgm_track_count = 1;
    playerLaneSource->m_audioSamples                    = {
        MMM::AudioSampleEvent{ .m_track           = 4,
                               .m_audioResourceId = "valid-first" },
        MMM::AudioSampleEvent{ .m_track           = 3,
                               .m_audioResourceId = "invalid-player" },
    };

    MMM::Logic::CmdCreateBeatmap playerLaneCommand;
    playerLaneCommand.baseMeta.name        = "RejectedPlayerLane";
    playerLaneCommand.baseMeta.version     = "RejectedPlayerLane";
    playerLaneCommand.baseMeta.track_count = 6;
    // 提供可能触发资源物化的歌曲提示，失败时资源表仍应为空。
    // 拒绝不能只阻止写谱面，却残留准备阶段创建的音频资源。
    playerLaneCommand.baseMeta.song_file_hint     = "pending-main.ogg";
    playerLaneCommand.templateBeatmap             = playerLaneSource;
    playerLaneCommand.templateOptions.copyObjects = true;

    // 命令目标六键不能使源玩家区采样变为合法：来源域必须按源四键解释。
    // 否则源编号三可能在减去玩家轨数时发生负值转无符号。
    const auto playerLaneResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          playerLaneCommand);
    if ( playerLaneResult.m_created || playerLaneResult.m_beatmap ||
         project.m_beatmaps.size() != 1 || !project.m_audioResources.empty() ||
         std::filesystem::exists(directory.path() /
                                 "RejectedPlayerLane.mmm") ) {
        XERROR("Player-lane template sample was not rejected atomically");
        return false;
    }

    // 源六键谱面的第五索引音符无法放进目标四键谱面。
    // 验证玩家音符不能通过钳制或静默丢弃强行完成模板复制。
    auto playerNoteSource = std::make_shared<MMM::BeatMap>();
    playerNoteSource->m_baseMapMetadata.track_count = 6;
    MMM::Note outOfRangeNote;
    outOfRangeNote.m_track = 5;
    playerNoteSource->m_noteData.notes.push_back(std::move(outOfRangeNote));
    playerNoteSource->sync();

    MMM::Logic::CmdCreateBeatmap playerNoteCommand;
    playerNoteCommand.baseMeta.name               = "RejectedPlayerNote";
    playerNoteCommand.baseMeta.version            = "RejectedPlayerNote";
    playerNoteCommand.baseMeta.track_count        = 4;
    playerNoteCommand.templateBeatmap             = playerNoteSource;
    playerNoteCommand.templateOptions.copyObjects = true;

    // 玩家音符在源六键中合法，但目标四键无法容纳它。
    // 拒绝针对目标可表达性，不代表源模板自身无法加载。
    const auto playerNoteResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          playerNoteCommand);
    if ( playerNoteResult.m_created || playerNoteResult.m_beatmap ||
         project.m_beatmaps.size() != 1 || !project.m_audioResources.empty() ||
         std::filesystem::exists(directory.path() /
                                 "RejectedPlayerNote.mmm") ) {
        XERROR("Out-of-range template Note was not rejected atomically");
        return false;
    }

    // 使用无符号最大轨号，目标玩家轨数增加后统一编号会超过可表示范围。
    // 需要在缩窄或加法溢出前拒绝，不能回绕成一个看似有效的低轨号。
    auto overflowSource = std::make_shared<MMM::BeatMap>();
    overflowSource->m_baseMapMetadata.track_count     = 1;
    overflowSource->m_baseMapMetadata.bgm_track_count = 1;
    overflowSource->m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_track           = std::numeric_limits<std::uint32_t>::max(),
        .m_audioResourceId = "overflow",
    });

    MMM::Logic::CmdCreateBeatmap overflowCommand;
    overflowCommand.baseMeta.name               = "RejectedOverflow";
    overflowCommand.baseMeta.version            = "RejectedOverflow";
    overflowCommand.baseMeta.track_count        = 2;
    overflowCommand.templateBeatmap             = overflowSource;
    overflowCommand.templateOptions.copyObjects = true;

    // 源一键改成目标二键，需要在采样统一轨号上增加一。
    // 最大无符号值无法执行该迁移，结果不能回绕到零号玩家轨。
    const auto overflowResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          overflowCommand);
    // 最后一个拒绝分支再次核对项目原记录与磁盘状态。
    // 返回失败但留下目标文件或资源条目仍属于不完整回退。
    if ( overflowResult.m_created || overflowResult.m_beatmap ||
         project.m_beatmaps.size() != 1 || !project.m_audioResources.empty() ||
         std::filesystem::exists(directory.path() / "RejectedOverflow.mmm") ) {
        XERROR("Overflowing template sample was not rejected atomically");
        return false;
    }
    return true;
}

/// @brief 验证新建谱面把所选 Main 物化为第一条 BGM 轨的自动采样。
/// @return 新建谱面行为正确时返回 true。
/// @note 新建与保存提示刷新不同，新建应显式创建可播放的 Main 采样事件。
/// @note 默认作者来自受控配置夹具，测试结束后恢复进程原设置。
/// @note 所选路径解析成稳定资源 ID，但歌曲提示仍按路径语义保留。
/// @note 作者为空时才涉及默认作者规则，本用例通过未设置 author
/// 的命令触发默认值。
/// @note 新建返回对象与项目序列化分别检查，避免只清理一种旧字段表示。
bool testCreateBeatmapMaterializesMainSample()
{
    // 为默认作者提供确定输入，避免结果随运行机器用户配置变化。
    // 目录夹具和配置夹具都通过析构处理提前失败的清理。
    ScopedDefaultCreator       creator("Creator Test");
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto audioPath = directory.path() / "audio" / "song.ogg";
    if ( !createAudioPlaceholder(audioPath) ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "song-resource",
                            .m_path = "audio/song.ogg",
                            .m_type = MMM::AudioTrackType::Main });

    MMM::Logic::CmdCreateBeatmap command;
    command.baseMeta.name        = "Created";
    command.baseMeta.version     = "Created";
    command.baseMeta.track_count = 4;
    // 用户选择用路径表达，结果采样应解析为项目稳定资源 ID。
    // 歌曲提示本身仍保留路径，两个字段的持久语义不同。
    command.baseMeta.song_file_hint = "audio/song.ogg";

    const auto result =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project, command);
    if ( !result.m_created || !result.m_beatmap ||
         result.m_beatmap->m_audioSamples.size() != 1 ) {
        XERROR("New beatmap did not materialize selected Main audio");
        return false;
    }

    // 检查时间零、偏移零及首条 BGM 统一轨号，保证新建即可表达主音轨播放。
    // 还要求至少一条 BGM 轨，避免事件存在却落在未声明区域。
    const auto& sample = result.m_beatmap->m_audioSamples.front();
    if ( sample.m_audioResourceId != "song-resource" ||
         sample.m_timestamp != 0.0 || sample.m_offsetMs != 0 ||
         sample.m_track != 4 ||
         result.m_beatmap->m_baseMapMetadata.bgm_track_count < 1 ||
         result.m_beatmap->m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/song.ogg") ||
         result.m_beatmap->m_baseMapMetadata.author != "Creator Test" ||
         !result.m_beatmap->m_baseMapMetadata.main_audio_path.empty() ) {
        XERROR("Materialized Main sample fields were incorrect");
        return false;
    }
    if ( project.m_beatmaps.size() != 1 ||
         !project.m_beatmaps.front().m_audioTrackId.empty() ) {
        XERROR("New beatmap entry still authored a legacy audio track ID");
        return false;
    }

    // 除了内存旧字段为空，还检查当前项目 JSON 不再输出旧键。
    // 这样不会把迁移后的兼容字段重新写回下一次保存文件。
    const nlohmann::json projectJson = project;
    return !projectJson["m_beatmaps"][0].contains("m_audioTrackId");
}

/// @brief 验证活动谱面默认音频解析优先使用提示和 Main 自动采样。
/// @return 默认资源选择符合预期时返回 true。
/// @note 有效歌曲提示可指向 Effect；无提示时才选择 Main 自动采样。
/// @note 用例仅验证默认资源解析，不修改谱面或物化新的采样事件。
/// @note 资源表包含 Effect 和 Main，两个分支分别验证提示优先和 Main 筛选。
/// @note 第二个谱面不继承第一个谱面的提示，避免合法提示遮蔽采样回退逻辑。
bool testDefaultBeatmapAudioResolution()
{
    MMM::Project project;
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "effect.wav",
                            .m_path = "audio/effect.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "main.ogg",
                            .m_path = "audio/main.ogg",
                            .m_type = MMM::AudioTrackType::Main },
    };

    // 先提供指向 Effect 的有效提示，证明提示优先于类型偏好。
    // 若实现只搜 Main，即使资源表存在该文件也不能通过。
    MMM::BeatMap hintedBeatmap;
    hintedBeatmap.m_baseMapMetadata.song_file_hint = "audio/effect.wav";
    const auto* hinted =
        MMM::Logic::ProjectResourceService::findDefaultBeatmapAudioResource(
            project, hintedBeatmap, "Hinted.mmm");
    if ( !hinted || hinted->m_id != "effect.wav" ) {
        XERROR("Beatmap song_file_hint was not preferred");
        return false;
    }

    MMM::BeatMap sampleBeatmap;
    // 把 Effect 排在时间更早的位置，排除简单取最早任意采样的实现。
    // 后面的 Main 虽晚，仍应被选作没有提示时的默认资源。
    MMM::AudioSampleEvent earlyEffect;
    earlyEffect.m_timestamp       = 0.0;
    earlyEffect.m_audioResourceId = "effect.wav";
    sampleBeatmap.m_audioSamples.push_back(earlyEffect);
    MMM::AudioSampleEvent laterMain;
    laterMain.m_timestamp       = 1000.0;
    laterMain.m_audioResourceId = "main.ogg";
    sampleBeatmap.m_audioSamples.push_back(laterMain);
    const auto* sampleDefault =
        MMM::Logic::ProjectResourceService::findDefaultBeatmapAudioResource(
            project, sampleBeatmap, "Samples.mmm");
    if ( !sampleDefault || sampleDefault->m_id != "main.ogg" ) {
        XERROR("Main automatic sample was not selected as beatmap default");
        return false;
    }
    return true;
}

/// @brief 验证旧项目单主音轨字段只为缺失时间线的 MMM 物化采样。
/// @return 迁移不移动 Note/Timing 且不会重复物化时返回 true。
/// @note 已加载项目与旧持久化项目分开提供，迁移信息只来自后者的旧字段。
/// @note 重复迁移是幂等性检查，不能再次添加相同主音轨事件。
/// @note 迁移输入保留旧持久化条目，当前项目提供可解析的实际资源路径。
/// @note 既有 Note 与 Timing 用非零时间定位，迁移后仍应保持原始时间和轨道。
/// @note 重复调用继续提供旧条目，要求已有音频时间线成为幂等跳过依据。
bool testLegacyProjectAudioTrackMigration()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto audioPath = directory.path() / "audio" / "song.ogg";
    if ( !createAudioPlaceholder(audioPath) ) return false;

    const auto   mapPath = directory.path() / "Legacy.mmm";
    MMM::BeatMap source;
    source.m_baseMapMetadata.name        = "Legacy";
    source.m_baseMapMetadata.version     = "Legacy";
    source.m_baseMapMetadata.track_count = 6;
    // 保存非零时间的 Timing 和 Note 作为内容不变基线。
    // 迁移只添加音频时间线，不能按主音轨起点平移原谱面事件。
    MMM::Timing timing;
    timing.m_timestamp = 123.0;
    timing.m_bpm       = 150.0;
    source.m_timings.push_back(timing);
    MMM::Note note;
    note.m_timestamp = 456.0;
    note.m_track     = 2;
    source.m_noteData.notes.push_back(note);
    source.sync();
    if ( !source.saveToFile(mapPath) ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Legacy", "Legacy.mmm", {} });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "song.ogg",
                            .m_path = "audio/song.ogg",
                            .m_type = MMM::AudioTrackType::Effect });

    // 当前项目条目不带旧 ID，单独构造旧项目数据提供待迁移来源。
    // 这覆盖打开旧文件后兼容字段被分离处理的调用方式。
    MMM::Project persistedProject;
    persistedProject.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Legacy", "Legacy.mmm", "song.ogg" });

    MMM::Logic::ProjectResourceService service;
    const auto                         migration =
        service.migrateLegacyBeatmapAudioTracks(project, persistedProject);
    // 迁移计数与失败路径列表同时校验，要求唯一目标完整处理。
    // 随后磁盘回读负责验证具体内容，计数本身不证明采样字段正确。
    if ( migration.m_migratedBeatmapCount != 1 ||
         !migration.m_failedBeatmapPaths.empty() ) {
        XERROR("Legacy project audio track migration did not complete");
        return false;
    }

    // 从真实文件读取迁移结果，检查首 BGM 轨、稳定引用与歌曲提示。
    // 同时保留原音符位置和 Timing 时间，防止迁移改动无关内容。
    auto migrated = MMM::BeatMap::loadFromFile(mapPath);
    if ( migrated.m_audioSamples.size() != 1 ||
         migrated.m_audioSamples.front().m_timestamp != 0.0 ||
         migrated.m_audioSamples.front().m_offsetMs != 0 ||
         migrated.m_audioSamples.front().m_track != 6 ||
         migrated.m_audioSamples.front().m_audioResourceId != "song.ogg" ||
         migrated.m_baseMapMetadata.bgm_track_count < 1 ||
         migrated.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/song.ogg") ||
         migrated.m_timings.size() != 1 ||
         migrated.m_timings.front().m_timestamp != 123.0 ||
         migrated.m_noteData.notes.size() != 1 ||
         migrated.m_noteData.notes.front().m_timestamp != 456.0 ||
         migrated.m_noteData.notes.front().m_track != 2 ) {
        XERROR("Legacy audio migration changed chart data or sample fields");
        return false;
    }
    // 迁移还需把旧主音轨资源保留为 Main，不能只创建一个引用 Effect 的主采样。
    // 这项断言针对项目内存表，与谱面文件内容检查互相补充。
    if ( project.m_audioResources.front().m_type !=
         MMM::AudioTrackType::Main ) {
        XERROR("Migrated legacy main resource did not retain Main type");
        return false;
    }

    // 再次给出同一旧项目来源，已经有音频时间线的谱面必须跳过。
    // 计数归零且采样仍为一个，联合证明没有重复物化。
    const auto repeatedMigration =
        service.migrateLegacyBeatmapAudioTracks(project, persistedProject);
    migrated = MMM::BeatMap::loadFromFile(mapPath);
    if ( repeatedMigration.m_migratedBeatmapCount != 0 ||
         migrated.m_audioSamples.size() != 1 ) {
        XERROR("Legacy audio migration duplicated an existing timeline");
        return false;
    }
    return true;
}

/// @brief 验证文件移动只更新资源路径并保持谱面引用 ID 稳定。
/// @return 单文件或目录移动后的路径重映射正确时返回 true。
/// @note 先执行真实目录移动，再通知资源服务更新项目路径与谱面引用。
/// @note 资源稳定 ID 不随目录名称改变，旧路径型采样引用应被规范化为该 ID。
/// @note 目录移动需保留资源相对目录后缀，不能把文件路径替换为目录本身。
/// @note 谱面采样原先采用路径引用，移动后规范化到稳定 ID 而非简单字符串换目录。
bool testAudioResourcePathRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldDirectory = directory.path() / "old";
    const auto newDirectory = directory.path() / "new";
    const auto audioPath    = oldDirectory / "song.ogg";
    if ( !createAudioPlaceholder(audioPath) ) return false;

    const auto mapPath = directory.path() / "Move.mmm";
    if ( !saveReferenceBeatmap(mapPath, "old/song.ogg", "", "old/song.ogg") ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Move", "Move.mmm", {} });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "stable-song-id",
                            .m_path = "old/song.ogg",
                            .m_type = MMM::AudioTrackType::Main });

    std::error_code filesystemError;
    // 操作仅针对隔离测试目录，文件移动失败立即结束夹具准备。
    // 服务接收移动前后路径时，磁盘已经处于新位置。
    std::filesystem::rename(oldDirectory, newDirectory, filesystemError);
    if ( filesystemError ) return false;

    // 修改数量应恰好对应一个资源，资源 ID 保持不变而路径更新。
    // 目录移动入口应按相对后缀保留文件名，不能只替换整个路径为目录名。
    const auto changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldDirectory, newDirectory);
    if ( changed != 1 ||
         project.m_audioResources.front().m_id != "stable-song-id" ||
         project.m_audioResources.front().m_path != "new/song.ogg" ) {
        XERROR("Moved audio resource path was not remapped with a stable ID");
        return false;
    }

    // 磁盘谱面同时检查歌曲提示的新路径和采样稳定 ID。
    // 只更新项目表会让已有谱面继续引用旧目录，因此需要独立回读验证。
    const auto remappedBeatmap = MMM::BeatMap::loadFromFile(mapPath);
    if ( remappedBeatmap.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("new/song.ogg") ||
         remappedBeatmap.m_audioSamples.size() != 1 ||
         remappedBeatmap.m_audioSamples.front().m_audioResourceId !=
             "stable-song-id" ) {
        XERROR("Moved audio references in MMM were not normalized");
        return false;
    }
    return true;
}

/// @brief 验证显式音频重命名会事务更新 MMM 与 Malody 资源 ID。
/// @return 两种内部可表达格式均完整改名且音量保持时返回 true。
/// @note 显式改名与物理路径移动不同，此处要求内部引用 ID 也更新。
/// @note MMM 与 Malody 共用同一事务，任一格式遗漏都会使改写数量不符。
/// @note 两个格式初始都含 Note 和自动采样绑定，成功计数应覆盖两个谱面文件。
/// @note 音量使用容差断言，重命名不得把实例音量重置为默认值。
bool testAudioResourceIdRenameTransaction()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto mmmPath = directory.path() / "Rename.mmm";
    const auto mcPath  = directory.path() / "Rename.mc";
    if ( !saveReferenceBeatmap(
             mmmPath, "audio/old.wav", "audio/old.wav", "audio/old.wav") ||
         !saveReferenceBeatmap(
             mcPath, "audio/old.wav", "audio/old.wav", "audio/old.wav") ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps    = {
        MMM::Project::BeatmapEntry{ "MMM", "Rename.mmm", {} },
        MMM::Project::BeatmapEntry{ "Malody", "Rename.mc", {} },
    };
    const MMM::AudioResource previousResource{
        .m_id   = "stable-old-id",
        .m_path = "audio/old.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };

    // 提供旧稳定 ID、旧路径及新路径和新 ID，允许服务识别原路径型引用。
    // 成功应一次改写两个目标谱面，而不是仅更新项目资源表。
    const auto result =
        MMM::Logic::ProjectResourceService::remapProjectBeatmapAudioResourceId(
            project, previousResource, "audio/renamed.wav", "renamed.wav");
    // 明确要求两个不同格式都成功提交，单文件成功不能算整个操作完成。
    // 失败日志保留服务错误消息，定位暂存、格式支持或写回中的问题。
    if ( !result.m_success || result.m_changedBeatmapCount != 2U ) {
        XERROR("MMM/Malody audio ID rename transaction failed: {}",
               result.m_errorMessage);
        return false;
    }

    // 逐格式回读正式文件，确认 Note 绑定和自动采样均使用新 ID。
    // 绑定音量用浮点容差检查，避免改名时重新创建默认参数。
    for ( const auto& path : { mmmPath, mcPath } ) {
        const auto beatmap = MMM::BeatMap::loadFromFile(path);
        if ( beatmap.m_noteData.notes.size() != 1U ||
             !beatmap.m_noteData.notes.front().getSampleBinding() ||
             beatmap.m_noteData.notes.front()
                     .getSampleBinding()
                     ->m_audioResourceId != "renamed.wav" ||
             std::abs(
                 beatmap.m_noteData.notes.front().getSampleBinding()->m_volume -
                 0.75F) > 1e-6F ||
             beatmap.m_audioSamples.size() != 1U ||
             beatmap.m_audioSamples.front().m_audioResourceId !=
                 "renamed.wav" ) {
            XERROR("Renamed audio ID did not round trip: {}",
                   MMM::Config::pathToUtf8(path));
            return false;
        }
        // 歌曲提示仍保存新物理路径，不能和对象绑定一样改成裸 ID。
        // 每种格式都单独检查这一区别，防止统一替换字符串破坏路径语义。
        if ( beatmap.m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/renamed.wav") ) {
            XERROR("Renamed song_file_hint did not round trip: {}",
                   MMM::Config::pathToUtf8(path));
            return false;
        }
    }
    return true;
}

/// @brief 验证任一谱面无法暂存时不会提交其它谱面的音频 ID。
/// @return 事务失败后全部谱面仍保留旧 ID 时返回 true。
/// @note 第二份谱面的暂存位置被非空目录占用，稳定制造写入失败。
/// @note 第一份谱面本可写入，失败后也必须保持旧引用，验证跨文件提交边界。
/// @note 阻塞目录内放置文件，使暂存路径无法被当作普通空占位清理后继续写入。
/// @note 失败后回读所有参与谱面，不能只检查报错的最后一个文件。
bool testAudioResourceIdRenameTransactionRollback()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto firstPath  = directory.path() / "First.mmm";
    const auto secondPath = directory.path() / "Second.mmm";
    if ( !saveReferenceBeatmap(firstPath, {}, "old.wav", "old.wav") ||
         !saveReferenceBeatmap(secondPath, {}, "old.wav", "old.wav") ) {
        return false;
    }

    // 阻塞路径采用服务的暂存文件名，非空目录防止清理后继续成功写入。
    // 故障仅位于隔离目录中，无需修改宿主权限或系统配置。
    auto blockedTemporaryPath = secondPath;
    blockedTemporaryPath += ".mmm-audio-remap.tmp";
    std::error_code filesystemError;
    std::filesystem::create_directories(blockedTemporaryPath, filesystemError);
    if ( filesystemError ||
         !createAudioPlaceholder(blockedTemporaryPath / "blocker") ) {
        return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    // 把可处理文件放前面、故障文件放后面，覆盖已准备前缀后的整体失败。
    // 若只测试一个失败文件，就无法发现前面的文件被提前提交。
    project.m_beatmaps = {
        MMM::Project::BeatmapEntry{ "First", "First.mmm", {} },
        MMM::Project::BeatmapEntry{ "Second", "Second.mmm", {} },
    };
    // 旧资源 ID 与旧路径相同，使两个谱面的引用都必然匹配此次事务。
    // 这里不测试复杂路径优先级，避免匹配失败遮蔽暂存故障。
    const MMM::AudioResource previousResource{
        .m_id   = "old.wav",
        .m_path = "old.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };
    const auto result =
        MMM::Logic::ProjectResourceService::remapProjectBeatmapAudioResourceId(
            project, previousResource, "renamed.wav", "renamed.wav");
    // 要求明确失败且提供原因，不能把未改写伪装成成功的零变化。
    // 随后回读两个文件验证实际数据，而不只信任事务状态。
    if ( result.m_success || result.m_errorMessage.empty() ) {
        XERROR("Blocked audio ID transaction unexpectedly succeeded");
        return false;
    }

    // 两份谱面的 Note 绑定和自动采样均应保留 old.wav。
    // 该用例检查语义回滚，逐字节不变由后面的 osu 写入失败用例覆盖。
    for ( const auto& path : { firstPath, secondPath } ) {
        const auto beatmap = MMM::BeatMap::loadFromFile(path);
        if ( beatmap.m_noteData.notes.size() != 1U ||
             !beatmap.m_noteData.notes.front().getSampleBinding() ||
             beatmap.m_noteData.notes.front()
                     .getSampleBinding()
                     ->m_audioResourceId != "old.wav" ||
             beatmap.m_audioSamples.size() != 1U ||
             beatmap.m_audioSamples.front().m_audioResourceId != "old.wav" ) {
            XERROR("Failed audio ID transaction partially changed {}",
                   MMM::Config::pathToUtf8(path));
            return false;
        }
    }
    return true;
}

/// @brief 验证 osu! 移动只原位改写音频字段而不重排或丢失其它文本。
/// @return 全局音频和 HitSample 引用更新且哨兵文本保留时返回 true。
/// @note 全局 AudioFilename 与物件 HitSample 采用谱面相对路径，不写项目稳定
/// ID。
/// @note 音效文件名包含冒号，验证字段解析不会截断合法文件名尾部。
/// @note 哨兵验证额外文本得以保留，不等价于逐字节证明所有非音频行不变。
/// @note 移动前后谱面位置不变，音频相对路径只替换目录部分。
/// @note 项目表仍保留稳定 ID，外部文本格式引用则继续使用相对文件路径。
bool testOsuAudioReferenceMoveRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldDirectory = directory.path() / "old";
    const auto newDirectory = directory.path() / "new";
    const auto mainAudio    = oldDirectory / "main.ogg";
    // C:effect.wav 在当前文件系统是普通文件名，冒号同时也是 osu 字段分隔符。
    // 这里专门覆盖解析音效文件名时保留剩余文本的规则。
    const auto effectAudio = oldDirectory / "C:effect.wav";
    const auto mapPath     = directory.path() / "charts" / "Move.osu";
    if ( !createAudioPlaceholder(mainAudio) ||
         !createAudioPlaceholder(effectAudio) ) {
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(mapPath.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name            = "Move";
    beatmap.m_baseMapMetadata.version         = "Move";
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_track           = 4,
        .m_audioResourceId = "../old/main.ogg",
    });
    MMM::Note note;
    // 普通音符单独位于非零时间，避免和全局主音轨导出混为一条事件。
    // Note 的资源绑定由 HitSample 文本表达，需与 AudioFilename 分别修补。
    note.m_timestamp = 1000.0;
    note.m_track     = 1;
    note.setSampleBinding(
        MMM::AudioSampleBinding{ "../old/C:effect.wav", 1.0F });
    beatmap.m_noteData.notes.push_back(std::move(note));
    beatmap.sync();
    if ( !beatmap.saveToFile(mapPath) ) return false;

    {
        // 在标准保存结果末尾追加不属于业务模型的注释哨兵。
        // 移动服务应原位修补引用，重新完整序列化谱面可能丢掉这段文本。
        std::ofstream stream(mapPath, std::ios::binary | std::ios::app);
        stream << "\n// mmm-audio-remap-sentinel\n";
        if ( !stream.good() ) return false;
    }

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Move", "charts/Move.osu", {} });
    project.m_audioResources = {
        MMM::AudioResource{ .m_id   = "stable-main-id",
                            .m_path = "old/main.ogg",
                            .m_type = MMM::AudioTrackType::Main },
        MMM::AudioResource{ .m_id   = "stable-effect-id",
                            .m_path = "old/C:effect.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    // 先调用预检，确认可表达新引用的 osu 移动不会被错误禁止。
    // 实际目录移动发生在预检通过后，服务随后修补已移动资源的引用。
    const auto validation =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, oldDirectory, newDirectory);
    if ( !validation.empty() ) {
        XERROR("Safe osu! audio move was rejected: {}", validation);
        return false;
    }

    std::filesystem::rename(oldDirectory, newDirectory, filesystemError);
    if ( filesystemError ) return false;
    const auto changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldDirectory, newDirectory);
    // 项目表需要同时更新主音轨与 Effect 的路径。
    // 这与后续谱面文本检查分开，防止只修复其中一层状态。
    if ( changed != 2U ||
         project.m_audioResources[0].m_path != "new/main.ogg" ||
         project.m_audioResources[1].m_path != "new/C:effect.wav" ) {
        XERROR("Moved osu! resources were not updated in the project");
        return false;
    }

    // 原始文本验证全局音频和物件音效路径，同时确认哨兵仍存在。
    // 冒号音效引用以完整尾部匹配，避免错误分割后仅保留一部分文件名。
    std::string remappedText;
    if ( !readTextFile(mapPath, remappedText) ||
         remappedText.find("AudioFilename: ../new/main.ogg") ==
             std::string::npos ||
         remappedText.find(":../new/C:effect.wav") == std::string::npos ||
         remappedText.find("// mmm-audio-remap-sentinel") ==
             std::string::npos ) {
        XERROR("osu! audio references were not patched in place");
        return false;
    }

    // 再次用正式解析器读取修补后的文件，确保文本看似正确但语法已损坏的情况会失败。
    // 回读主要检查主采样与普通音符数量，音效完整文本由前一断言覆盖。
    const auto loaded = MMM::BeatMap::loadFromFile(mapPath);
    if ( loaded.m_audioSamples.size() != 1U ||
         loaded.m_audioSamples.front().m_audioResourceId != "../new/main.ogg" ||
         loaded.m_noteData.notes.size() != 1U ) {
        XERROR("Patched osu! references did not round trip");
        return false;
    }
    return true;
}

/// @brief 验证只移动 osu! 谱面时会按新目录重算相对音频引用。
/// @return 资源路径不变且 AudioFilename 指向同一音频文件时返回 true。
/// @note 只移动谱面，音频资源物理位置与项目相对路径都应保持不变。
/// @note 返回改动数针对音频资源，不代表没有发生谱面文本修补。
/// @note 相对路径变化来自谱面所在目录变深，资源本身仍位于项目 audio 目录。
/// @note 零资源改动数与成功修补谱面可同时成立，因此必须独立检查输出文本。
bool testOsuBeatmapOnlyMoveRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto audioPath  = directory.path() / "audio" / "main.ogg";
    const auto oldMapPath = directory.path() / "charts" / "Move.osu";
    // 目标谱面比原路径多一层目录，音频相对路径应增加一个 ..。
    // 音频文件本身不移动，用来区分资源移动与引用基准移动。
    const auto newMapPath = directory.path() / "nested" / "deeper" / "Move.osu";
    if ( !createAudioPlaceholder(audioPath) ) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(oldMapPath.parent_path(),
                                        filesystemError);
    if ( filesystemError ) return false;

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_track           = 4,
        .m_audioResourceId = "../audio/main.ogg",
    });
    beatmap.sync();
    if ( !beatmap.saveToFile(oldMapPath) ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Move", "charts/Move.osu", {} });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "stable-main-id",
                            .m_path = "audio/main.ogg",
                            .m_type = MMM::AudioTrackType::Main });

    const auto validation =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, oldMapPath, newMapPath);
    if ( !validation.empty() ) return false;

    std::filesystem::create_directories(newMapPath.parent_path(),
                                        filesystemError);
    if ( filesystemError ) return false;
    // 先把谱面移到新位置，服务需要从移动前的项目条目识别这次路径变化。
    // 目标父目录由夹具准备，避免目录不存在成为非预期失败原因。
    std::filesystem::rename(oldMapPath, newMapPath, filesystemError);
    if ( filesystemError ) return false;

    std::string errorMessage;
    const auto  changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldMapPath, newMapPath, &errorMessage);
    // 音频资源数量变化为零且路径保持原值，但错误消息也必须为空。
    // 不能把正常的纯谱面移动误报为引用重写失败。
    if ( changed != 0U || !errorMessage.empty() ||
         project.m_audioResources.front().m_path != "audio/main.ogg" ) {
        XERROR("Moving only an osu! beatmap changed its audio resource");
        return false;
    }

    // 在新路径读取文本，验证 AudioFilename 按新目录深度重新计算。
    // 检查旧路径文件无法证明实际移动后的谱面可以找到原音频。
    std::string remappedText;
    if ( !readTextFile(newMapPath, remappedText) ||
         remappedText.find("AudioFilename: ../../audio/main.ogg") ==
             std::string::npos ) {
        XERROR("Moved osu! beatmap did not recalculate AudioFilename");
        return false;
    }
    return true;
}

/// @brief 验证 osu! 引用写盘失败会恢复谱面并回滚物理音频移动。
/// @return 错误可上报且项目、文件和谱面均保持旧状态时返回 true。
/// @note 预检通过且源音频已物理移动后，才阻塞谱面暂存写入。
/// @note 回滚必须跨越项目表、谱面文本和音频文件位置三个状态层。
/// @note 写盘故障在移动后注入，覆盖需要撤销已经发生的物理操作的失败路径。
/// @note 原始文本以二进制读取，比较包含换行和未知字段在内的全部字节。
bool testOsuMoveWriteFailureRollsBack()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldAudio = directory.path() / "old" / "main.ogg";
    const auto newAudio = directory.path() / "new" / "main.ogg";
    const auto mapPath  = directory.path() / "charts" / "Rollback.osu";
    if ( !createAudioPlaceholder(oldAudio) ) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(mapPath.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_track           = 4,
        .m_audioResourceId = "../old/main.ogg",
    });
    beatmap.sync();
    if ( !beatmap.saveToFile(mapPath) ) return false;

    // 提前保存文件原始字节，失败后逐字节比较而非只重新解析引用。
    // 这样可以发现格式变化、额外行丢失或部分文本写入。
    std::string originalText;
    if ( !readTextFile(mapPath, originalText) ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Rollback", "charts/Rollback.osu", {} });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "stable-main-id",
                            .m_path = "old/main.ogg",
                            .m_type = MMM::AudioTrackType::Main });

    // 验证初始移动本身合法，后续失败明确来自暂存写入。
    // 不以权限或无效输入替代写盘阶段故障，避免测错回滚分支。
    const auto validation =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, oldAudio, newAudio);
    if ( !validation.empty() ) return false;

    std::filesystem::create_directories(newAudio.parent_path(),
                                        filesystemError);
    if ( filesystemError ) return false;
    std::filesystem::rename(oldAudio, newAudio, filesystemError);
    if ( filesystemError ) return false;

    // 物理音频已在新路径，随后用非空目录占住谱面临时文件。
    // 这要求失败处理主动把音频移回，不能只停止更新项目表。
    auto blockedTemporaryPath = mapPath;
    blockedTemporaryPath += ".mmm-audio-remap.tmp";
    std::filesystem::create_directories(blockedTemporaryPath, filesystemError);
    if ( filesystemError ||
         !createAudioPlaceholder(blockedTemporaryPath / "blocker") ) {
        return false;
    }

    std::string errorMessage;
    const auto  changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldAudio, newAudio, &errorMessage);
    // 最终回读使用独立字符串，原始字节基线保持不变。
    // 不能把结果读入 originalText 后再与自身比较，否则会掩盖部分写入。
    std::string finalText;
    // 联合检查旧源恢复、新源消失、项目路径回旧值和谱面字节完全相同。
    // 还要求非空错误消息，保证上层知道移动没有完成。
    if ( changed != 0U || errorMessage.empty() ||
         !std::filesystem::exists(oldAudio) ||
         std::filesystem::exists(newAudio) ||
         project.m_audioResources.front().m_path != "old/main.ogg" ||
         !readTextFile(mapPath, finalText) || finalText != originalText ) {
        XERROR("Failed osu! rewrite did not roll back the complete move");
        return false;
    }
    return true;
}

/// @brief 验证 RM/IMD 隐式音频关联在移动前被保护。
/// @return 单独改名被拒绝、保持相对关系的整目录移动被允许时返回 true。
/// @note IMD 的音频关联依赖隐式名称关系，无法像 osu 一样直接修补显式字段。
/// @note 预检负责拒绝破坏关系的单文件改名，完整目录移动保持相对布局可继续。
/// @note 同名音频与谱面由夹具实际保存，预检不能仅凭资源表扩展名猜测关系。
/// @note 整个目录移动后从新路径回读隐式采样，验证允许移动不等于丢弃引用保护。
bool testImdAudioMovePreflight()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldDirectory = directory.path() / "old";
    const auto newDirectory = directory.path() / "new";
    const auto audioPath    = oldDirectory / "Song.ogg";
    const auto renamedAudio = oldDirectory / "Renamed.ogg";
    // 谱面与 Song.ogg 同目录且共享名称前缀，构造隐式关联的正常输入。
    // Renamed.ogg 改变关联名称，整目录移动则不改变两者相对关系。
    const auto mapPath = oldDirectory / "Song_4k_Test.imd";
    if ( !createAudioPlaceholder(audioPath) ) return false;

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_track           = 4,
        .m_audioResourceId = "Song.ogg",
    });
    beatmap.sync();
    if ( !beatmap.saveToFile(mapPath) ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Song", "old/Song_4k_Test.imd", {} });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "stable-song-id",
                            .m_path = "old/Song.ogg",
                            .m_type = MMM::AudioTrackType::Main });

    // 单独音频改名预检应返回拒绝原因，源文件和项目表都仍处于原状态。
    // 预检阶段不能为了试探支持性先执行物理移动。
    const auto blocked =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, audioPath, renamedAudio);
    if ( blocked.empty() || !std::filesystem::exists(audioPath) ||
         std::filesystem::exists(renamedAudio) ||
         project.m_audioResources.front().m_path != "old/Song.ogg" ) {
        XERROR("Destructive RM/IMD audio rename was not blocked before move");
        return false;
    }

    // 同一输入随后检查整目录移动，防止实现把所有 IMD 相关移动一律禁止。
    // 允许条件关注关系保留，而不只是资源是否被引用。
    const auto allowed =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, oldDirectory, newDirectory);
    if ( !allowed.empty() ) {
        XERROR("Relationship-preserving RM/IMD directory move was rejected: {}",
               allowed);
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::rename(oldDirectory, newDirectory, filesystemError);
    if ( filesystemError ) return false;
    const auto changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldDirectory, newDirectory);
    if ( changed != 1U ||
         project.m_audioResources.front().m_path != "new/Song.ogg" ) {
        XERROR("Safe RM/IMD directory move did not update the resource path");
        return false;
    }

    // 从新目录实际加载谱面，隐式资源仍应解析为同名 Song.ogg。
    // 项目资源路径变化与谱面内部关联不变需要同时成立。
    const auto loaded =
        MMM::BeatMap::loadFromFile(newDirectory / mapPath.filename());
    if ( loaded.m_audioSamples.size() != 1U ||
         loaded.m_audioSamples.front().m_audioResourceId != "Song.ogg" ) {
        XERROR("Safe RM/IMD directory move changed its implicit audio");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行项目音频引用和资源约束测试。
/// @return 全部测试通过时返回 0。
/// @note 通过逻辑与串行运行，首个失败立即返回非零，后续用例不再执行。
/// @note 返回零表示全部列出的用例均到达成功出口，不存在跳过后的伪成功。
int main()
{
    // 各用例独立管理临时目录或内存项目，失败时析构仍清理已创建夹具。
    // 配置覆盖用例也恢复默认作者，后续测试不继承它的专用设置。
    return testLegacyBeatmapEntryIsReadOnly() &&
                   testReferenceAwareDirectoryScan() &&
                   testAudioResolutionPreservesCrossModeFirstMatch() &&
                   testRootBeatmapEscapedLegacyReferenceFallback() &&
                   testBulkReferenceIndexPreservesCompatibility() &&
                   testBulkDirectorySyncReusesNormalizedResources() &&
                   testAudioReferenceMutationGuards() &&
                   testOpenBeatmapReferencesSupplementDiskGuards() &&
                   testPhysicalAudioDeletionHonorsReferences() &&
                   testInMemoryAudioReferenceRemapResult() &&
                   testSongFileHintSaveSemantics() &&
                   testTemplateAudioSampleTrackRemap() &&
                   testImdStyleTemplatePolylineCopyAvoidsDuplicateChildren() &&
                   testInvalidTemplateObjectTracksAreRejectedAtomically() &&
                   testCreateBeatmapMaterializesMainSample() &&
                   testDefaultBeatmapAudioResolution() &&
                   testLegacyProjectAudioTrackMigration() &&
                   testAudioResourcePathRemap() &&
                   testAudioResourceIdRenameTransaction() &&
                   testAudioResourceIdRenameTransactionRollback() &&
                   testOsuAudioReferenceMoveRemap() &&
                   testOsuBeatmapOnlyMoveRemap() &&
                   testOsuMoveWriteFailureRollsBack() &&
                   testImdAudioMovePreflight()
               ? 0
               : 1;
}
