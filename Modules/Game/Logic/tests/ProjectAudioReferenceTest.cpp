#include "logic/ProjectCommandService.h"
#include "logic/ProjectResourceService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

/// @brief 读取测试文本文件并保留全部原始内容。
/// @param path 待读取文件。
/// @param output 文件内容。
/// @return 完整读取成功时返回 true。
bool readTextFile(const std::filesystem::path& path, std::string& output)
{
    std::ifstream stream(path, std::ios::binary);
    if ( !stream.is_open() ) return false;
    output.assign(std::istreambuf_iterator<char>(stream),
                  std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
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

/// @brief 验证打开会话的内存谱面引用会补充磁盘扫描结果。
/// @return 未落盘的 Note 和自动采样引用都能阻止破坏性操作时返回 true。
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

    MMM::BeatMap openBeatmap;
    MMM::Note    note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "live-note", 1.0F });
    openBeatmap.m_noteData.notes.push_back(std::move(note));
    openBeatmap.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_audioResourceId = "live-sample" });

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
    if ( updateResult.m_updated ||
         updateResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "charts/OpenOnly.mmm" } ) {
        XERROR("Open-session Note binding did not block Effect-to-Main");
        return false;
    }

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
    project.m_projectRoot    = directory.path();
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
    if ( blockedResult.m_removed ||
         blockedResult.m_blockingBeatmapPaths !=
             std::vector<std::string>{ "charts/OpenOnly.mmm" } ||
         !std::filesystem::exists(usedPath) ||
         findResource(project, "used-resource") == nullptr ) {
        XERROR("Referenced source audio was physically deleted");
        return false;
    }

    const auto removedResult = service.removeAudioResource(
        project, MMM::Logic::CmdRemoveAudioResource{ "unused-resource", true });
    if ( !removedResult.m_removed || !removedResult.m_errorMessage.empty() ||
         std::filesystem::exists(unusedPath) ||
         findResource(project, "unused-resource") != nullptr ) {
        XERROR("Unreferenced source audio was not deleted atomically");
        return false;
    }

    const auto missingResult = service.removeAudioResource(
        project,
        MMM::Logic::CmdRemoveAudioResource{ "missing-resource", true });
    if ( missingResult.m_removed || missingResult.m_errorMessage.empty() ||
         findResource(project, "missing-resource") == nullptr ) {
        XERROR("Missing source audio removed its project resource entry");
        return false;
    }
    return true;
}

/// @brief 验证内存谱面移动引用重映射会报告匹配并稳定改写字段。
/// @return Note、自动采样和歌曲提示全部按约定更新时返回 true。
bool testInMemoryAudioReferenceRemapResult()
{
    MMM::Project project;
    project.m_projectRoot = "/tmp/mmm-open-reference-remap";

    const MMM::AudioResource previousResource{
        .m_id   = "stable-audio-id",
        .m_path = "old/song.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.song_file_hint  = "old/song.wav";
    beatmap.m_baseMapMetadata.main_audio_path = "old/song.wav";
    MMM::Note note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "old/song.wav", 0.75F });
    beatmap.m_noteData.notes.push_back(std::move(note));
    beatmap.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_audioResourceId = "old/song.wav" });

    const auto result = MMM::Logic::ProjectResourceService::
        remapBeatmapAudioReferencesAfterMove(
            project, beatmap, "Open.mmm", previousResource, "new/song.wav");
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
    beatmap.m_audioSamples                    = {
        MMM::AudioSampleEvent{ .m_timestamp       = -1000.0,
                               .m_audioResourceId = "effect-hint" },
        MMM::AudioSampleEvent{ .m_timestamp       = 1000.0,
                               .m_offsetMs        = -100,
                               .m_audioResourceId = "main-late" },
        MMM::AudioSampleEvent{ .m_timestamp       = 800.0,
                               .m_offsetMs        = -300,
                               .m_audioResourceId = "main-early" },
    };
    const auto sampleCount = beatmap.m_audioSamples.size();

    auto result =
        MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
            project, beatmap, beatmap.m_baseMapMetadata.map_path);
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
bool testTemplateAudioSampleTrackRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 4;
    source->m_baseMapMetadata.bgm_track_count = 5;
    source->m_audioSamples                    = {
        MMM::AudioSampleEvent{ .m_track = 4, .m_audioResourceId = "first-bgm" },
        MMM::AudioSampleEvent{ .m_track = 6, .m_audioResourceId = "third-bgm" },
    };
    MMM::Note sourceNote;
    sourceNote.m_track = 3;
    source->m_noteData.notes.push_back(std::move(sourceNote));
    source->sync();

    MMM::Logic::CmdCreateBeatmap preserveCommand;
    preserveCommand.baseMeta.name               = "TemplatePreserve";
    preserveCommand.baseMeta.version            = "TemplatePreserve";
    preserveCommand.baseMeta.track_count        = 6;
    preserveCommand.baseMeta.bgm_track_count    = 1;
    preserveCommand.templateBeatmap             = source;
    preserveCommand.templateOptions.copyObjects = true;

    const auto preserveResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          preserveCommand);
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

    const auto persisted =
        MMM::BeatMap::loadFromFile(directory.path() / "TemplatePreserve.mmm");
    if ( persisted.m_audioSamples.size() != 2 ||
         persisted.m_audioSamples[0].m_track != 6 ||
         persisted.m_audioSamples[1].m_track != 8 ||
         persisted.m_baseMapMetadata.bgm_track_count != 5 ) {
        XERROR("Remapped template sample tracks were not persisted");
        return false;
    }

    source->m_baseMapMetadata.bgm_track_count  = 1;
    MMM::Logic::CmdCreateBeatmap expandCommand = preserveCommand;
    expandCommand.baseMeta.name                = "TemplateExpand";
    expandCommand.baseMeta.version             = "TemplateExpand";
    const auto expandResult = MMM::Logic::ProjectCommandService{}.createBeatmap(
        project, expandCommand);
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

