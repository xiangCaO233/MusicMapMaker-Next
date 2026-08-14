#include "network/collaboration/CollaborationRoom.h"
#include "network/collaboration/CollaborationBuildFingerprint.h"
#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration_server/CollaborationSignalingServer.h"
#include "runtime/AppThreadPool.h"

#include "log/colorful-log.h"

#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using MMM::BeatMap;
using MMM::BeatmapMutationFlags;
using MMM::Network::Collaboration::CollaborationHostRoomConfig;
using MMM::Network::Collaboration::CollaborationJoinRoomConfig;
using MMM::Network::Collaboration::CollaborationLogEventType;
using MMM::Network::Collaboration::CollaborationRoom;
using MMM::Network::Collaboration::CollaborationRoomState;
using MMM::Network::Collaboration::CollaborationResourceBundle;
using MMM::Network::Collaboration::CollaborationServerEndpoint;
using MMM::Network::Collaboration::PeerId;
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 本机 WebRTC 集成测试允许的最长等待时间。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(15);
/// @brief 公网部署探针允许的最长等待时间。
constexpr auto EXTERNAL_TEST_TIMEOUT = std::chrono::seconds(30);
/// @brief 验收房间中的访客数量。
constexpr std::size_t GUEST_COUNT = 1;

/// @brief 为真实 WebRTC 测试参与者生成确定的稳定身份。
/// @param ordinal 当前测试房间内的身份序号。
/// @return 满足协议格式的 32 字符小写十六进制标识。
[[nodiscard]] std::string makeRoomTestParticipantId(std::size_t ordinal)
{
    constexpr std::string_view DIGITS = "0123456789abcdef";
    std::string                identity(32U, '0');
    identity.front() = 'd';
    for ( std::size_t index = identity.size(); index > 1U; --index ) {
        identity[index - 1U] = DIGITS[ordinal & 0xFU];
        ordinal >>= 4U;
    }
    return identity;
}

