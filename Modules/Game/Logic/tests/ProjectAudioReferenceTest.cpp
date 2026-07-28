#include "logic/ProjectCommandService.h"
#include "logic/ProjectResourceService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

/// @brief 为单次测试创建并清理隔离的项目目录。
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

        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = baseDirectory /
                 ("mmm-project-audio-reference-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, filesystemError);
        if ( filesystemError ) m_path.clear();
    }

    /// @brief 清理本测试创建的隔离目录。
    ~ScopedTestProjectDirectory()
    {
        if ( m_path.empty() ) return;
        std::error_code filesystemError;
        std::filesystem::remove_all(m_path, filesystemError);
    }

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

/// @brief 创建目录扫描所需的最小音频占位文件。
/// @param path 待创建文件路径。
/// @return 文件成功创建时返回 true。
bool createAudioPlaceholder(const std::filesystem::path& path)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    std::ofstream stream(path, std::ios::binary);
    stream.put('\0');
    return stream.good();
}

/// @brief 按 ID 查找项目音频资源。
/// @param project 待查询项目。
/// @param id 资源 ID。
/// @return 找到时返回资源地址，否则返回空。
const MMM::AudioResource* findResource(const MMM::Project& project,
                                       const std::string&  id)
{
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
bool saveReferenceBeatmap(const std::filesystem::path& path,
                          const std::filesystem::path& songHint,
                          const std::string&           noteAudioRef,
                          const std::string&           sampleAudioRef)
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name            = "Reference";
    beatmap.m_baseMapMetadata.version         = "Reference";
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 1;
    beatmap.m_baseMapMetadata.song_file_hint  = songHint;

    if ( !noteAudioRef.empty() ) {
        MMM::Note note;
        note.setSampleBinding(MMM::AudioSampleBinding{ noteAudioRef, 0.75F });
        beatmap.m_noteData.notes.push_back(std::move(note));
    }
    if ( !sampleAudioRef.empty() ) {
        MMM::AudioSampleEvent sample;
        sample.m_track           = 4;
        sample.m_audioResourceId = sampleAudioRef;
        beatmap.m_audioSamples.push_back(std::move(sample));
    }
    beatmap.sync();
    return beatmap.saveToFile(path);
}

/// @brief 验证旧单主音轨字段只读兼容且当前项目不再写出该字段。
/// @return 兼容行为正确时返回 true。
bool testLegacyBeatmapEntryIsReadOnly()
{
    const nlohmann::json legacyJson{ { "m_name", "Legacy" },
                                     { "m_filePath", "Legacy.mmm" },
                                     { "m_audioTrackId", "legacy.ogg" } };
    const auto           entry = legacyJson.get<MMM::Project::BeatmapEntry>();
    if ( entry.m_audioTrackId != "legacy.ogg" ) {
        XERROR("Legacy BeatmapEntry audio track ID was not readable");
        return false;
    }

    const nlohmann::json currentJson = entry;
    if ( currentJson.contains("m_audioTrackId") ) {
        XERROR("Current BeatmapEntry still persisted m_audioTrackId");
        return false;
    }
    return true;
}

/// @brief 验证目录扫描使用完整谱面引用推断类型且允许零主音轨。
/// @return 扫描分类行为正确时返回 true。
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
    if ( !shared || !main || !sample || !unused ||
         shared->m_type != MMM::AudioTrackType::Effect ||
         main->m_type != MMM::AudioTrackType::Main ||
         sample->m_type != MMM::AudioTrackType::Effect ||
         unused->m_type != MMM::AudioTrackType::Effect ) {
        XERROR("Reference-aware audio type inference was incorrect");
        return false;
    }
    if ( std::any_of(project.m_beatmaps.begin(),
                     project.m_beatmaps.end(),
                     [](const MMM::Project::BeatmapEntry& entry) {
                         return !entry.m_audioTrackId.empty();
                     }) ) {
        XERROR("Directory scan still authored legacy m_audioTrackId values");
        return false;
    }

    MMM::Project noMainProject;
    noMainProject.m_projectRoot = directory.path();
    MMM::Logic::ProjectDirectoryScanner::ScanResult noMainScan;
    noMainScan.m_success    = true;
    noMainScan.m_audioFiles = { unusedAudio };
    MMM::Logic::ProjectResourceService{}.buildInitialResources(noMainProject,
                                                               noMainScan);
    return noMainProject.m_audioResources.size() == 1 &&
           noMainProject.m_audioResources.front().m_type ==
               MMM::AudioTrackType::Effect;
}

