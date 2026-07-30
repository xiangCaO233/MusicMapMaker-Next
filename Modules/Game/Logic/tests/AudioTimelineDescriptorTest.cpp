#include "logic/audio/AudioTimelineDescriptor.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view BEATMAP_PATH = "charts/descriptor.mmm";

/// @brief 构造覆盖 Main、Effect 和高级配置的测试项目。
/// @return 使用绝对临时根目录的项目。
MMM::Project makeProject()
{
    std::error_code filesystemError;
    auto root = std::filesystem::temp_directory_path(filesystemError);
    if ( filesystemError ) {
        filesystemError.clear();
        root = std::filesystem::current_path(filesystemError);
    }

    MMM::Project project;
    project.m_projectRoot = root / "mmm-audio-timeline-descriptor-test";

    MMM::AudioTrackConfig mainConfig;
    mainConfig.volume        = 0.8F;
    mainConfig.playbackSpeed = 1.15F;
    mainConfig.playbackPitch = -1.5F;
    mainConfig.muted         = false;
    mainConfig.eqEnabled     = true;
    mainConfig.eqPreset      = 2;
    mainConfig.eqBandGains   = { 1.0F, -2.0F };
    mainConfig.eqBandQs      = { 0.7F, 1.2F };

    MMM::AudioTrackConfig effectConfig;
    effectConfig.volume        = 0.45F;
    effectConfig.playbackSpeed = 0.9F;
    effectConfig.playbackPitch = 2.0F;
    effectConfig.eqBandGains   = { 3.0F };
    effectConfig.eqBandQs      = { 0.85F };

    project.m_audioResources = {
        MMM::AudioResource{
            .m_id     = "main-id",
            .m_path   = "audio/main.ogg",
            .m_type   = MMM::AudioTrackType::Main,
            .m_config = mainConfig,
        },
        MMM::AudioResource{
            .m_id     = "effect-id",
            .m_path   = "audio/effect.wav",
            .m_type   = MMM::AudioTrackType::Effect,
            .m_config = effectConfig,
        },
    };
    return project;
}

/// @brief 构造单个自动采样测试对象。
/// @param timestamp 锚点时间，单位毫秒。
/// @param offsetMs 有符号偏移，单位毫秒。
/// @param track 纯视觉统一轨道索引。
/// @param audioReference 项目资源 ID 或旧路径。
/// @param volume 物件音量。
/// @return 自动采样对象。
MMM::AudioSampleEvent makeSample(double timestamp, std::int64_t offsetMs,
                                 std::uint32_t      track,
                                 const std::string& audioReference,
                                 float              volume)
{
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = timestamp;
    sample.m_offsetMs        = offsetMs;
    sample.m_track           = track;
    sample.m_audioResourceId = audioReference;
    sample.m_volume          = volume;
    return sample;
}

/// @brief 构造包含重复采样、正负 offset 和缺失引用的测试谱面。
/// @param reverseOrder 是否反转输入采样顺序。
/// @return 测试谱面。
MMM::BeatMap makeBeatMap(bool reverseOrder)
{
    MMM::BeatMap beatMap;
    beatMap.m_baseMapMetadata.track_count     = 4;
    beatMap.m_baseMapMetadata.bgm_track_count = 96;
    beatMap.m_baseMapMetadata.song_file_hint  = "ignored-song-hint.ogg";

    std::vector<MMM::AudioSampleEvent> samples{
        makeSample(1000.0, 250, 4, "main-id", 0.6F),
        makeSample(2500.0, -500, 8, "audio/effect.wav", 0.75F),
        makeSample(100.0, -600, 1000, "missing.wav", 0.4F),
        makeSample(1000.0, 250, 27, "audio/main.ogg", 0.6F),
    };
    if ( reverseOrder ) std::reverse(samples.begin(), samples.end());
    for ( auto& sample : samples ) {
        beatMap.m_audioSamples.push_back(std::move(sample));
    }

    MMM::Note ignoredNote;
    ignoredNote.setSampleBinding(MMM::AudioSampleBinding{ "effect-id", 0.2F });
    beatMap.m_noteData.notes.push_back(std::move(ignoredNote));
    return beatMap;
}