/// @brief 为真实 WebRTC 资源测试创建并清理隔离目录。
class ScopedRoomResourceDirectory
{
public:
    /// @brief 创建唯一测试目录。
    ScopedRoomResourceDirectory()
    {
        std::error_code error;
        auto            root = std::filesystem::temp_directory_path(error);
        if ( error ) return;
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = root /
                 ("mmm-collaboration-room-resource-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, error);
        if ( error ) m_path.clear();
    }

    /// @brief 删除测试目录。
    ~ScopedRoomResourceDirectory()
    {
        if ( m_path.empty() ) return;
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedRoomResourceDirectory(const ScopedRoomResourceDirectory&) = delete;
    ScopedRoomResourceDirectory& operator=(const ScopedRoomResourceDirectory&) =
        delete;

    /// @brief 返回测试目录。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 测试目录路径。
    std::filesystem::path m_path;
};

/// @brief 构造覆盖物件、时间线、采样和元数据的协作谱面。
std::shared_ptr<BeatMap> makeBeatmap(double noteTimestamp, std::string author)
{
    auto beatmap                               = std::make_shared<BeatMap>();
    beatmap->m_baseMapMetadata.name            = "Collaboration Test";
    beatmap->m_baseMapMetadata.title           = "Transport Integrity";
    beatmap->m_baseMapMetadata.author          = std::move(author);
    beatmap->m_baseMapMetadata.track_count     = 6;
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    beatmap->m_baseMapMetadata.preference_bpm  = 150.0;

    auto& note             = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp       = noteTimestamp;
    note.m_track           = 2;
    note.m_collaborationId = "room-note-root";
    note.m_sampleBinding   = MMM::AudioSampleBinding{
        .m_audioResourceId = "tap.wav",
        .m_volume          = 0.45F,
    };
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["transport"] =
        "preserved";

    auto& hold             = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp       = 2000.0;
    hold.m_duration        = 600.0;
    hold.m_track           = 3;
    hold.m_collaborationId = "room-hold-root";

    auto& subHold              = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp        = 2800.0;
    subHold.m_duration         = 400.0;
    subHold.m_track            = 1;
    subHold.m_isSubNote        = true;
    subHold.m_collaborationId  = "room-polyline-sub-hold";
    auto& subFlick             = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp       = 3200.0;
    subFlick.m_track           = 1;
    subFlick.m_dtrack          = 2;
    subFlick.m_isSubNote       = true;
    subFlick.m_collaborationId = "room-polyline-sub-flick";
    auto& polyline             = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp       = subHold.m_timestamp;
    polyline.m_track           = subHold.m_track;
    polyline.m_collaborationId = "room-polyline-root";
    polyline.m_subHolds.emplace_back(subHold);
    polyline.m_subFlicks.emplace_back(subFlick);
    polyline.m_subNotes.emplace_back(subHold);
    polyline.m_subNotes.emplace_back(subFlick);

    auto& timing                   = beatmap->m_timings.emplace_back();
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 150.0;
    timing.m_beat_length           = 400.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 150.0;
    timing.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["transport"] =
        "preserved";

    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 500.0;
    sample.m_offsetMs        = -12;
    sample.m_track           = 6;
    sample.m_audioResourceId = "kick.wav";
    sample.m_volume          = 0.75F;
    sample.m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["transport"] =
        "preserved";
    beatmap->sync();
    return beatmap;
}

/// @brief 驱动房主和全部访客直到条件满足或超时。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server, CollaborationRoom& host,
               std::vector<std::unique_ptr<CollaborationRoom>>& guests,
               Predicate                                        predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        server.update();
        host.update();
        for ( auto& guest : guests ) guest->update();
        host.update();
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// @brief 统计指定类型的协作日志条数。
std::size_t countLogs(const CollaborationRoom&  room,
                      CollaborationLogEventType type)
{
    return static_cast<std::size_t>(std::count_if(
        room.logs().begin(), room.logs().end(), [type](const auto& entry) {
            return entry.type == type;
        }));
}

/// @brief 统计指定详情的协作日志条数。
std::size_t countLogDetails(const CollaborationRoom& room,
                            std::string_view         detail)
{
    return static_cast<std::size_t>(std::count_if(
        room.logs().begin(), room.logs().end(), [detail](const auto& entry) {
            return entry.detail == detail;
        }));
}

/// @brief 判断谱面是否包含指定同步结果。
bool hasExpectedState(const std::shared_ptr<const BeatMap>& beatmap,
                      double noteTimestamp, std::string_view author)
{
    if ( !beatmap || beatmap->m_noteData.notes.size() != 1U ||
         beatmap->m_noteData.holds.size() != 2U ||
         beatmap->m_noteData.flicks.size() != 1U ||
         beatmap->m_noteData.polylines.size() != 1U ||
         beatmap->m_timings.size() != 1U ||
         beatmap->m_audioSamples.size() != 1U ) {
        return false;
    }
    const auto& note     = beatmap->m_noteData.notes.front();
    const auto& hold     = beatmap->m_noteData.holds.front();
    const auto& timing   = beatmap->m_timings.front();
    const auto& sample   = beatmap->m_audioSamples.front();
    const auto& polyline = beatmap->m_noteData.polylines.front();
    return std::abs(note.m_timestamp - noteTimestamp) < 1e-6 &&
           note.m_track == 2U && note.m_sampleBinding &&
           note.m_sampleBinding->m_audioResourceId == "tap.wav" &&
           std::abs(note.m_sampleBinding->m_volume - 0.45F) < 1e-6F &&
           note.m_metadata.note_properties.at(MMM::NoteMetadataType::MMM)
                   .at("transport") == "preserved" &&
           std::abs(hold.m_timestamp - 2000.0) < 1e-6 &&
           std::abs(hold.m_duration - 600.0) < 1e-6 && hold.m_track == 3U &&
           polyline.m_subNotes.size() == 2U &&
           polyline.m_subHolds.size() == 1U &&
           polyline.m_subFlicks.size() == 1U &&
           polyline.m_subNotes[0].get().m_isSubNote &&
           polyline.m_subNotes[0].get().m_type == MMM::NoteType::HOLD &&
           polyline.m_subNotes[1].get().m_isSubNote &&
           polyline.m_subNotes[1].get().m_type == MMM::NoteType::FLICK &&
           std::abs(timing.m_timestamp) < 1e-6 &&
           std::abs(timing.m_bpm - 150.0) < 1e-6 &&
           std::abs(timing.m_beat_length - 400.0) < 1e-6 &&
           timing.m_timingEffect == MMM::TimingEffect::BPM &&
           timing.m_metadata.timing_properties
                   .at(MMM::TimingMetadataType::MALODY)
                   .at("transport") == "preserved" &&
           std::abs(sample.m_timestamp - 500.0) < 1e-6 &&
           sample.m_offsetMs == -12 && sample.m_track == 6U &&
           sample.m_audioResourceId == "kick.wav" &&
           std::abs(sample.m_volume - 0.75F) < 1e-6F &&
           sample.m_metadata.sample_properties.at(MMM::SampleMetadataType::MMM)
                   .at("transport") == "preserved" &&
           beatmap->m_baseMapMetadata.name == "Collaboration Test" &&
           beatmap->m_baseMapMetadata.title == "Transport Integrity" &&
           beatmap->m_baseMapMetadata.author == author &&
           beatmap->m_baseMapMetadata.track_count == 6 &&
           beatmap->m_baseMapMetadata.bgm_track_count == 1 &&
           std::abs(beatmap->m_baseMapMetadata.preference_bpm - 150.0) < 1e-6;
}

/// @brief 判断谱面根 Note 是否包含全部指定时间戳。
bool hasRootNoteTimestamps(const std::shared_ptr<const BeatMap>& beatmap,
                           std::initializer_list<double>         timestamps)
{
    if ( !beatmap || beatmap->m_noteData.notes.size() != timestamps.size() ) {
        return false;
    }
    return std::all_of(timestamps.begin(), timestamps.end(), [&](double value) {
        return std::any_of(beatmap->m_noteData.notes.begin(),
                           beatmap->m_noteData.notes.end(),
                           [value](const MMM::Note& note) {
                               return std::abs(note.m_timestamp - value) < 1e-6;
                           });
    });
}

/// @brief 判断房主和访客连续写入的稳定 ID 物件是否全部收敛。
bool hasConcurrentBurstNotes(const std::shared_ptr<const BeatMap>& beatmap,
                             std::size_t                           count)
{
    if ( !beatmap || beatmap->m_noteData.notes.size() != 3U + count * 2U ) {
        return false;
    }
    for ( std::size_t index = 0; index < count; ++index ) {
        const auto contains = [&](std::string_view prefix) {
            const std::string identity =
                std::string(prefix) + std::to_string(index);
            return std::any_of(beatmap->m_noteData.notes.begin(),
                               beatmap->m_noteData.notes.end(),
                               [&identity](const MMM::Note& note) {
                                   return note.m_collaborationId == identity;
                               });
        };
        if ( !contains("host-burst-") || !contains("guest-burst-") ) {
            return false;
        }
    }
    return true;
}

/// @brief 覆盖中心信令、真实 WebRTC、初始快照和双向增量收敛。
bool testPublicDirectoryWebRtcRoom()
{
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Host Creator";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Public WebRTC Test";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return false;
    }

    std::shared_ptr<const BeatMap> hostModel;
    std::size_t                    hostApplyCount = 0;
    host.setApplyBeatmapCallback(
        [&hostModel, &hostApplyCount](std::shared_ptr<const BeatMap> beatmap,
                                      BeatmapMutationFlags,
                                      std::uint64_t,
                                      std::uint64_t,
                                      std::optional<std::vector<std::string>>) {
            hostModel = std::move(beatmap);
            ++hostApplyCount;
        });
    auto initial = makeBeatmap(1000.0, "Host Creator");
    hostModel    = initial;
    static_cast<void>(
        host.onBeatmapMutated(*initial, BeatmapMutationFlags::All));
    host.update();
    if ( !hasExpectedState(hostModel, 1000.0, "Host Creator") ||
         hostApplyCount != 0U ) {
        return false;
    }

    std::vector<std::shared_ptr<const BeatMap>> guestModels(GUEST_COUNT);
    std::vector<std::size_t>                    guestApplyCounts(GUEST_COUNT);
    std::vector<std::vector<BeatmapMutationFlags>> guestApplyFlags(GUEST_COUNT);
    std::vector<std::vector<std::optional<std::vector<std::string>>>>
                                            guestApplyObjectDeltas(GUEST_COUNT);
    std::vector<std::vector<std::uint64_t>> guestApplyMutationSequences(
        GUEST_COUNT);
    std::vector<std::vector<std::uint64_t>> guestAcknowledgedMutationSequences(
        GUEST_COUNT);
    std::vector<std::uint64_t> guestRequiredMutationSequences(GUEST_COUNT);
    guests.reserve(GUEST_COUNT);
    for ( std::size_t index = 0; index < GUEST_COUNT; ++index ) {
        auto guest = std::make_unique<CollaborationRoom>();
        guest->setApplyBeatmapCallback(
            [&guestModels,
             &guestApplyCounts,
             &guestApplyFlags,
             &guestApplyObjectDeltas,
             &guestApplyMutationSequences,
             &guestRequiredMutationSequences,
             index](std::shared_ptr<const BeatMap> beatmap,
                    BeatmapMutationFlags           flags,
                    std::uint64_t includedLocalMutationSequence,
                    std::uint64_t,
                    std::optional<std::vector<std::string>>
                        objectDeltaIdentities) {
                if ( includedLocalMutationSequence <
                     guestRequiredMutationSequences[index] ) {
                    return;
                }
                guestModels[index] = std::move(beatmap);
                ++guestApplyCounts[index];
                guestApplyFlags[index].push_back(flags);
                guestApplyObjectDeltas[index].push_back(
                    std::move(objectDeltaIdentities));
                guestApplyMutationSequences[index].push_back(
                    includedLocalMutationSequence);
            });
        guest->setLocalMutationAcknowledgedCallback(
            [&guestAcknowledgedMutationSequences,
             &guestRequiredMutationSequences,
             index](std::uint64_t sequence) {
                guestAcknowledgedMutationSequences[index].push_back(sequence);
                guestRequiredMutationSequences[index] =
                    std::max(guestRequiredMutationSequences[index], sequence);
            });
        CollaborationJoinRoomConfig guestConfig;
        guestConfig.creator       = "Guest " + std::to_string(index + 1);
        guestConfig.participantId = makeRoomTestParticipantId(index + 2);
        guestConfig.roomId        = host.roomId();
        guestConfig.roomName      = host.roomName();
        guestConfig.endpoint      = endpoint;
        if ( !guest->join(guestConfig) ) return false;
        guests.push_back(std::move(guest));
        if ( !pumpUntil(
                 server,
                 host,
                 guests,
                 [&]() {
                     return guests.back()->state() ==
                                CollaborationRoomState::AwaitingApproval &&
                            !host.pendingJoinRequests().empty();
                 }) ||
             !host.approveJoinRequest(
                 host.pendingJoinRequests().front().requestId) ) {
            return false;
        }
        if ( !pumpUntil(server, host, guests, [&]() {
                 return guests.back()->state() ==
                        CollaborationRoomState::Connected;
             }) ) {
            XERROR(
                "Guest {} failed during staged connection: state={}, error={}",
                index + 1U,
                static_cast<int>(guests.back()->state()),
                guests.back()->lastError());
            for ( const auto& entry : guests.back()->logs() ) {
                XERROR("Guest {} staged log type={} detail={}",
                       index + 1U,
                       static_cast<int>(entry.type),
                       entry.detail);
            }
            XERROR("Server clients={}, rooms={}, host participants={}",
                   server.clientCount(),
                   server.roomCount(),
                   host.participants().size());
            for ( const auto& entry : host.logs() ) {
                XERROR("Host staged log type={} detail={}",
                       static_cast<int>(entry.type),
                       entry.detail);
            }
            return false;
        }
    }

    const bool connected = pumpUntil(server, host, guests, [&]() {
        if ( host.participants().size() != GUEST_COUNT + 1U ) return false;
        for ( std::size_t index = 0; index < guests.size(); ++index ) {
            if ( guests[index]->state() != CollaborationRoomState::Connected ||
                 guests[index]->participants().size() != GUEST_COUNT + 1U ||
                 !hasExpectedState(
                     guestModels[index], 1000.0, "Host Creator") ) {
                return false;
            }
        }
        return true;
    });
    if ( !connected ) {
        XERROR(
            "Collaboration test connection timeout: host participants={}, "
            "state={}, error={}",
            host.participants().size(),
            static_cast<int>(host.state()),
            host.lastError());
        for ( std::size_t index = 0; index < guests.size(); ++index ) {
            XERROR("Guest {} state={}, peer={}, participants={}, error={}",
                   index + 1U,
                   static_cast<int>(guests[index]->state()),
                   guests[index]->localPeerId(),
                   guests[index]->participants().size(),
                   guests[index]->lastError());
            for ( const auto& entry : guests[index]->logs() ) {
                XERROR("Guest {} log type={} detail={}",
                       index + 1U,
                       static_cast<int>(entry.type),
                       entry.detail);
            }
        }
        return false;
    }
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        guests[index]->onBeatmapSynchronized(*guestModels[index]);
        guestApplyFlags[index].clear();
        guestApplyObjectDeltas[index].clear();
        guestApplyMutationSequences[index].clear();
        guestAcknowledgedMutationSequences[index].clear();
    }

