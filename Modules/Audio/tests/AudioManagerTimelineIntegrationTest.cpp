#include "audio/AudioManager.h"

#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

/// @brief 在真实音频后端回调推进期间等待一个有界条件。
/// @tparam Predicate 无参数条件函数类型。
/// @param predicate 待满足条件。
/// @param timeout 最长等待时间。
/// @return 条件在超时前满足时返回 true。
template<typename Predicate>
bool waitUntil(Predicate&&               predicate,
               std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( predicate() ) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

/// @brief 判断加载结果是否包含指定诊断。
/// @param result 待检查加载结果。
/// @param code 目标诊断类型。
/// @return 至少包含一项时返回 true。
bool hasDiagnostic(const MMM::Audio::AudioTimelineLoadResult&  result,
                   MMM::Audio::AudioTimelineLoadDiagnosticCode code)
{
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [code](const MMM::Audio::AudioTimelineLoadDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

/// @brief 验证无音频片段时谱面结束时间仍可独立推进。
/// @param manager 已初始化音频管理器。
/// @return 验证通过时返回 true。
bool testEmptyTimelineClock(MMM::Audio::AudioManager& manager)
{
    const auto result = manager.loadAudioTimeline({}, 0.08, "empty-timeline");
    if ( !result.success || result.loadedClipCount != 0U ||
         result.missingClipCount != 0U || result.requestedSourceCount != 0U ||
         result.preparedResourceCount != 0U ||
         !manager.hasLoadedAudioTimeline() ||
         manager.getLoadedAudioTimelineFingerprint() != "empty-timeline" ||
         std::abs(manager.getTotalTime() - 0.08) > 0.002 ) {
        XERROR("Empty timeline was not constructed as an independent clock");
        return false;
    }

    manager.play();
    if ( !waitUntil(
             [&]() {
                 return manager.getStatus() ==
                        MMM::Audio::PlaybackStatus::Stopped;
             },
             1000ms) ||
         manager.getCurrentTime() < 0.075 ) {
        XERROR("Empty timeline did not reach its chart-defined end");
        return false;
    }
    return true;
}

/// @brief 验证单片段资源 DSP 生效且不会提升为全局预览参数。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
bool testSingleClipResourceProcessing(MMM::Audio::AudioManager& manager,
                                      const std::string&        samplePath)
{
    MMM::AudioTrackConfig config;
    config.volume        = 0.75F;
    config.playbackSpeed = 1.5F;
    config.playbackPitch = 3.0F;
    config.eqEnabled     = true;
    config.eqPreset      = static_cast<int>(MMM::Audio::EQPreset::TenBand);
    config.eqBandGains.assign(10U, 3.0F);
    config.eqBandQs.assign(10U, 1.2F);

    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                      .eventId               = 11U,
                                      .resourceKey           = "main",
                                      .filePath              = samplePath,
                                      .effectiveStartSeconds = 0.01,
                                      .eventVolume           = 0.5F,
                                      .resourceConfig        = config,
                                  } },
                                  0.02,
                                  "single-timeline");

    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 1U ||
         result.missingClipCount != 0U || !result.diagnostics.empty() ||
         std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.empty();
                     }) ||
         std::abs(manager.getPlaybackSpeed() - 1.0) > 1.0e-6 ||
         std::abs(manager.getPlaybackPitch()) > 1.0e-6 ||
         manager.isMainTrackEQEnabled() ) {
        XERROR(
            "Per-resource advanced settings leaked into the composite graph");
        return false;
    }

    const auto   rawTrack = manager.getBGMTrack();
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( !rawTrack || sampleRate <= 0.0 ) return false;
    const double expectedEnd =
        0.01 + static_cast<double>(rawTrack->num_frames()) /
                   (sampleRate * static_cast<double>(config.playbackSpeed));
    if ( std::abs(manager.getTotalTime() - expectedEnd) > 0.02 ) {
        XERROR("Per-resource playbackSpeed did not change clip duration");
        return false;
    }
    return true;
}