/// @brief 比较两个完整音轨配置。
/// @param lhs 左侧配置。
/// @param rhs 右侧配置。
/// @return 所有持久化字段均相同时返回 true。
bool sameConfig(const MMM::AudioTrackConfig& lhs,
                const MMM::AudioTrackConfig& rhs)
{
    return lhs.volume == rhs.volume && lhs.playbackSpeed == rhs.playbackSpeed &&
           lhs.playbackPitch == rhs.playbackPitch && lhs.muted == rhs.muted &&
           lhs.eqEnabled == rhs.eqEnabled && lhs.eqPreset == rhs.eqPreset &&
           lhs.eqBandGains == rhs.eqBandGains && lhs.eqBandQs == rhs.eqBandQs;
}

/// @brief 比较两个 AudioManager 加载事件的完整字段。
/// @param lhs 左侧事件。
/// @param rhs 右侧事件。
/// @return 事件 ID 和全部听觉字段均相同时返回 true。
bool sameLoadEvent(const MMM::Audio::AudioTimelineLoadEvent& lhs,
                   const MMM::Audio::AudioTimelineLoadEvent& rhs)
{
    return lhs.eventId == rhs.eventId && lhs.resourceKey == rhs.resourceKey &&
           lhs.filePath == rhs.filePath &&
           lhs.effectiveStartSeconds == rhs.effectiveStartSeconds &&
           lhs.bgmTrackIndex == rhs.bgmTrackIndex &&
           lhs.eventVolume == rhs.eventVolume &&
           sameConfig(lhs.resourceConfig, rhs.resourceConfig);
}

/// @brief 比较两个规范事件序列。
/// @param lhs 左侧事件序列。
/// @param rhs 右侧事件序列。
/// @return 顺序和全部事件字段均相同时返回 true。
bool sameLoadEvents(const std::vector<MMM::Audio::AudioTimelineLoadEvent>& lhs,
                    const std::vector<MMM::Audio::AudioTimelineLoadEvent>& rhs)
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), sameLoadEvent);
}

/// @brief 按资源键查找首个加载事件。
/// @param descriptor 待查询描述符。
/// @param resourceKey 项目资源键。
/// @return 找到时返回事件地址，否则返回空。
const MMM::Audio::AudioTimelineLoadEvent* findEvent(
    const MMM::Logic::AudioTimelineDescriptor& descriptor,
    const std::string&                         resourceKey)
{
    const auto iterator =
        std::find_if(descriptor.m_events.begin(),
                     descriptor.m_events.end(),
                     [&](const MMM::Audio::AudioTimelineLoadEvent& event) {
                         return event.resourceKey == resourceKey;
                     });
    return iterator == descriptor.m_events.end() ? nullptr : &*iterator;
}