    auto  hostConcurrent      = makeBeatmap(1000.0, "Host Creator");
    auto& hostAddedNote       = hostConcurrent->m_noteData.notes.emplace_back();
    hostAddedNote.m_timestamp = 3000.0;
    hostAddedNote.m_track     = 0;
    hostAddedNote.m_collaborationId = "host-concurrent-note";
    hostConcurrent->sync();
    auto  guestConcurrent = makeBeatmap(1000.0, "Host Creator");
    auto& guestAddedNote  = guestConcurrent->m_noteData.notes.emplace_back();
    guestAddedNote.m_timestamp       = 4000.0;
    guestAddedNote.m_track           = 1;
    guestAddedNote.m_collaborationId = "guest-concurrent-note";
    guestConcurrent->sync();
    hostModel           = hostConcurrent;
    guestModels.front() = guestConcurrent;
    static_cast<void>(
        host.onBeatmapMutated(*hostConcurrent, BeatmapMutationFlags::Objects));
    static_cast<void>(guests.front()->onBeatmapMutated(
        *guestConcurrent, BeatmapMutationFlags::Objects));
    const bool concurrentEditsConverged =
        pumpUntil(server, host, guests, [&]() {
            return hasRootNoteTimestamps(hostModel,
                                         { 1000.0, 3000.0, 4000.0 }) &&
                   hasRootNoteTimestamps(guestModels.front(),
                                         { 1000.0, 3000.0, 4000.0 });
        });
    if ( !concurrentEditsConverged ) {
        XERROR(
            "Concurrent host and guest object additions overwrote each other");
        return false;
    }
    if ( guestApplyFlags.front().empty() ||
         std::any_of(guestApplyFlags.front().begin(),
                     guestApplyFlags.front().end(),
                     [](BeatmapMutationFlags flags) {
                         return flags != BeatmapMutationFlags::Objects;
                     }) ) {
        XERROR("Concurrent object rebase widened its replacement flags");
        return false;
    }
    const bool backgroundDeltaDelivered =
        GUEST_COUNT < 2U ||
        std::any_of(
            guestApplyObjectDeltas[1].begin(),
            guestApplyObjectDeltas[1].end(),
            [](const auto& delta) {
                return delta &&
                       std::find(delta->begin(),
                                 delta->end(),
                                 "host-concurrent-note") != delta->end() &&
                       std::find(delta->begin(),
                                 delta->end(),
                                 "guest-concurrent-note") != delta->end();
            });
    if ( !backgroundDeltaDelivered ) {
        XERROR("Remote object updates did not expose background ID deltas");
        return false;
    }
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    constexpr std::size_t CONCURRENT_BURST_COUNT = 32;
    auto                  hostBurst = makeBeatmap(1000.0, "Host Creator");
    auto& hostBurstEarlier       = hostBurst->m_noteData.notes.emplace_back();
    hostBurstEarlier.m_timestamp = 3000.0;
    hostBurstEarlier.m_track     = 0;
    hostBurstEarlier.m_collaborationId = "host-concurrent-note";
    auto& hostBurstGuestEarlier = hostBurst->m_noteData.notes.emplace_back();
    hostBurstGuestEarlier.m_timestamp       = 4000.0;
    hostBurstGuestEarlier.m_track           = 1;
    hostBurstGuestEarlier.m_collaborationId = "guest-concurrent-note";
    auto guestBurst                      = makeBeatmap(1000.0, "Host Creator");
    guestBurst->m_noteData.notes         = hostBurst->m_noteData.notes;
    std::uint64_t lastGuestBurstSequence = 0;
    for ( std::size_t index = 0; index < CONCURRENT_BURST_COUNT; ++index ) {
        auto& hostNote             = hostBurst->m_noteData.notes.emplace_back();
        hostNote.m_timestamp       = 5000.0 + static_cast<double>(index);
        hostNote.m_track           = static_cast<std::uint32_t>(index % 6U);
        hostNote.m_collaborationId = "host-burst-" + std::to_string(index);
        hostBurst->sync();
        static_cast<void>(
            host.onBeatmapMutated(*hostBurst, BeatmapMutationFlags::Objects));

        auto& guestNote       = guestBurst->m_noteData.notes.emplace_back();
        guestNote.m_timestamp = 6000.0 + static_cast<double>(index);
        guestNote.m_track     = static_cast<std::uint32_t>(index % 6U);
        guestNote.m_collaborationId = "guest-burst-" + std::to_string(index);
        guestBurst->sync();
        lastGuestBurstSequence = guests.front()->onBeatmapMutated(
            *guestBurst, BeatmapMutationFlags::Objects);
    }
    hostModel                              = hostBurst;
    guestModels.front()                    = guestBurst;
    guestRequiredMutationSequences.front() = lastGuestBurstSequence;
    if ( lastGuestBurstSequence == 0 ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return hasConcurrentBurstNotes(
                                   hostModel, CONCURRENT_BURST_COUNT) &&
                               hasConcurrentBurstNotes(guestModels.front(),
                                                       CONCURRENT_BURST_COUNT);
                    }) ||
         countLogDetails(host, "invalid_beatmap_document") != 0U ||
         countLogDetails(*guests.front(), "invalid_beatmap_document") != 0U ) {
        XERROR("Concurrent object burst failed to converge without errors");
        return false;
    }
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    auto hostReset      = makeBeatmap(1000.0, "Host Creator");
    auto guestReset     = makeBeatmap(1000.0, "Host Creator");
    hostModel           = hostReset;
    guestModels.front() = guestReset;
    static_cast<void>(
        host.onBeatmapMutated(*hostReset, BeatmapMutationFlags::Objects));
    static_cast<void>(guests.front()->onBeatmapMutated(
        *guestReset, BeatmapMutationFlags::Objects));
    if ( !pumpUntil(server, host, guests, [&]() {
             return hasExpectedState(hostModel, 1000.0, "Host Creator") &&
                    hasExpectedState(
                        guestModels.front(), 1000.0, "Host Creator");
         }) ) {
        XERROR("Concurrent object cleanup did not converge");
        return false;
    }
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    const auto guestApplyCountBeforeLocalEdit = guestApplyCounts.front();
    auto       guestEdit                 = makeBeatmap(1250.0, "Host Creator");
    guestModels.front()                  = guestEdit;
    const auto guestEditMutationSequence = guests.front()->onBeatmapMutated(
        *guestEdit, BeatmapMutationFlags::Objects);
    guestRequiredMutationSequences.front() = guestEditMutationSequence;
    const bool guestEditConverged = pumpUntil(server, host, guests, [&]() {
        if ( !hasExpectedState(hostModel, 1250.0, "Host Creator") ) {
            return false;
        }
        const auto appliedSequence =
            guestApplyMutationSequences.front().empty()
                ? 0
                : *std::max_element(guestApplyMutationSequences.front().begin(),
                                    guestApplyMutationSequences.front().end());
        const auto acknowledgedSequence =
            guestAcknowledgedMutationSequences.front().empty()
                ? 0
                : *std::max_element(
                      guestAcknowledgedMutationSequences.front().begin(),
                      guestAcknowledgedMutationSequences.front().end());
        return std::max(appliedSequence, acknowledgedSequence) >=
                   guestEditMutationSequence &&
               std::all_of(guestModels.begin(),
                           guestModels.end(),
                           [](const auto& model) {
                               return hasExpectedState(
                                   model, 1250.0, "Host Creator");
                           });
    });
    const auto latestGuestAppliedMutationSequence = std::max(
        guestApplyMutationSequences.front().empty()
            ? 0
            : *std::max_element(guestApplyMutationSequences.front().begin(),
                                guestApplyMutationSequences.front().end()),
        guestAcknowledgedMutationSequences.front().empty()
            ? 0
            : *std::max_element(
                  guestAcknowledgedMutationSequences.front().begin(),
                  guestAcknowledgedMutationSequences.front().end()));
    if ( !guestEditConverged || guestEditMutationSequence == 0 ||
         latestGuestAppliedMutationSequence < guestEditMutationSequence ) {
        XERROR(
            "Guest edit failed to converge or acknowledge its local view: "
            "participants={}, applyBefore={}, applyAfter={}, mutation={}, "
            "included={}",
            host.participants().size(),
            guestApplyCountBeforeLocalEdit,
            guestApplyCounts.front(),
            guestEditMutationSequence,
            latestGuestAppliedMutationSequence);
        return false;
    }

    auto hostEdit = makeBeatmap(1250.0, "Host Revised");
    hostModel     = hostEdit;
    static_cast<void>(
        host.onBeatmapMutated(*hostEdit, BeatmapMutationFlags::Metadata));
    const bool hostEditConverged = pumpUntil(server, host, guests, [&]() {
        if ( !hasExpectedState(hostModel, 1250.0, "Host Revised") ) {
            return false;
        }
        return std::all_of(
            guestModels.begin(), guestModels.end(), [](const auto& model) {
                return hasExpectedState(model, 1250.0, "Host Revised");
            });
    });
    if ( !hostEditConverged || hostApplyCount < 3U ||
         guestApplyCounts.front() < 4U ||
         !std::all_of(
             guestApplyCounts.begin() + 1,
             guestApplyCounts.end(),
             [](std::size_t count) { return count == 3U; }) ||
         countLogs(host, CollaborationLogEventType::OperationCommitted) < 3U ) {
        XERROR(
            "Host edit validation failed: converged={}, hostApply={}, "
            "firstGuestApply={}, hostLogs={}",
            hostEditConverged,
            hostApplyCount,
            guestApplyCounts.front(),
            countLogs(host, CollaborationLogEventType::OperationCommitted));
        return false;
    }

    for ( auto& guest : guests ) guest->disconnect();
    return pumpUntil(server, host, guests, [&]() {
        return host.participants().size() == 1U;
    });
}