/// @brief 验证资源改类型和删除时不会破坏谱面引用。
/// @return 引用约束正确时返回 true。
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

    MMM::Logic::ProjectCommandService service;
    auto                              updateResult =
        service.updateAudioResource(project,
                                    MMM::Logic::CmdUpdateAudioResource{
                                        "shared.wav",
                                        MMM::AudioTrackType::Main,
                                    });
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

    const auto removeHint = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "hint.wav" });
    if ( !removeHint.m_removed || !removeHint.m_blockingBeatmapPaths.empty() ) {
        XERROR("song_file_hint incorrectly blocked resource removal");
        return false;
    }

    const auto removeUnused = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "unused.wav" });
    if ( !removeUnused.m_removed ||
         findResource(project, "unused.wav") != nullptr ) {
        XERROR("Unreferenced audio resource could not be removed");
        return false;
    }
    return true;
}

/// @brief 验证新建谱面把所选 Main 物化为第一条 BGM 轨的自动采样。
/// @return 新建谱面行为正确时返回 true。
bool testCreateBeatmapMaterializesMainSample()
{
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
    command.baseMeta.name            = "Created";
    command.baseMeta.version         = "Created";
    command.baseMeta.track_count     = 4;
    command.baseMeta.main_audio_path = "audio/song.ogg";

    const auto result =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project, command);
    if ( !result.m_created || !result.m_beatmap ||
         result.m_beatmap->m_audioSamples.size() != 1 ) {
        XERROR("New beatmap did not materialize selected Main audio");
        return false;
    }

    const auto& sample = result.m_beatmap->m_audioSamples.front();
    if ( sample.m_audioResourceId != "song-resource" ||
         sample.m_timestamp != 0.0 || sample.m_offsetMs != 0 ||
         sample.m_track != 4 ||
         result.m_beatmap->m_baseMapMetadata.bgm_track_count < 1 ||
         result.m_beatmap->m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/song.ogg") ) {
        XERROR("Materialized Main sample fields were incorrect");
        return false;
    }
    if ( project.m_beatmaps.size() != 1 ||
         !project.m_beatmaps.front().m_audioTrackId.empty() ) {
        XERROR("New beatmap entry still authored a legacy audio track ID");
        return false;
    }

    const nlohmann::json projectJson = project;
    return !projectJson["m_beatmaps"][0].contains("m_audioTrackId");
}

/// @brief 验证活动谱面默认音频解析优先使用提示和 Main 自动采样。
/// @return 默认资源选择符合预期时返回 true。
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

    MMM::BeatMap hintedBeatmap;
    hintedBeatmap.m_baseMapMetadata.song_file_hint = "audio/effect.wav";
    const auto* hinted =
        MMM::Logic::ProjectResourceService::findDefaultBeatmapAudioResource(
            project, hintedBeatmap, "Hinted.mmm");
    if ( !hinted || hinted->m_id != "effect.wav" ) {
        XERROR("Beatmap song_file_hint was not preferred");
        return false;
    }

    MMM::BeatMap          sampleBeatmap;
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

    MMM::Project persistedProject;
    persistedProject.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Legacy", "Legacy.mmm", "song.ogg" });

    MMM::Logic::ProjectResourceService service;
    const auto                         migration =
        service.migrateLegacyBeatmapAudioTracks(project, persistedProject);
    if ( migration.m_migratedBeatmapCount != 1 ||
         !migration.m_failedBeatmapPaths.empty() ) {
        XERROR("Legacy project audio track migration did not complete");
        return false;
    }

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
    if ( project.m_audioResources.front().m_type !=
         MMM::AudioTrackType::Main ) {
        XERROR("Migrated legacy main resource did not retain Main type");
        return false;
    }

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
    std::filesystem::rename(oldDirectory, newDirectory, filesystemError);
    if ( filesystemError ) return false;

    const auto changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldDirectory, newDirectory);
    if ( changed != 1 ||
         project.m_audioResources.front().m_id != "stable-song-id" ||
         project.m_audioResources.front().m_path != "new/song.ogg" ) {
        XERROR("Moved audio resource path was not remapped with a stable ID");
        return false;
    }

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

}  // namespace

/// @brief 运行项目音频引用和资源约束测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testLegacyBeatmapEntryIsReadOnly() &&
                   testReferenceAwareDirectoryScan() &&
                   testAudioReferenceMutationGuards() &&
                   testCreateBeatmapMaterializesMainSample() &&
                   testDefaultBeatmapAudioResolution() &&
                   testLegacyProjectAudioTrackMigration() &&
                   testAudioResourcePathRemap()
               ? 0
               : 1;
}
