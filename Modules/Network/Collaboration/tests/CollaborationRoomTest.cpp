#include "network/collaboration/CollaborationRoom.h"

#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
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

/// @brief 本机 WebRTC 集成测试允许的最长等待时间。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(15);
/// @brief 验收房间中的访客数量。
constexpr std::size_t GUEST_COUNT = 7;

/// @brief 构造覆盖物件、时间线、采样和元数据的协作谱面。
std::shared_ptr<BeatMap> makeBeatmap(double noteTimestamp, std::string author)
{
    auto beatmap                               = std::make_shared<BeatMap>();
    beatmap->m_baseMapMetadata.name            = "Collaboration Test";
    beatmap->m_baseMapMetadata.author          = std::move(author);
    beatmap->m_baseMapMetadata.track_count     = 6;
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    beatmap->m_baseMapMetadata.preference_bpm  = 150.0;

    auto& note       = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp = noteTimestamp;
    note.m_track     = 2;

    auto& hold       = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp = 2000.0;
    hold.m_duration  = 600.0;
    hold.m_track     = 3;

    auto& timing                   = beatmap->m_timings.emplace_back();
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 150.0;
    timing.m_beat_length           = 400.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 150.0;

    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 500.0;
    sample.m_track           = 6;
    sample.m_audioResourceId = "kick.wav";
    sample.m_volume          = 0.75F;
    beatmap->sync();
    return beatmap;
}

/// @brief 驱动房主和全部访客直到条件满足或超时。
template<typename Predicate>
bool pumpUntil(CollaborationRoom&                               host,
               std::vector<std::unique_ptr<CollaborationRoom>>& guests,
               Predicate                                        predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
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

/// @brief 判断谱面是否包含指定同步结果。
bool hasExpectedState(const std::shared_ptr<const BeatMap>& beatmap,
                      double noteTimestamp, std::string_view author)
{
    return beatmap && beatmap->m_noteData.notes.size() == 1U &&
           std::abs(beatmap->m_noteData.notes.front().m_timestamp -
                    noteTimestamp) < 1e-6 &&
           beatmap->m_noteData.holds.size() == 1U &&
           beatmap->m_timings.size() == 1U &&
           beatmap->m_audioSamples.size() == 1U &&
           beatmap->m_baseMapMetadata.author == author;
}

/// @brief 覆盖 1 房主 + 7 访客的真实 WebRTC、初始快照和双向增量收敛。
bool testEightClientLocalWebRtcRoom()
{
    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator  = "Host Creator";
    hostConfig.port     = 0;
    hostConfig.roomCode = "ABC234";
    if ( !host.startHost(hostConfig) || host.port() == 0 ) return false;

    std::shared_ptr<const BeatMap> hostModel;
    host.setApplyBeatmapCallback(
        [&hostModel](std::shared_ptr<const BeatMap> beatmap,
                     BeatmapMutationFlags) { hostModel = std::move(beatmap); });
    auto initial = makeBeatmap(1000.0, "Host Creator");
    host.onBeatmapMutated(*initial, BeatmapMutationFlags::All);
    host.update();
    if ( !hasExpectedState(hostModel, 1000.0, "Host Creator") ) return false;

    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    std::vector<std::shared_ptr<const BeatMap>>     guestModels(GUEST_COUNT);
    guests.reserve(GUEST_COUNT);
    for ( std::size_t index = 0; index < GUEST_COUNT; ++index ) {
        auto guest = std::make_unique<CollaborationRoom>();
        guest->setApplyBeatmapCallback(
            [&guestModels, index](std::shared_ptr<const BeatMap> beatmap,
                                  BeatmapMutationFlags) {
                guestModels[index] = std::move(beatmap);
            });
        CollaborationJoinRoomConfig guestConfig;
        guestConfig.creator  = "Guest " + std::to_string(index + 1);
        guestConfig.host     = "127.0.0.1";
        guestConfig.port     = host.port();
        guestConfig.roomCode = host.roomCode();
        if ( !guest->join(guestConfig) ) return false;
        guests.push_back(std::move(guest));
    }

    const bool connected = pumpUntil(host, guests, [&]() {
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
    if ( !connected ) return false;

    auto guestEdit = makeBeatmap(1250.0, "Host Creator");
    guests.front()->onBeatmapMutated(*guestEdit, BeatmapMutationFlags::Objects);
    const bool guestEditConverged = pumpUntil(host, guests, [&]() {
        if ( !hasExpectedState(hostModel, 1250.0, "Host Creator") ) {
            return false;
        }
        return std::all_of(
            guestModels.begin(), guestModels.end(), [](const auto& model) {
                return hasExpectedState(model, 1250.0, "Host Creator");
            });
    });
    if ( !guestEditConverged ) return false;

    auto hostEdit = makeBeatmap(1250.0, "Host Revised");
    host.onBeatmapMutated(*hostEdit, BeatmapMutationFlags::Metadata);
    const bool hostEditConverged = pumpUntil(host, guests, [&]() {
        if ( !hasExpectedState(hostModel, 1250.0, "Host Revised") ) {
            return false;
        }
        return std::all_of(
            guestModels.begin(), guestModels.end(), [](const auto& model) {
                return hasExpectedState(model, 1250.0, "Host Revised");
            });
    });
    if ( !hostEditConverged ||
         countLogs(host, CollaborationLogEventType::OperationCommitted) < 3U ) {
        return false;
    }

    for ( auto& guest : guests ) guest->disconnect();
    return pumpUntil(
        host, guests, [&]() { return host.participants().size() == 1U; });
}
}  // namespace

int main()
{
    return testEightClientLocalWebRtcRoom() ? 0 : 1;
}