/// @brief 验证房主可以拒绝待审批访客并移出已经获准的访客。
bool testHostAdmissionControl()
{
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Admission Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Admission Test";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    const auto failAdmission = [&host, &guests](std::string_view phase) {
        const CollaborationRoom* guest =
            guests.empty() ? nullptr : guests.back().get();
        XERROR(
            "Admission control test failed at {}: host_state={}, "
            "host_participants={}, pending={}, guest_state={}, guest_error={}",
            phase,
            static_cast<int>(host.state()),
            host.participants().size(),
            host.pendingJoinRequests().size(),
            guest ? static_cast<int>(guest->state()) : -1,
            guest ? guest->lastError() : std::string{});
        return false;
    };
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return failAdmission("publish");
    }

    std::size_t nextGuestIdentity = 2;
    auto        makeGuest         = [&](std::string creator) {
        auto guest = std::make_unique<CollaborationRoom>();
        CollaborationJoinRoomConfig config;
        config.creator       = std::move(creator);
        config.participantId = makeRoomTestParticipantId(nextGuestIdentity++);
        config.roomId        = host.roomId();
        config.roomName      = host.roomName();
        config.endpoint      = endpoint;
        if ( !guest->join(std::move(config)) ) {
            return std::unique_ptr<CollaborationRoom>{};
        }
        return guest;
    };

    guests.push_back(makeGuest("Rejected Guest"));
    if ( !guests.back() ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.back()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               host.pendingJoinRequests().size() == 1U;
                    }) ||
         host.participants().size() != 1U ||
         !host.rejectJoinRequest(
             host.pendingJoinRequests().front().requestId) ||
         !pumpUntil(server, host, guests, [&]() {
             return guests.back()->state() == CollaborationRoomState::Error &&
                    guests.back()->lastError() == "host_rejected" &&
                    host.pendingJoinRequests().empty();
         }) ) {
        return failAdmission("reject");
    }
    guests.back()->disconnect();
    guests.clear();

    guests.push_back(makeGuest("Approved Guest"));
    if ( !guests.back() ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.back()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               host.pendingJoinRequests().size() == 1U;
                    }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ||
         !pumpUntil(server, host, guests, [&]() {
             return guests.back()->state() ==
                        CollaborationRoomState::Connected &&
                    host.participants().size() == 2U;
         }) ) {
        return failAdmission("approve");
    }

    auto sharedBeatmap = makeBeatmap(1000.0, "Admission Host");
    static_cast<void>(
        host.onBeatmapMutated(*sharedBeatmap, BeatmapMutationFlags::All));
    if ( !pumpUntil(server, host, guests, [&]() {
             return countLogs(*guests.back(),
                              CollaborationLogEventType::OperationCommitted) >=
                    1U;
         }) ) {
        return failAdmission("initial_document");
    }

    const PeerId guestPeerId = guests.back()->localPeerId();
    if ( guestPeerId == 0 || !host.removeParticipant(guestPeerId) ) {
        return failAdmission("remove_start");
    }
    if ( !pumpUntil(server, host, guests, [&]() {
             return host.participants().size() == 1U &&
                    guests.back()->state() == CollaborationRoomState::Error;
         }) ) {
        return failAdmission("remove_complete");
    }

    const auto submitFailuresBefore =
        countLogDetails(*guests.back(), "local_operation_submit_failed");
    sharedBeatmap->m_noteData.notes.front().m_timestamp = 1250.0;
    sharedBeatmap->sync();
    static_cast<void>(guests.back()->onBeatmapMutated(
        *sharedBeatmap, BeatmapMutationFlags::Objects));
    guests.back()->update();
    if ( countLogDetails(*guests.back(), "local_operation_submit_failed") !=
         submitFailuresBefore ) {
        return failAdmission("post_disconnect_mutation");
    }
    return true;
}