/// @brief 验证玩家与 BGM 逐轨静音状态及调度替换保持时间线结构。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 状态可独立切换且 BGM 调度替换不改变时长和片段数量时返回 true。
bool testKeySoundTrackMutes(MMM::Audio::AudioManager& manager,
                            const std::string&        samplePath)
{
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                        .eventId               = 501U,
                                        .resourceKey           = "lane-zero",
                                        .filePath              = samplePath,
                                        .effectiveStartSeconds = 0.0,
                                        .bgmTrackIndex         = 0U,
                                    },
                                    MMM::Audio::AudioTimelineLoadEvent{
                                        .eventId               = 502U,
                                        .resourceKey           = "lane-two",
                                        .filePath              = samplePath,
                                        .effectiveStartSeconds = 0.02,
                                        .bgmTrackIndex         = 2U,
                                    } },
                                  0.08,
                                  "key-sound-track-mutes");
    if ( !result.success || result.loadedClipCount != 2U ) return false;

    const double totalTime = manager.getTotalTime();
    manager.setPlayerKeySoundTrackMuted(1U, true);
    manager.setBgmKeySoundTrackMuted(2U, true);
    manager.setBgmKeySoundAreaMuted(true);
    if ( !manager.isPlayerKeySoundTrackMuted(1U) ||
         manager.isPlayerKeySoundTrackMuted(2U) ||
         !manager.isBgmKeySoundTrackMuted(2U) ||
         manager.isBgmKeySoundTrackMuted(1U) ||
         !manager.isBgmKeySoundAreaMuted() ||
         manager.getLoadedAudioTimelineClipCount() != 2U ||
         std::abs(manager.getTotalTime() - totalTime) > 0.002 ) {
        XERROR("Key sound track mute state changed timeline structure");
        return false;
    }

    manager.setPlayerKeySoundTrackMuted(1U, false);
    manager.setBgmKeySoundTrackMuted(2U, false);
    manager.setBgmKeySoundAreaMuted(false);
    return !manager.isPlayerKeySoundTrackMuted(1U) &&
           !manager.isBgmKeySoundTrackMuted(2U) &&
           !manager.isBgmKeySoundAreaMuted();
}

/// @brief 验证同一文件的不同资源倍率可并存且全局预览倍率不改写资源时长。
bool testIndependentResourceAndGlobalSpeed(MMM::Audio::AudioManager& manager,
                                           const std::string&        samplePath)
{
    MMM::AudioTrackConfig fastConfig;
    fastConfig.playbackSpeed = 2.0F;
    MMM::AudioTrackConfig slowConfig;
    slowConfig.playbackSpeed = 0.5F;

    manager.setPlaybackSpeed(1.0);
    const auto result = manager.loadAudioTimeline(
        {
            {
                .eventId               = 21U,
                .resourceKey           = "fast",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = fastConfig,
            },
            {
                .eventId               = 22U,
                .resourceKey           = "slow",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = slowConfig,
            },
        },
        0.0,
        "independent-resource-speeds");
    const auto   rawTrack = manager.getBGMTrack();
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 2U || result.loadedClipCount != 2U ||
         !rawTrack || sampleRate <= 0.0 ) {
        XERROR("Different resource speeds did not load together");
        return false;
    }

    const double expectedEnd =
        static_cast<double>(rawTrack->num_frames()) /
        (sampleRate * static_cast<double>(slowConfig.playbackSpeed));
    const double resourceDefinedEnd = manager.getTotalTime();
    manager.setPlaybackSpeed(1.75);
    if ( std::abs(resourceDefinedEnd - expectedEnd) > 0.02 ||
         std::abs(manager.getTotalTime() - resourceDefinedEnd) > 1.0e-9 ||
         std::abs(manager.getPlaybackSpeed() - 1.75) > 1.0e-9 ) {
        XERROR("Global preview speed overwrote per-resource duration");
        return false;
    }
    manager.setPlaybackSpeed(1.0);
    return true;
}