/// @brief 验证非法模板物件不会产生部分谱面、文件或项目资源。
/// @return 越界玩家列、落入玩家区的采样和轨道溢出均被原子拒绝时返回 true。
bool testInvalidTemplateObjectTracksAreRejectedAtomically()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    MMM::Project project;
    project.m_projectRoot = directory.path();
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "Existing", "Existing.mmm", {} });

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
    playerLaneCommand.baseMeta.name               = "RejectedPlayerLane";
    playerLaneCommand.baseMeta.version            = "RejectedPlayerLane";
    playerLaneCommand.baseMeta.track_count        = 6;
    playerLaneCommand.baseMeta.song_file_hint     = "pending-main.ogg";
    playerLaneCommand.templateBeatmap             = playerLaneSource;
    playerLaneCommand.templateOptions.copyObjects = true;

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

    const auto overflowResult =
        MMM::Logic::ProjectCommandService{}.createBeatmap(project,
                                                          overflowCommand);
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
    command.baseMeta.name           = "Created";
    command.baseMeta.version        = "Created";
    command.baseMeta.track_count    = 4;
    command.baseMeta.song_file_hint = "audio/song.ogg";

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
             std::filesystem::path("audio/song.ogg") ||
         !result.m_beatmap->m_baseMapMetadata.main_audio_path.empty() ) {
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

/// @brief 验证 osu! 移动只原位改写音频字段而不重排或丢失其它文本。
/// @return 全局音频和 HitSample 引用更新且哨兵文本保留时返回 true。
bool testOsuAudioReferenceMoveRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldDirectory = directory.path() / "old";
    const auto newDirectory = directory.path() / "new";
    const auto mainAudio    = oldDirectory / "main.ogg";
    const auto effectAudio  = oldDirectory / "C:effect.wav";
    const auto mapPath      = directory.path() / "charts" / "Move.osu";
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
    note.m_timestamp = 1000.0;
    note.m_track     = 1;
    note.setSampleBinding(
        MMM::AudioSampleBinding{ "../old/C:effect.wav", 1.0F });
    beatmap.m_noteData.notes.push_back(std::move(note));
    beatmap.sync();
    if ( !beatmap.saveToFile(mapPath) ) return false;

    {
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
    if ( changed != 2U ||
         project.m_audioResources[0].m_path != "new/main.ogg" ||
         project.m_audioResources[1].m_path != "new/C:effect.wav" ) {
        XERROR("Moved osu! resources were not updated in the project");
        return false;
    }

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
bool testOsuBeatmapOnlyMoveRemap()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto audioPath  = directory.path() / "audio" / "main.ogg";
    const auto oldMapPath = directory.path() / "charts" / "Move.osu";
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
    std::filesystem::rename(oldMapPath, newMapPath, filesystemError);
    if ( filesystemError ) return false;

    std::string errorMessage;
    const auto  changed =
        MMM::Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            project, oldMapPath, newMapPath, &errorMessage);
    if ( changed != 0U || !errorMessage.empty() ||
         project.m_audioResources.front().m_path != "audio/main.ogg" ) {
        XERROR("Moving only an osu! beatmap changed its audio resource");
        return false;
    }

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

    const auto validation =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, oldAudio, newAudio);
    if ( !validation.empty() ) return false;

    std::filesystem::create_directories(newAudio.parent_path(),
                                        filesystemError);
    if ( filesystemError ) return false;
    std::filesystem::rename(oldAudio, newAudio, filesystemError);
    if ( filesystemError ) return false;

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
    std::string finalText;
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
bool testImdAudioMovePreflight()
{
    ScopedTestProjectDirectory directory;
    if ( directory.path().empty() ) return false;

    const auto oldDirectory = directory.path() / "old";
    const auto newDirectory = directory.path() / "new";
    const auto audioPath    = oldDirectory / "Song.ogg";
    const auto renamedAudio = oldDirectory / "Renamed.ogg";
    const auto mapPath      = oldDirectory / "Song_4k_Test.imd";
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

    const auto blocked =
        MMM::Logic::ProjectResourceService::validateAudioResourceMove(
            project, audioPath, renamedAudio);
    if ( blocked.empty() || !std::filesystem::exists(audioPath) ||
         std::filesystem::exists(renamedAudio) ||
         project.m_audioResources.front().m_path != "old/Song.ogg" ) {
        XERROR("Destructive RM/IMD audio rename was not blocked before move");
        return false;
    }

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
int main()
{
    return testLegacyBeatmapEntryIsReadOnly() &&
                   testReferenceAwareDirectoryScan() &&
                   testAudioReferenceMutationGuards() &&
                   testOpenBeatmapReferencesSupplementDiskGuards() &&
                   testPhysicalAudioDeletionHonorsReferences() &&
                   testInMemoryAudioReferenceRemapResult() &&
                   testSongFileHintSaveSemantics() &&
                   testTemplateAudioSampleTrackRemap() &&
                   testInvalidTemplateObjectTracksAreRejectedAtomically() &&
                   testCreateBeatmapMaterializesMainSample() &&
                   testDefaultBeatmapAudioResolution() &&
                   testLegacyProjectAudioTrackMigration() &&
                   testAudioResourcePathRemap() &&
                   testOsuAudioReferenceMoveRemap() &&
                   testOsuBeatmapOnlyMoveRemap() &&
                   testOsuMoveWriteFailureRollsBack() &&
                   testImdAudioMovePreflight()
               ? 0
               : 1;
}