/// @brief 验证规范排序、资源解析、重复事件和正负 offset。
/// @return 描述符保留全部自动采样听觉语义时返回 true。
bool testCanonicalDescriptor()
{
    const auto project    = makeProject();
    const auto beatMap    = makeBeatMap(false);
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        12.5);

    if ( descriptor.m_events.size() != 4U ||
         descriptor.m_diagnostics.size() != 1U ||
         descriptor.m_fingerprint.size() != 32U ||
         descriptor.m_chartEndSeconds != 12.5 ) {
        XERROR("Audio timeline descriptor did not retain all sample events");
        return false;
    }

    if ( descriptor.m_events[0].resourceKey != "missing.wav" ||
         descriptor.m_events[0].filePath != "" ||
         std::abs(descriptor.m_events[0].effectiveStartSeconds + 0.5) >
             1.0e-9 ||
         descriptor.m_events[0].bgmTrackIndex != 996U ||
         descriptor.m_events[1].resourceKey != "main-id" ||
         descriptor.m_events[2].resourceKey != "main-id" ||
         descriptor.m_events[1].bgmTrackIndex != 0U ||
         descriptor.m_events[2].bgmTrackIndex != 23U ||
         std::abs(descriptor.m_events[1].effectiveStartSeconds - 1.25) >
             1.0e-9 ||
         std::abs(descriptor.m_events[2].effectiveStartSeconds - 1.25) >
             1.0e-9 ||
         descriptor.m_events[3].resourceKey != "effect-id" ||
         descriptor.m_events[3].bgmTrackIndex != 4U ||
         std::abs(descriptor.m_events[3].effectiveStartSeconds - 2.0) >
             1.0e-9 ) {
        XERROR("Audio timeline descriptor was not canonically time-sorted");
        return false;
    }

    if ( descriptor.m_events[1].eventId == descriptor.m_events[2].eventId ||
         descriptor.m_events[1].eventId == 0U ||
         descriptor.m_events[2].eventId == 0U ) {
        XERROR("Duplicate audio tuples did not receive unique stable IDs");
        return false;
    }

    const auto* mainEvent   = findEvent(descriptor, "main-id");
    const auto* effectEvent = findEvent(descriptor, "effect-id");
    if ( !mainEvent || !effectEvent ||
         !MMM::Config::utf8ToPath(mainEvent->filePath).is_absolute() ||
         !MMM::Config::utf8ToPath(effectEvent->filePath).is_absolute() ||
         !sameConfig(mainEvent->resourceConfig,
                     project.m_audioResources[0].m_config) ||
         !sameConfig(effectEvent->resourceConfig,
                     project.m_audioResources[1].m_config) ) {
        XERROR("Main or Effect resources were not fully resolved");
        return false;
    }

    const auto& diagnostic = descriptor.m_diagnostics.front();
    if ( diagnostic.m_code !=
             MMM::Logic::AudioTimelineDescriptorDiagnosticCode::
                 UnresolvedAudioResource ||
         diagnostic.m_eventId != descriptor.m_events.front().eventId ||
         diagnostic.m_audioReference != "missing.wav" ||
         diagnostic.m_message.empty() ) {
        XERROR("Missing audio reference diagnostic was incomplete");
        return false;
    }
    return true;
}

/// @brief 验证输入排列不影响规范顺序、事件 ID 和指纹。
/// @return 反序输入得到完全相同描述符时返回 true。
bool testOrderIndependentIdentity()
{
    const auto project           = makeProject();
    const auto forward           = makeBeatMap(false);
    const auto reversed          = makeBeatMap(true);
    const auto forwardDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        forward,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        8.0);
    const auto reversedDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        reversed,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        8.0);

    if ( forwardDescriptor.m_fingerprint != reversedDescriptor.m_fingerprint ||
         !sameLoadEvents(forwardDescriptor.m_events,
                         reversedDescriptor.m_events) ) {
        XERROR("Audio descriptor identity depended on source event order");
        return false;
    }
    return true;
}

/// @brief 验证玩家轨道数量变化后保持相对 BGM 轨道时指纹不变。
/// @return 仅绝对画布索引迁移及非自动采样字段变化时指纹保持不变。
bool testNonAudioFieldsAreExcluded()
{
    const auto project                        = makeProject();
    const auto baseline                       = makeBeatMap(false);
    auto       changed                        = makeBeatMap(false);
    changed.m_baseMapMetadata.track_count     = 9;
    changed.m_baseMapMetadata.bgm_track_count = 2048;
    changed.m_baseMapMetadata.song_file_hint  = "another-hint.wav";
    for ( auto& sample : changed.m_audioSamples ) {
        sample.m_track += 5U;
    }
    changed.m_noteData.notes.front().setSampleBinding(
        MMM::AudioSampleBinding{ "main-id", 0.95F });

    const auto baselineDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        baseline,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        9.0);
    const auto changedDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        changed,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        9.0);
    if ( baselineDescriptor.m_fingerprint != changedDescriptor.m_fingerprint ||
         !sameLoadEvents(baselineDescriptor.m_events,
                         changedDescriptor.m_events) ) {
        XERROR("Absolute canvas track migration leaked into audio fingerprint");
        return false;
    }
    return true;
}