/// @brief 验证同一 Effect 的自动采样和 Note HitEffect 共用资源 DSP PCM。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 缓存身份、资源时长、初始静音和全局变速隔离均正确时返回 true。
bool testDualUseEffectSharesPreparedAudio(MMM::Audio::AudioManager& manager,
                                          const std::string&        samplePath)
{
    constexpr const char* EFFECT_KEY = "dual-use-effect";
    MMM::AudioTrackConfig config;
    config.volume        = 0.63F;
    config.muted         = true;
    config.playbackSpeed = 1.6F;
    config.playbackPitch = 5.0F;
    config.eqEnabled     = true;
    config.eqPreset      = static_cast<int>(MMM::Audio::EQPreset::TenBand);
    config.eqBandGains.assign(10U, 2.0F);
    config.eqBandQs.assign(10U, 1.1F);

    manager.setPlaybackSpeed(1.0);
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                      .eventId               = 31U,
                                      .resourceKey           = EFFECT_KEY,
                                      .filePath              = samplePath,
                                      .effectiveStartSeconds = 0.0,
                                      .eventVolume           = 1.0F,
                                      .resourceConfig        = config,
                                  } },
                                  0.0,
                                  "dual-use-effect-timeline");
    const auto rawTrack = manager.getBGMTrack();
    const auto sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 1U ||
         !rawTrack || sampleRate <= 0.0 ) {
        XERROR("Dual-use Effect timeline could not be prepared");
        return false;
    }

    manager.registerSoundEffect(EFFECT_KEY, samplePath, config);
    if ( !manager.queueBoundNoteSoundEffectLoad(EFFECT_KEY) ||
         !waitUntil([&]() {
             manager.updateQueuedSoundEffectLoads();
             return manager.isSoundEffectLoaded(EFFECT_KEY);
         }) ) {
        XERROR("Dual-use Effect could not create its HitEffect pool");
        return false;
    }

    const double timelineDuration = manager.getTotalTime();
    const double sfxDuration      = manager.getSFXDuration(EFFECT_KEY);
    const double expectedDuration =
        static_cast<double>(rawTrack->num_frames()) /
        (sampleRate * static_cast<double>(config.playbackSpeed));
    if ( !manager.isSFXUsingSharedTimelineAudio(EFFECT_KEY) ||
         std::abs(timelineDuration - sfxDuration) > 1.0e-9 ||
         std::abs(sfxDuration - expectedDuration) > 0.02 ||
         std::abs(manager.getSFXPoolVolume(EFFECT_KEY) - config.volume) >
             1.0e-6F ||
         !manager.getSFXPoolMute(EFFECT_KEY) ) {
        XERROR(
            "Dual-use Effect did not share prepared DSP PCM or resource gain");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    manager.setPlaybackSpeed(1.75);
    if ( std::abs(manager.getTotalTime() - timelineDuration) > 1.0e-9 ||
         std::abs(manager.getSFXDuration(EFFECT_KEY) - sfxDuration) > 1.0e-9 ) {
        XERROR("Global preview speed was applied twice to dual-use Effect PCM");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    manager.setSFXPoolMute(EFFECT_KEY, false, false);
    manager.seek(0.0);
    manager.playSoundEffectScheduled(EFFECT_KEY, 60.0);
    if ( !manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Scrub seek setup did not schedule the dual-use Effect");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }
    manager.seek(0.01, MMM::Audio::AudioSeekMode::ScrubUpdate);
    if ( !manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Scrub update unexpectedly traversed and cleared SFX pools");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }
    manager.seek(0.01);
    if ( manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Committed seek did not clear scheduled SFX pools");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    manager.setPlaybackSpeed(1.0);
    manager.unloadSoundEffect(EFFECT_KEY);
    return true;
}

/// @brief 验证旧单 BGM 入口只包装零秒单事件且不恢复旧 SourceNode 时钟。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
bool testLegacyBgmWrapper(MMM::Audio::AudioManager& manager,
                          const std::string&        samplePath)
{
    MMM::AudioTrackConfig config;
    config.volume        = 0.7F;
    config.playbackSpeed = 1.75F;
    config.playbackPitch = -4.0F;
    manager.setPlaybackPitch(2.0);
    manager.setPlaybackQuality(MMM::Audio::AudioManager::StretchQuality::Fast);
    manager.createMainTrackEQ(MMM::Audio::EQPreset::TenBand);
    if ( !manager.loadBGM(samplePath, config) ||
         !manager.hasLoadedAudioTimeline() ||
         manager.getLoadedAudioTimelineClipCount() != 1U ||
         manager.getLoadedBGMPath() != samplePath ||
         std::abs(manager.getPlaybackSpeed() - 1.0) > 1.0e-6 ||
         std::abs(manager.getPlaybackPitch() - 2.0) > 1.0e-6 ||
         manager.getPlaybackQuality() !=
             MMM::Audio::AudioManager::StretchQuality::Fast ||
         !manager.isMainTrackEQEnabled() ) {
        XERROR("Legacy BGM wrapper bypassed the composite timeline");
        return false;
    }
    manager.setPlaybackPitch(0.0);
    manager.setPlaybackQuality(MMM::Audio::AudioManager::StretchQuality::Finer);
    manager.destroyMainTrackEQ();
    return true;
}

/// @brief 验证重复文件、多资源、缺失资源及暂停、Seek、停止和自然结束。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
bool testCompositePlayback(MMM::Audio::AudioManager& manager,
                           const std::string&        samplePath)
{
    MMM::AudioTrackConfig mainConfig;
    mainConfig.volume = 0.8F;
    MMM::AudioTrackConfig effectConfig;
    effectConfig.volume = 0.6F;
    MMM::AudioTrackConfig mutedConfig;
    mutedConfig.volume = 1.0F;
    mutedConfig.muted  = true;

    const auto missingPath = (std::filesystem::path(samplePath).parent_path() /
                              "missing-audio-timeline-resource.wav")
                                 .string();
    const auto result      = manager.loadAudioTimeline(
        {
            {
                .eventId               = 1U,
                .resourceKey           = "main-a",
                .filePath              = samplePath,
                .effectiveStartSeconds = -0.01,
                .eventVolume           = 1.0F,
                .resourceConfig        = mainConfig,
            },
            {
                .eventId               = 2U,
                .resourceKey           = "effect-a",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.01,
                .eventVolume           = 0.5F,
                .resourceConfig        = effectConfig,
            },
            {
                .eventId               = 3U,
                .resourceKey           = "main-b",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.02,
                .eventVolume           = 1.0F,
                .resourceConfig        = mutedConfig,
            },
            {
                .eventId               = 4U,
                .resourceKey           = "missing",
                .filePath              = missingPath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = effectConfig,
            },
        },
        0.5,
        "composite-timeline");

    if ( !result.success || result.requestedSourceCount != 2U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 3U ||
         result.missingClipCount != 1U ||
         manager.getLoadedAudioTimelineClipCount() != 3U ||
         manager.getMissingAudioTimelineClipCount() != 1U ||
         manager.getLoadedAudioTimelineFingerprint() != "composite-timeline" ||
         !hasDiagnostic(
             result,
             MMM::Audio::AudioTimelineLoadDiagnosticCode::MissingResource) ||
         manager.getTotalTime() < 0.49 ) {
        XERROR("Composite timeline phases did not deduplicate sources and DSP");
        return false;
    }

    manager.setPlaybackSpeed(1.5);
    manager.seek(0.0);
    manager.play();
    if ( !waitUntil([&]() {
             return manager.getCurrentTime() > 0.04 &&
                    std::abs(manager.getActualPlaybackSpeed() - 1.5) <= 0.02;
         }) ) {
        XERROR("Composite timeline did not follow global preview speed");
        return false;
    }

    manager.pause();
    if ( !waitUntil([&]() {
             return manager.getStatus() == MMM::Audio::PlaybackStatus::Paused;
         }) ) {
        XERROR("Composite timeline did not pause");
        return false;
    }
    manager.setPlaybackSpeed(1.0);
    std::this_thread::sleep_for(30ms);
    const double pausedTime = manager.getCurrentTime();
    std::this_thread::sleep_for(40ms);
    if ( std::abs(manager.getCurrentTime() - pausedTime) > 0.002 ) {
        XERROR("Paused composite timeline continued advancing");
        return false;
    }

    manager.seek(0.2);
    if ( !waitUntil(
             [&]() { return std::abs(manager.getCurrentTime() - 0.2) < 0.01; },
             1000ms) ) {
        XERROR("Composite timeline seek did not reach target");
        return false;
    }

    manager.play();
    if ( !waitUntil([&]() { return manager.getCurrentTime() > 0.23; }) ) {
        XERROR("Composite timeline did not resume after seek");
        return false;
    }

    manager.stop();
    if ( !waitUntil(
             [&]() { return std::abs(manager.getCurrentTime()) < 0.002; },
             1000ms) ||
         manager.getStatus() != MMM::Audio::PlaybackStatus::Stopped ) {
        XERROR("Composite timeline stop did not clear its position");
        return false;
    }

    manager.seek(manager.getTotalTime() - 0.02);
    manager.play();
    if ( !waitUntil(
             [&]() {
                 return manager.getStatus() ==
                        MMM::Audio::PlaybackStatus::Stopped;
             },
             1000ms) ) {
        XERROR("Composite timeline did not publish natural completion");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 AudioManager 自动采样时间线集成测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为可解码短音频路径。
/// @return 全部验证通过时返回零。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: AudioManagerTimelineIntegrationTest <sample_path>");
        return 1;
    }

    XLogger::init("AudioManagerTimelineIntegrationTest");
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    settings.audioPlaybackBackend = MMM::Config::AudioPlaybackBackend::SDL;
    settings.sdlAudioOutputDeviceName.clear();

    MMM::Runtime::AppThreadPool::instance().init();
    auto& manager = MMM::Audio::AudioManager::instance();
    manager.init();
    manager.setPlaybackSpeed(1.0);
    manager.setPlaybackPitch(0.0);
    manager.destroyMainTrackEQ();

    const std::string samplePath = argv[1];
    const bool        passed =
        testEmptyTimelineClock(manager) &&
        testSingleClipResourceProcessing(manager, samplePath) &&
        testKeySoundTrackMutes(manager, samplePath) &&
        testIndependentResourceAndGlobalSpeed(manager, samplePath) &&
        testDualUseEffectSharesPreparedAudio(manager, samplePath) &&
        testLegacyBgmWrapper(manager, samplePath) &&
        testCompositePlayback(manager, samplePath);

    manager.shutdown();
    MMM::Runtime::AppThreadPool::instance().shutdown();
    return passed ? 0 : 1;
}