/// @brief 验证资源清单和多分块通过真实 DataChannel 到达访客并完成校验。
bool testOneGuestResourceSync()
{
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    ScopedRoomResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto      projectRoot = directory.path() / "host";
    const auto      audioPath   = projectRoot / "audio/main.bin";
    std::error_code error;
    std::filesystem::create_directories(audioPath.parent_path(), error);
    if ( error ) return false;
    std::vector<std::uint8_t> expected(150003U);
    for ( std::size_t index = 0; index < expected.size(); ++index ) {
        expected[index] = static_cast<std::uint8_t>((index * 73U) % 251U);
    }
    std::ofstream output(audioPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(expected.data()),
                 static_cast<std::streamsize>(expected.size()));
    output.close();
    if ( !output ) return false;

    MMM::Project project;
    project.m_projectRoot = projectRoot;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "main-id",
        .m_path = "audio/main.bin",
        .m_type = MMM::AudioTrackType::Main,
    });
    BeatMap beatmap;
    beatmap.m_audioSamples.emplace_back().m_audioResourceId = "main-id";

    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Resource Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Resource Test";
    hostConfig.endpoint      = endpoint;
    host.prepareHostResources(project, beatmap);
    if ( !host.startHost(hostConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return false;
    }

    std::optional<CollaborationResourceBundle> receivedBundle;
    auto guest = std::make_unique<CollaborationRoom>();
    guest->setResourceBundleCallback(
        [&receivedBundle](CollaborationResourceBundle bundle) {
            receivedBundle = std::move(bundle);
        });
    CollaborationJoinRoomConfig joinConfig;
    joinConfig.creator           = "Resource Guest";
    joinConfig.participantId     = makeRoomTestParticipantId(2);
    joinConfig.roomId            = host.roomId();
    joinConfig.roomName          = host.roomName();
    joinConfig.endpoint          = endpoint;
    joinConfig.resourceCacheRoot = directory.path() / "cache";
    if ( !guest->join(joinConfig) ) return false;
    guests.push_back(std::move(guest));
    if ( !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.front()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               !host.pendingJoinRequests().empty();
                    }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ) {
        return false;
    }
    if ( !pumpUntil(server, host, guests, [&]() {
             return receivedBundle.has_value() &&
                    guests.front()->state() ==
                        CollaborationRoomState::Connected;
         }) ) {
        XERROR(
            "Resource collaboration timeout: host participants={}, guest "
            "state={}, error={}, resource phase={}",
            host.participants().size(),
            static_cast<int>(guests.front()->state()),
            guests.front()->lastError(),
            static_cast<int>(guests.front()->resourceProgress().phase));
        for ( const auto& entry : guests.front()->logs() ) {
            XERROR("Resource guest log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        return false;
    }
    if ( !receivedBundle->project ||
         receivedBundle->project->m_audioResources.size() != 1U ||
         countLogs(host, CollaborationLogEventType::ResourceManifest) != 1U ||
         countLogs(*guests.front(),
                   CollaborationLogEventType::ResourceCompleted) != 1U ) {
        return false;
    }
    const auto& receivedResource =
        receivedBundle->project->m_audioResources.front();
    std::ifstream input(
        receivedBundle->project->m_projectRoot / receivedResource.m_path,
        std::ios::binary | std::ios::ate);
    if ( !input ||
         input.tellg() != static_cast<std::streamoff>(expected.size()) ) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> actual(expected.size());
    input.read(reinterpret_cast<char*>(actual.data()),
               static_cast<std::streamsize>(actual.size()));
    if ( input.gcount() != static_cast<std::streamsize>(actual.size()) ||
         actual != expected ) {
        return false;
    }

    const auto replacementRoot = directory.path() / "replacement-host";
    const auto replacementPath = replacementRoot / "audio/replacement.bin";
    std::filesystem::create_directories(replacementPath.parent_path(), error);
    if ( error ) return false;
    std::vector<std::uint8_t> replacementExpected(90017U);
    for ( std::size_t index = 0; index < replacementExpected.size(); ++index ) {
        replacementExpected[index] =
            static_cast<std::uint8_t>((index * 41U + 17U) % 253U);
    }
    std::ofstream replacementOutput(replacementPath,
                                    std::ios::binary | std::ios::trunc);
    replacementOutput.write(
        reinterpret_cast<const char*>(replacementExpected.data()),
        static_cast<std::streamsize>(replacementExpected.size()));
    replacementOutput.close();
    if ( !replacementOutput ) return false;

    MMM::Project replacementProject;
    replacementProject.m_projectRoot = replacementRoot;
    replacementProject.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "replacement-main-id",
        .m_path = "audio/replacement.bin",
        .m_type = MMM::AudioTrackType::Main,
    });
    BeatMap replacementBeatmap;
    replacementBeatmap.m_audioSamples.emplace_back().m_audioResourceId =
        "replacement-main-id";
    receivedBundle.reset();
    host.prepareHostResources(replacementProject, replacementBeatmap);
    if ( !pumpUntil(
             server,
             host,
             guests,
             [&]() {
                 return receivedBundle && receivedBundle->project &&
                        receivedBundle->project->m_audioResources.size() ==
                            1U &&
                        receivedBundle->project->m_audioResources.front()
                                .m_id == "replacement-main-id";
             }) ||
         countLogs(host, CollaborationLogEventType::ResourceManifest) != 2U ||
         countLogs(*guests.front(),
                   CollaborationLogEventType::ResourceCompleted) != 2U ) {
        return false;
    }

    const auto& replacementResource =
        receivedBundle->project->m_audioResources.front();
    std::ifstream replacementInput(
        receivedBundle->project->m_projectRoot / replacementResource.m_path,
        std::ios::binary | std::ios::ate);
    if ( !replacementInput ||
         replacementInput.tellg() !=
             static_cast<std::streamoff>(replacementExpected.size()) ) {
        return false;
    }
    replacementInput.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> replacementActual(replacementExpected.size());
    replacementInput.read(
        reinterpret_cast<char*>(replacementActual.data()),
        static_cast<std::streamsize>(replacementActual.size()));
    return replacementInput.gcount() ==
               static_cast<std::streamsize>(replacementActual.size()) &&
           replacementActual == replacementExpected;
}