/// @brief 验证全部资源配置、物件音量和谱面结束时间参与指纹。
/// @return 任一听觉语义变化均产生不同指纹。
bool testFingerprintSensitivity()
{
    const auto baselineProject = makeProject();
    const auto baselineMap     = makeBeatMap(false);
    const auto baseline        = MMM::Logic::buildAudioTimelineDescriptor(
        baselineMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);

    /// @brief 验证单个资源配置修改能够改变指纹。
    const auto configChangesFingerprint = [&](auto&&      mutate,
                                              const char* fieldName) {
        auto project = makeProject();
        mutate(project.m_audioResources.front().m_config);
        const auto beatMap = makeBeatMap(false);
        const auto changed = MMM::Logic::buildAudioTimelineDescriptor(
            beatMap,
            project,
            MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
            10.0);
        if ( changed.m_fingerprint == baseline.m_fingerprint ) {
            XERROR("Audio config field '{}' was absent from fingerprint",
                   fieldName);
            return false;
        }
        return true;
    };

    if ( !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.volume += 0.01F; },
             "volume") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.playbackSpeed += 0.01F;
             },
             "playbackSpeed") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.playbackPitch += 0.25F;
             },
             "playbackPitch") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.muted = true; },
             "muted") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.eqEnabled = false; },
             "eqEnabled") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { ++config.eqPreset; },
             "eqPreset") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.eqBandGains.push_back(4.0F);
             },
             "eqBandGains") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.eqBandQs.push_back(1.5F);
             },
             "eqBandQs") ) {
        return false;
    }

    auto volumeMap = makeBeatMap(false);
    volumeMap.m_audioSamples.front().m_volume += 0.1F;
    const auto volumeChanged = MMM::Logic::buildAudioTimelineDescriptor(
        volumeMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    const auto chartEndChanged = MMM::Logic::buildAudioTimelineDescriptor(
        baselineMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.5);
    auto bgmTrackMap = makeBeatMap(false);
    ++bgmTrackMap.m_audioSamples.front().m_track;
    const auto bgmTrackChanged = MMM::Logic::buildAudioTimelineDescriptor(
        bgmTrackMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    if ( volumeChanged.m_fingerprint == baseline.m_fingerprint ||
         chartEndChanged.m_fingerprint == baseline.m_fingerprint ||
         bgmTrackChanged.m_fingerprint == baseline.m_fingerprint ) {
        XERROR(
            "Event volume, BGM track routing or chart end was absent from "
            "fingerprint");
        return false;
    }
    return true;
}

/// @brief 验证资源配置失效筛选只匹配自动采样的规范资源 ID。
/// @return Main、Effect 和缺失引用可匹配，Note 绑定与别名路径不会误匹配。
bool testDescriptorResourceReferenceLookup()
{
    const auto project    = makeProject();
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        makeBeatMap(false),
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    if ( !MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                                "main-id") ||
         !MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                                "effect-id") ||
         !MMM::Logic::audioTimelineDescriptorReferencesResource(
             descriptor, "missing.wav") ||
         MMM::Logic::audioTimelineDescriptorReferencesResource(
             descriptor, "audio/main.ogg") ||
         MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                               "") ) {
        XERROR("Audio timeline resource reference lookup was incorrect");
        return false;
    }

    MMM::BeatMap noteOnlyMap;
    MMM::Note    note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "effect-id", 1.0F });
    noteOnlyMap.m_noteData.notes.push_back(std::move(note));
    const auto noteOnlyDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        noteOnlyMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    return !MMM::Logic::audioTimelineDescriptorReferencesResource(
        noteOnlyDescriptor, "effect-id");
}

