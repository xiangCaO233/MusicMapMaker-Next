#include "network/collaboration/BeatmapDocumentCodec.h"

#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <memory>
#include <string>

namespace
{
using MMM::BeatMap;
using MMM::BeatmapMutationFlags;
using MMM::Network::Collaboration::BeatmapDocumentCodec;
using MMM::Network::Collaboration::BeatmapDocumentError;

/// @brief 构造覆盖全部协作谱面数据类别的领域对象。
std::shared_ptr<BeatMap> makeCompleteBeatmap(std::string author)
{
    auto  beatmap        = std::make_shared<BeatMap>();
    auto& base           = beatmap->m_baseMapMetadata;
    base.name            = "Codec Test";
    base.title           = "Title";
    base.artist          = "Artist";
    base.version         = "Hard";
    base.author          = std::move(author);
    base.preference_bpm  = 180.0;
    base.track_count     = 6;
    base.bgm_track_count = 2;
    base.map_length      = 120000.0;
    beatmap->m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        "key";

    auto& note           = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp     = 1000.0;
    note.m_track         = 1;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "tap.wav", 0.8F };
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["color"] =
        "#112233";

    auto& flick       = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp = 1500.0;
    flick.m_track     = 2;
    flick.m_dtrack    = -1;

    auto& subHold        = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp  = 2000.0;
    subHold.m_duration   = 500.0;
    subHold.m_track      = 3;
    subHold.m_isSubNote  = true;
    auto& subFlick       = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp = 2500.0;
    subFlick.m_track     = 3;
    subFlick.m_dtrack    = 2;
    subFlick.m_isSubNote = true;
    auto& polyline       = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp = subHold.m_timestamp;
    polyline.m_track     = subHold.m_track;
    polyline.m_subNotes.emplace_back(subHold);
    polyline.m_subNotes.emplace_back(subFlick);
    polyline.m_subHolds.emplace_back(subHold);
    polyline.m_subFlicks.emplace_back(subFlick);

    auto& timing                   = beatmap->m_timings.emplace_back();
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 180.0;
    timing.m_beat_length           = 1000.0 / 3.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 180.0;
    timing.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["beat"] = "0,0,1";

    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 750.0;
    sample.m_offsetMs        = -25;
    sample.m_track           = 6;
    sample.m_audioResourceId = "bgm.wav";
    sample.m_volume          = 0.65F;
    sample.m_metadata.sample_properties[MMM::SampleMetadataType::MMM]["lane"] =
        "0";
    beatmap->sync();
    return beatmap;
}

/// @brief 校验完整快照往返后仍保留全部数据类型及引用关系。
bool testCompleteSnapshotRoundTrip()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec receiver;
    const auto           source = makeCompleteBeatmap("Creator A");
    auto snapshot = encoder.encode(*source, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() || snapshot->empty() ) return false;
    auto applied = receiver.apply(snapshot.value());
    if ( !applied.has_value() || !applied->isSnapshot ||
         applied->flags != BeatmapMutationFlags::All ) {
        return false;
    }
    const auto restored = receiver.materialize();
    if ( !restored || restored->m_noteData.notes.size() != 1U ||
         restored->m_noteData.holds.size() != 1U ||
         restored->m_noteData.flicks.size() != 2U ||
         restored->m_noteData.polylines.size() != 1U ||
         restored->m_timings.size() != 1U ||
         restored->m_audioSamples.size() != 1U ) {
        return false;
    }
    const auto& polyline = restored->m_noteData.polylines.front();
    return polyline.m_subNotes.size() == 2U &&
           polyline.m_subHolds.size() == 1U &&
           polyline.m_subFlicks.size() == 1U &&
           restored->m_noteData.notes.front().m_sampleBinding.has_value() &&
           restored->m_noteData.notes.front()
                   .m_metadata.note_properties.at(MMM::NoteMetadataType::MMM)
                   .at("color") == "#112233" &&
           restored->m_audioSamples.front().m_offsetMs == -25 &&
           std::abs(restored->m_timings.front().m_bpm - 180.0) < 1e-9;
}

/// @brief 校验元数据增量小于完整快照且不会覆盖其它谱面类别。
bool testCategoryDelta()
{
    BeatmapDocumentCodec codec;
    auto                 initial = makeCompleteBeatmap("Creator A");
    auto snapshot = codec.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() || !codec.apply(snapshot.value()).has_value() ) {
        return false;
    }

    auto edited = makeCompleteBeatmap("Creator B");
    auto delta  = codec.encode(*edited, BeatmapMutationFlags::Metadata, false);
    if ( !delta.has_value() || delta->size() >= snapshot->size() ) return false;
    auto applied = codec.apply(delta.value());
    if ( !applied.has_value() || applied->isSnapshot ||
         applied->flags != BeatmapMutationFlags::Metadata ) {
        return false;
    }
    auto restored = codec.materialize();
    return restored && restored->m_baseMapMetadata.author == "Creator B" &&
           restored->m_noteData.notes.size() == 1U &&
           restored->m_noteData.notes.front().m_timestamp == 1000.0 &&
           restored->m_audioSamples.size() == 1U;
}

/// @brief 校验没有基础快照时拒绝增量和畸形 CBOR。
bool testInvalidPayloads()
{
    BeatmapDocumentCodec codec;
    auto                 beatmap = makeCompleteBeatmap("Creator");
    auto delta = codec.encode(*beatmap, BeatmapMutationFlags::Objects, false);
    if ( !delta.has_value() ) return false;
    auto missingSnapshot = codec.apply(delta.value());
    if ( missingSnapshot.has_value() ||
         missingSnapshot.error() != BeatmapDocumentError::MissingSnapshot ) {
        return false;
    }
    const MMM::Network::Collaboration::ByteBuffer malformed{ 0xFF, 0x00 };
    auto malformedResult = codec.apply(malformed);
    return !malformedResult.has_value() &&
           malformedResult.error() == BeatmapDocumentError::InvalidPayload;
}

/// @brief 校验大谱面快照经过压缩后仍能在单条协作消息上限内往返。
bool testLargeSnapshotCompression()
{
    BeatmapDocumentCodec  encoder;
    BeatmapDocumentCodec  receiver;
    auto                  beatmap = makeCompleteBeatmap("Large Creator");
    constexpr std::size_t LARGE_NOTE_COUNT = 20000;
    beatmap->m_noteData.notes.clear();
    for ( std::size_t index = 0; index < LARGE_NOTE_COUNT; ++index ) {
        auto& note       = beatmap->m_noteData.notes.emplace_back();
        note.m_timestamp = static_cast<double>(index) * 25.0;
        note.m_track     = static_cast<int>(index % 6U);
    }
    beatmap->sync();

    auto snapshot = encoder.encode(*beatmap, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() || snapshot->size() >= 1024U * 1024U ) {
        return false;
    }
    auto applied = receiver.apply(snapshot.value());
    if ( !applied.has_value() || !applied->isSnapshot ) return false;
    const auto restored = receiver.materialize();
    return restored && restored->m_noteData.notes.size() == LARGE_NOTE_COUNT &&
           restored->m_noteData.notes.back().m_track == 1;
}
}  // namespace

int main()
{
    return testCompleteSnapshotRoundTrip() && testCategoryDelta() &&
                   testInvalidPayloads() && testLargeSnapshotCompression()
               ? 0
               : 1;
}