/// @brief 驱动公网目录、房主和访客，直到条件满足或超时。
template<typename Predicate>
bool pumpExternalUntil(
    CollaborationRoom& host, CollaborationRoom& guest,
    MMM::Network::Collaboration::CollaborationDirectoryClient& directory,
    Predicate                                                  predicate)
{
    const auto deadline =
        std::chrono::steady_clock::now() + EXTERNAL_TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        directory.update();
        host.update();
        guest.update();
        host.update();
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// @brief 验证部署后的房间发布、目录发现、加入配对和 DataChannel 建立。
bool testExternalPublicDirectoryWebRtcRoom(CollaborationServerEndpoint endpoint)
{
    using MMM::Network::Collaboration::CollaborationDirectoryClient;
    using MMM::Network::Collaboration::CollaborationDirectoryState;

    CollaborationDirectoryClient directory;
    if ( !directory.connect(endpoint) ) return false;

    CollaborationRoom host;
    CollaborationRoom guest;
    if ( !pumpExternalUntil(host, guest, directory, [&]() {
             return directory.state() == CollaborationDirectoryState::Connected;
         }) ) {
        XERROR("External directory bootstrap failed: state={}, error={}",
               static_cast<int>(directory.state()),
               directory.lastError());
        return false;
    }

    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Deployment Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Public Deployment Probe";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;

    const bool roomDiscovered =
        pumpExternalUntil(host, guest, directory, [&]() {
            if ( host.roomId().empty() ||
                 directory.state() != CollaborationDirectoryState::Connected ) {
                return false;
            }
            return std::any_of(directory.rooms().begin(),
                               directory.rooms().end(),
                               [&host](const auto& room) {
                                   return room.roomId == host.roomId();
                               });
        });
    if ( !roomDiscovered ) {
        XERROR(
            "External room discovery failed: host state={}, host error={}, "
            "directory state={}, directory error={}",
            static_cast<int>(host.state()),
            host.lastError(),
            static_cast<int>(directory.state()),
            directory.lastError());
        return false;
    }

    const auto roomIterator = std::find_if(
        directory.rooms().begin(),
        directory.rooms().end(),
        [&host](const auto& room) { return room.roomId == host.roomId(); });
    if ( roomIterator == directory.rooms().end() ) return false;
    CollaborationJoinRoomConfig guestConfig;
    guestConfig.creator       = "Deployment Guest";
    guestConfig.participantId = makeRoomTestParticipantId(2);
    guestConfig.roomId        = roomIterator->roomId;
    guestConfig.roomName      = roomIterator->roomName;
    guestConfig.endpoint      = endpoint;
    if ( !guest.join(guestConfig) ) return false;

    if ( !pumpExternalUntil(
             host,
             guest,
             directory,
             [&]() {
                 return guest.state() ==
                            CollaborationRoomState::AwaitingApproval &&
                        !host.pendingJoinRequests().empty();
             }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ) {
        return false;
    }

    const bool connected = pumpExternalUntil(host, guest, directory, [&]() {
        return guest.state() == CollaborationRoomState::Connected &&
               host.participants().size() == 2U &&
               guest.participants().size() == 2U;
    });
    if ( !connected ) {
        XERROR(
            "External P2P connection failed: host state={}, participants={}, "
            "error={}; guest state={}, participants={}, error={}",
            static_cast<int>(host.state()),
            host.participants().size(),
            host.lastError(),
            static_cast<int>(guest.state()),
            guest.participants().size(),
            guest.lastError());
        for ( const auto& entry : host.logs() ) {
            XERROR("External host log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        for ( const auto& entry : guest.logs() ) {
            XERROR("External guest log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        return false;
    }

    guest.disconnect();
    const bool guestRemoved = pumpExternalUntil(host, guest, directory, [&]() {
        return host.participants().size() == 1U;
    });
    host.disconnect();
    directory.disconnect();
    return guestRemoved;
}

}  // namespace

int main(int argc, char** argv)
{
    if ( argc < 2 || !argv[1] ) return 3;
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();
    if ( !MMM::Network::Collaboration::
             startCollaborationBuildFingerprintInitialization() ) {
        return 4;
    }
    const auto initialFingerprintState =
        MMM::Network::Collaboration::collaborationBuildFingerprintState();
    if ( initialFingerprintState !=
             MMM::Network::Collaboration::CollaborationBuildFingerprintState::
                 Calculating &&
         initialFingerprintState !=
             MMM::Network::Collaboration::CollaborationBuildFingerprintState::
                 Ready ) {
        return 4;
    }
    constexpr auto FINGERPRINT_TIMEOUT = std::chrono::seconds(30);
    const auto     fingerprintDeadline =
        std::chrono::steady_clock::now() + FINGERPRINT_TIMEOUT;
    while ( MMM::Network::Collaboration::collaborationBuildFingerprintState() ==
                MMM::Network::Collaboration::
                    CollaborationBuildFingerprintState::Calculating &&
            std::chrono::steady_clock::now() < fingerprintDeadline ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if ( MMM::Network::Collaboration::collaborationBuildFingerprintState() !=
         MMM::Network::Collaboration::CollaborationBuildFingerprintState::
             Ready ) {
        return 4;
    }
    if ( !MMM::Network::Collaboration::isValidCollaborationBuildFingerprint(
             MMM::Network::Collaboration::collaborationBuildFingerprint()) ) {
        return 4;
    }
    const std::string_view mode(argv[1]);
    int                    result = 3;
    if ( mode == "p2p" && argc == 2 ) {
        result = testPublicDirectoryWebRtcRoom() && testHostAdmissionControl()
                     ? 0
                     : 1;
    } else if ( mode == "resource" && argc == 2 ) {
        result = testOneGuestResourceSync() ? 0 : 2;
    } else if ( mode == "external" && argc == 5 ) {
        std::uint32_t          port = 0;
        const std::string_view portText(argv[3]);
        const auto [end, error] = std::from_chars(
            portText.data(), portText.data() + portText.size(), port);
        if ( error != std::errc{} || end != portText.data() + portText.size() ||
             port == 0 || port > 65535 ) {
            result = 3;
        } else {
            const std::string_view tlsText(argv[4]);
            if ( tlsText == "true" || tlsText == "false" ) {
                CollaborationServerEndpoint endpoint;
                endpoint.address       = argv[2];
                endpoint.signalingPort = static_cast<std::uint16_t>(port);
                endpoint.useTls        = tlsText == "true";
                XLogger::init("CollaborationRoomTest");
                const bool success =
                    testExternalPublicDirectoryWebRtcRoom(std::move(endpoint));
                XLogger::shutdown();
                result = success ? 0 : 1;
            }
        }
    }
    appThreadPool.shutdown();
    return result;
}