/// @brief 验证大量重复 v2 ID 事件复用首次出现的资源索引结果。
///
/// 资源和事件数量同时扩大，使测试输入能够暴露逐事件线性扫描退化；
/// 断言不依赖机器耗时，只验证首次同 ID 资源及全部事件解析结果。
/// @return 全部事件均解析到首次同 ID 资源时返回 true。
bool testBulkStableIdResolutionUsesFirstResource()
{
    constexpr std::size_t FILLER_RESOURCE_COUNT = 256U;
    constexpr std::size_t EVENT_COUNT           = 512U;

    auto project = makeProject();
    project.m_audioResources.clear();
    project.m_audioResources.reserve(FILLER_RESOURCE_COUNT + 2U);
    for ( std::size_t index = 0U; index < FILLER_RESOURCE_COUNT; ++index ) {
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = "filler-" + std::to_string(index),
            .m_path = "audio/filler-" + std::to_string(index) + ".wav",
            .m_type = MMM::AudioTrackType::Effect,
        });
    }

    MMM::AudioTrackConfig firstConfig;
    firstConfig.volume        = 0.37F;
    firstConfig.playbackSpeed = 1.25F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "repeated-id",
        .m_path   = "audio/repeated-first.wav",
        .m_type   = MMM::AudioTrackType::Effect,
        .m_config = firstConfig,
    });

    MMM::AudioTrackConfig duplicateConfig;
    duplicateConfig.volume = 0.91F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "repeated-id",
        .m_path   = "audio/repeated-second.wav",
        .m_type   = MMM::AudioTrackType::Main,
        .m_config = duplicateConfig,
    });

    MMM::BeatMap beatMap;
    for ( std::size_t index = 0U; index < EVENT_COUNT; ++index ) {
        beatMap.m_audioSamples.push_back(
            makeSample(static_cast<double>(index), 0, 4, "repeated-id", 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    if ( descriptor.m_events.size() != EVENT_COUNT ||
         !descriptor.m_diagnostics.empty() ) {
        XERROR("Bulk stable ID events were not all resolved");
        return false;
    }

    const auto expectedFilename =
        MMM::Config::utf8ToPath("audio/repeated-first.wav").filename();
    const auto invalidEvent = std::find_if(
        descriptor.m_events.begin(),
        descriptor.m_events.end(),
        [&](const MMM::Audio::AudioTimelineLoadEvent& event) {
            return event.resourceKey != "repeated-id" ||
                   MMM::Config::utf8ToPath(event.filePath).filename() !=
                       expectedFilename ||
                   !sameConfig(event.resourceConfig, firstConfig);
        });
    if ( invalidEvent != descriptor.m_events.end() ) {
        XERROR("Repeated stable ID did not retain first-resource semantics");
        return false;
    }
    return true;
}

/// @brief 验证不同匹配方式冲突时仍按项目资源顺序选择首项。
///
/// 同一引用对前项是路径和 basename 匹配，对后项是精确 ID 匹配；
/// 旧实现逐资源扫描时必须选择前项，统一索引也必须保持该语义。
/// @return 描述符解析到项目列表前项时返回 true。
bool testCrossModeConflictPreservesResourceOrder()
{
    auto project = makeProject();
    project.m_audioResources.clear();

    MMM::AudioTrackConfig firstConfig;
    firstConfig.volume = 0.23F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "foo.wav",
        .m_path   = "audio/foo.wav",
        .m_type   = MMM::AudioTrackType::Effect,
        .m_config = firstConfig,
    });

    MMM::AudioTrackConfig exactIdConfig;
    exactIdConfig.volume = 0.87F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "audio/foo.wav",
        .m_path   = "audio/exact-id.wav",
        .m_type   = MMM::AudioTrackType::Main,
        .m_config = exactIdConfig,
    });

    MMM::BeatMap beatMap;
    beatMap.m_audioSamples.push_back(
        makeSample(0.0, 0, 4, "audio/foo.wav", 1.0F));
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    if ( descriptor.m_events.size() != 1U ||
         !descriptor.m_diagnostics.empty() ) {
        return false;
    }

    const auto& event = descriptor.m_events.front();
    if ( event.resourceKey != "foo.wav" ||
         MMM::Config::utf8ToPath(event.filePath).filename() != "foo.wav" ||
         !sameConfig(event.resourceConfig, firstConfig) ) {
        XERROR("Cross-mode audio reference conflict changed first-match order");
        return false;
    }
    return true;
}

