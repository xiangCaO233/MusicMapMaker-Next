#include "network/collaboration/CollaborationRoom.h"

#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
using MMM::Network::Collaboration::CollaborationResourceBundle;

/// @brief 本机 WebRTC 集成测试允许的最长等待时间。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(15);
/// @brief 验收房间中的访客数量。
constexpr std::size_t GUEST_COUNT = 7;

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

    auto& note           = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp     = noteTimestamp;
    note.m_track         = 2;
    note.m_sampleBinding = MMM::AudioSampleBinding{
        .m_audioResourceId = "tap.wav",
        .m_volume          = 0.45F,
    };
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["transport"] =
        "preserved";

    auto& hold       = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp = 2000.0;
    hold.m_duration  = 600.0;
    hold.m_track     = 3;

    auto& subHold        = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp  = 2800.0;
    subHold.m_duration   = 400.0;
    subHold.m_track      = 1;
    subHold.m_isSubNote  = true;
    auto& subFlick       = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp = 3200.0;
    subFlick.m_track     = 1;
    subFlick.m_dtrack    = 2;
    subFlick.m_isSubNote = true;
    auto& polyline       = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp = subHold.m_timestamp;
    polyline.m_track     = subHold.m_track;
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
    std::size_t                    hostApplyCount = 0;
    host.setApplyBeatmapCallback(
        [&hostModel, &hostApplyCount](std::shared_ptr<const BeatMap> beatmap,
                                      BeatmapMutationFlags) {
            hostModel = std::move(beatmap);
            ++hostApplyCount;
        });
    auto initial = makeBeatmap(1000.0, "Host Creator");
    hostModel    = initial;
    host.onBeatmapMutated(*initial, BeatmapMutationFlags::All);
    host.update();
    if ( !hasExpectedState(hostModel, 1000.0, "Host Creator") ||
         hostApplyCount != 0U ) {
        return false;
    }

    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    std::vector<std::shared_ptr<const BeatMap>>     guestModels(GUEST_COUNT);
    std::vector<std::size_t> guestApplyCounts(GUEST_COUNT);
    guests.reserve(GUEST_COUNT);
    for ( std::size_t index = 0; index < GUEST_COUNT; ++index ) {
        auto guest = std::make_unique<CollaborationRoom>();
        guest->setApplyBeatmapCallback(
            [&guestModels, &guestApplyCounts, index](
                std::shared_ptr<const BeatMap> beatmap, BeatmapMutationFlags) {
                guestModels[index] = std::move(beatmap);
                ++guestApplyCounts[index];
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

    auto guestEdit      = makeBeatmap(1250.0, "Host Creator");
    guestModels.front() = guestEdit;
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
    hostModel     = hostEdit;
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
    if ( !hostEditConverged || hostApplyCount != 1U ||
         guestApplyCounts.front() != 2U ||
         !std::all_of(guestApplyCounts.begin() + 1,
                      guestApplyCounts.end(),
                      [](std::size_t count) { return count == 3U; }) ||
         countLogs(host, CollaborationLogEventType::OperationCommitted) < 3U ) {
        return false;
    }

    for ( auto& guest : guests ) guest->disconnect();
    return pumpUntil(
        host, guests, [&]() { return host.participants().size() == 1U; });
}

/// @brief 验证资源清单和多分块通过真实 DataChannel 到达访客并完成校验。
bool testOneGuestResourceSync()
{
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
    hostConfig.creator  = "Resource Host";
    hostConfig.port     = 0;
    hostConfig.roomCode = "RES234";
    host.prepareHostResources(project, beatmap);
    if ( !host.startHost(hostConfig) ) return false;

    std::optional<CollaborationResourceBundle> receivedBundle;
    auto guest = std::make_unique<CollaborationRoom>();
    guest->setResourceBundleCallback(
        [&receivedBundle](CollaborationResourceBundle bundle) {
            receivedBundle = std::move(bundle);
        });
    CollaborationJoinRoomConfig joinConfig;
    joinConfig.creator           = "Resource Guest";
    joinConfig.host              = "127.0.0.1";
    joinConfig.port              = host.port();
    joinConfig.roomCode          = host.roomCode();
    joinConfig.resourceCacheRoot = directory.path() / "cache";
    if ( !guest->join(joinConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    guests.push_back(std::move(guest));
    if ( !pumpUntil(host, guests, [&]() {
             return receivedBundle.has_value() &&
                    guests.front()->state() ==
                        CollaborationRoomState::Connected;
         }) ) {
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
    return input.gcount() == static_cast<std::streamsize>(actual.size()) &&
           actual == expected;
}
}  // namespace

int main()
{
    if ( !testEightClientLocalWebRtcRoom() ) return 1;
    if ( !testOneGuestResourceSync() ) return 2;
    return 0;
}