/// @brief 验证大量不同谱面相对旧路径在一次资源索引中完整解析。
///
/// 每个事件引用不同路径且资源使用不同稳定 ID，使输入能够暴露
/// unique-reference×resource 的重复扫描退化。
/// @return 所有不同旧路径均解析为对应稳定资源 ID 时返回 true。
bool testBulkDistinctLegacyPathResolution()
{
    constexpr std::size_t RESOURCE_COUNT = 384U;

    auto project = makeProject();
    project.m_audioResources.clear();
    project.m_audioResources.reserve(RESOURCE_COUNT);

    MMM::BeatMap beatMap;
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto filename = "legacy-" + std::to_string(index) + ".wav";
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = "stable-legacy-" + std::to_string(index),
            .m_path = "audio/" + filename,
            .m_type = MMM::AudioTrackType::Effect,
        });
        beatMap.m_audioSamples.push_back(makeSample(
            static_cast<double>(index), 0, 4, "../audio/" + filename, 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    if ( descriptor.m_events.size() != RESOURCE_COUNT ||
         !descriptor.m_diagnostics.empty() ) {
        XERROR("Bulk distinct legacy references were not all resolved");
        return false;
    }

    std::unordered_set<std::string> resolvedIds;
    resolvedIds.reserve(descriptor.m_events.size());
    for ( const auto& event : descriptor.m_events ) {
        resolvedIds.insert(event.resourceKey);
    }
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        if ( !resolvedIds.contains("stable-legacy-" + std::to_string(index)) ) {
            XERROR("Distinct legacy reference missed resource {}", index);
            return false;
        }
    }
    return true;
}

/// @brief 验证重复绝对旧路径在批量解析中共享同一缓存结果。
///
/// 大量事件使用相同绝对引用，确保描述符只需按唯一引用和唯一资源执行
/// 文件系统规范化，同时仍保留每个自动采样事件。
/// @return 全部重复事件均解析到同一稳定资源时返回 true。
bool testRepeatedAbsoluteLegacyReferenceResolution()
{
    constexpr std::size_t EVENT_COUNT = 512U;

    auto       project = makeProject();
    const auto absolutePath =
        project.m_projectRoot / "outside" / "repeated-absolute.wav";
    project.m_audioResources = {
        MMM::AudioResource{
            .m_id   = "stable-absolute",
            .m_path = MMM::Config::pathToUtf8(absolutePath),
            .m_type = MMM::AudioTrackType::Effect,
        },
    };

    MMM::BeatMap beatMap;
    const auto   absoluteReference = MMM::Config::pathToUtf8(absolutePath);
    for ( std::size_t index = 0U; index < EVENT_COUNT; ++index ) {
        beatMap.m_audioSamples.push_back(makeSample(
            static_cast<double>(index), 0, 4, absoluteReference, 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    if ( descriptor.m_events.size() != EVENT_COUNT ||
         !descriptor.m_diagnostics.empty() ||
         std::any_of(descriptor.m_events.begin(),
                     descriptor.m_events.end(),
                     [](const MMM::Audio::AudioTimelineLoadEvent& event) {
                         return event.resourceKey != "stable-absolute";
                     }) ) {
        XERROR("Repeated absolute legacy references were not fully resolved");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行音频时间线描述符构建测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testCanonicalDescriptor() && testOrderIndependentIdentity() &&
                   testNonAudioFieldsAreExcluded() &&
                   testFingerprintSensitivity() &&
                   testDescriptorResourceReferenceLookup() &&
                   testBulkStableIdResolutionUsesFirstResource() &&
                   testCrossModeConflictPreservesResourceOrder() &&
                   testBulkDistinctLegacyPathResolution() &&
                   testRepeatedAbsoluteLegacyReferenceResolution()
               ? 0
               : 1;
}
