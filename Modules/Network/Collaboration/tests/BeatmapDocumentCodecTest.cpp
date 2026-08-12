#include "network/collaboration/BeatmapDocumentCodec.h"

#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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

    auto& note             = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp       = 1000.0;
    note.m_track           = 1;
    note.m_collaborationId = "note-root";
    note.m_annotation      = "检查节奏重音";
    note.m_sampleBinding   = MMM::AudioSampleBinding{ "tap.wav", 0.8F };
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["color"] =
        "#112233";

    auto& flick             = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp       = 1500.0;
    flick.m_track           = 2;
    flick.m_dtrack          = -1;
    flick.m_collaborationId = "flick-root";

    auto& subHold              = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp        = 2000.0;
    subHold.m_duration         = 500.0;
    subHold.m_track            = 3;
    subHold.m_isSubNote        = true;
    subHold.m_collaborationId  = "polyline-sub-hold";
    subHold.m_annotation       = "折线起段";
    auto& subFlick             = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp       = 2500.0;
    subFlick.m_track           = 3;
    subFlick.m_dtrack          = 2;
    subFlick.m_isSubNote       = true;
    subFlick.m_collaborationId = "polyline-sub-flick";
    auto& polyline             = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp       = subHold.m_timestamp;
    polyline.m_track           = subHold.m_track;
    polyline.m_collaborationId = "polyline-root";
    polyline.m_annotation      = "整条折线说明";
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
           restored->m_noteData.notes.front().m_annotation == "检查节奏重音" &&
           polyline.m_annotation == "整条折线说明" &&
           polyline.m_subNotes.front().get().m_annotation == "折线起段" &&
           restored->m_noteData.notes.front()
                   .m_metadata.note_properties.at(MMM::NoteMetadataType::MMM)
                   .at("color") == "#112233" &&
           restored->m_audioSamples.front().m_offsetMs == -25 &&
           std::abs(restored->m_timings.front().m_bpm - 180.0) < 1e-9;
}

/// @brief 校验 Polyline 实际引用是子物件身份的权威来源。
bool testPolylineReferencesPreventDuplicateRootObjects()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec receiver;
    const auto           source = makeCompleteBeatmap("Creator");
    source->m_noteData.holds.front().m_isSubNote = false;
    source->m_noteData.flicks.back().m_isSubNote = false;

    auto snapshot = encoder.encode(*source, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() ||
         !receiver.apply(snapshot.value()).has_value() ) {
        return false;
    }
    const auto restored = receiver.materialize();
    if ( !restored || restored->m_noteData.notes.size() != 1U ||
         restored->m_noteData.holds.size() != 1U ||
         restored->m_noteData.flicks.size() != 2U ||
         restored->m_noteData.polylines.size() != 1U ) {
        return false;
    }
    const auto& polyline = restored->m_noteData.polylines.front();
    return polyline.m_subNotes.size() == 2U &&
           polyline.m_subNotes[0].get().m_isSubNote &&
           polyline.m_subNotes[1].get().m_isSubNote;
}

/// @brief 判断两个谱面的物件类别是否逐字段等价。
bool sameObjects(const BeatMap& lhs, const BeatMap& rhs)
{
    const auto sameBinding = [](const auto& left, const auto& right) {
        if ( left.has_value() != right.has_value() ) return false;
        return !left || (left->m_audioResourceId == right->m_audioResourceId &&
                         std::abs(left->m_volume - right->m_volume) < 1e-6F);
    };
    const auto sameNote = [&](const MMM::Note& left, const MMM::Note& right) {
        if ( left.m_type != right.m_type ||
             std::abs(left.m_timestamp - right.m_timestamp) >= 1e-9 ||
             left.m_track != right.m_track ||
             left.m_isSubNote != right.m_isSubNote ||
             left.m_collaborationId != right.m_collaborationId ||
             left.m_metadata.note_properties !=
                 right.m_metadata.note_properties ||
             !sameBinding(left.m_sampleBinding, right.m_sampleBinding) ) {
            return false;
        }
        if ( left.m_type == MMM::NoteType::HOLD ) {
            return std::abs(static_cast<const MMM::Hold&>(left).m_duration -
                            static_cast<const MMM::Hold&>(right).m_duration) <
                   1e-9;
        }
        if ( left.m_type == MMM::NoteType::FLICK ) {
            return static_cast<const MMM::Flick&>(left).m_dtrack ==
                   static_cast<const MMM::Flick&>(right).m_dtrack;
        }
        return true;
    };
    const auto sameDeque = [&](const auto& left, const auto& right) {
        if ( left.size() != right.size() ) return false;
        for ( std::size_t index = 0; index < left.size(); ++index ) {
            if ( !sameNote(left[index], right[index]) ) return false;
        }
        return true;
    };
    if ( !sameDeque(lhs.m_noteData.notes, rhs.m_noteData.notes) ||
         !sameDeque(lhs.m_noteData.holds, rhs.m_noteData.holds) ||
         !sameDeque(lhs.m_noteData.flicks, rhs.m_noteData.flicks) ||
         lhs.m_noteData.polylines.size() != rhs.m_noteData.polylines.size() ) {
        return false;
    }
    for ( std::size_t index = 0; index < lhs.m_noteData.polylines.size();
          ++index ) {
        const auto& left  = lhs.m_noteData.polylines[index];
        const auto& right = rhs.m_noteData.polylines[index];
        if ( !sameNote(left, right) ||
             left.m_subNotes.size() != right.m_subNotes.size() ||
             left.m_subHolds.size() != right.m_subHolds.size() ||
             left.m_subFlicks.size() != right.m_subFlicks.size() ) {
            return false;
        }
        for ( std::size_t subIndex = 0; subIndex < left.m_subNotes.size();
              ++subIndex ) {
            if ( !sameNote(left.m_subNotes[subIndex].get(),
                           right.m_subNotes[subIndex].get()) ) {
                return false;
            }
        }
    }
    return true;
}

/// @brief 判断两个谱面的整物件及折线子物件注释是否等价。
bool sameAnnotations(const BeatMap& lhs, const BeatMap& rhs)
{
    const auto annotations = [](const BeatMap& beatmap) {
        std::unordered_map<std::string, std::string> result;
        const auto append = [&](const MMM::Note& note) {
            if ( !note.m_collaborationId.empty() ) {
                result[note.m_collaborationId] = note.m_annotation;
            }
        };
        for ( const auto& note : beatmap.m_noteData.notes ) append(note);
        for ( const auto& hold : beatmap.m_noteData.holds ) append(hold);
        for ( const auto& flick : beatmap.m_noteData.flicks ) append(flick);
        for ( const auto& polyline : beatmap.m_noteData.polylines ) {
            append(polyline);
        }
        return result;
    };
    return annotations(lhs) == annotations(rhs);
}

/// @brief 判断两个谱面的时间线类别是否逐字段等价。
bool sameTimelines(const BeatMap& lhs, const BeatMap& rhs)
{
    if ( lhs.m_timings.size() != rhs.m_timings.size() ) return false;
    for ( std::size_t index = 0; index < lhs.m_timings.size(); ++index ) {
        const auto& left  = lhs.m_timings[index];
        const auto& right = rhs.m_timings[index];
        if ( std::abs(left.m_timestamp - right.m_timestamp) >= 1e-9 ||
             std::abs(left.m_bpm - right.m_bpm) >= 1e-9 ||
             std::abs(left.m_beat_length - right.m_beat_length) >= 1e-9 ||
             left.m_timingEffect != right.m_timingEffect ||
             std::abs(left.m_timingEffectParameter -
                      right.m_timingEffectParameter) >= 1e-9 ||
             left.m_metadata.timing_properties !=
                 right.m_metadata.timing_properties ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断两个谱面的自动采样类别是否逐字段等价。
bool sameAudioSamples(const BeatMap& lhs, const BeatMap& rhs)
{
    if ( lhs.m_audioSamples.size() != rhs.m_audioSamples.size() ) return false;
    for ( std::size_t index = 0; index < lhs.m_audioSamples.size(); ++index ) {
        const auto& left  = lhs.m_audioSamples[index];
        const auto& right = rhs.m_audioSamples[index];
        if ( std::abs(left.m_timestamp - right.m_timestamp) >= 1e-9 ||
             left.m_offsetMs != right.m_offsetMs ||
             left.m_track != right.m_track ||
             left.m_audioResourceId != right.m_audioResourceId ||
             std::abs(left.m_volume - right.m_volume) >= 1e-6F ||
             left.m_metadata.sample_properties !=
                 right.m_metadata.sample_properties ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断两个谱面的元数据类别是否逐字段等价。
bool sameMetadata(const BeatMap& lhs, const BeatMap& rhs)
{
    const auto& left  = lhs.m_baseMapMetadata;
    const auto& right = rhs.m_baseMapMetadata;
    return left.name == right.name && left.title == right.title &&
           left.title_unicode == right.title_unicode &&
           left.artist == right.artist &&
           left.artist_unicode == right.artist_unicode &&
           left.map_path == right.map_path &&
           left.main_audio_path == right.main_audio_path &&
           left.song_file_hint == right.song_file_hint &&
           left.main_cover_path == right.main_cover_path &&
           left.cover_path == right.cover_path &&
           left.cover_type == right.cover_type &&
           left.video_starttime == right.video_starttime &&
           left.bgxoffset == right.bgxoffset &&
           left.bgyoffset == right.bgyoffset && left.version == right.version &&
           left.author == right.author &&
           std::abs(left.preference_bpm - right.preference_bpm) < 1e-9 &&
           left.track_count == right.track_count &&
           left.bgm_track_count == right.bgm_track_count &&
           std::abs(left.map_length - right.map_length) < 1e-9 &&
           lhs.m_metadata.map_properties == rhs.m_metadata.map_properties;
}

/// @brief 校验每种增量只替换声明的类别，且全部字段与发送端一致。
bool testStrictCategoryIsolation()
{
    constexpr std::array FLAGS{
        BeatmapMutationFlags::Objects,      BeatmapMutationFlags::Timelines,
        BeatmapMutationFlags::AudioSamples, BeatmapMutationFlags::Metadata,
        BeatmapMutationFlags::Annotations,
    };
    for ( const auto flag : FLAGS ) {
        BeatmapDocumentCodec receiver;
        BeatmapDocumentCodec encoder;
        auto                 initial = makeCompleteBeatmap("Creator A");
        auto                 edited  = makeCompleteBeatmap("Creator B");
        edited->m_noteData.notes.front().m_timestamp = 12345.0;
        edited->m_noteData.notes.front()
            .m_metadata.note_properties[MMM::NoteMetadataType::MMM]["delta"] =
            "objects";
        edited->m_timings.front().m_timestamp             = 875.0;
        edited->m_timings.front().m_timingEffectParameter = 90.0;
        edited->m_audioSamples.front().m_offsetMs         = 321;
        edited->m_audioSamples.front().m_volume           = 0.25F;
        edited->m_baseMapMetadata.title                   = "Changed Title";
        edited->m_baseMapMetadata.map_length              = 654321.0;
        edited->m_noteData.notes.front().m_annotation     = "新注释";
        edited->m_noteData.holds.front().m_annotation     = "新子音符注释";
        edited->sync();

        auto snapshot =
            encoder.encode(*initial, BeatmapMutationFlags::All, true);
        auto delta = encoder.encode(*edited, flag, false);
        if ( !snapshot.has_value() || !delta.has_value() ||
             !receiver.apply(snapshot.value()).has_value() ||
             !receiver.apply(delta.value()).has_value() ) {
            return false;
        }
        const auto restored = receiver.materialize();
        if ( !restored ) return false;
        if ( sameObjects(
                 *restored,
                 flag == BeatmapMutationFlags::Objects ? *edited : *initial) ==
                 false ||
             sameTimelines(*restored,
                           flag == BeatmapMutationFlags::Timelines
                               ? *edited
                               : *initial) == false ||
             sameAudioSamples(*restored,
                              flag == BeatmapMutationFlags::AudioSamples
                                  ? *edited
                                  : *initial) == false ||
             sameMetadata(
                 *restored,
                 flag == BeatmapMutationFlags::Metadata ? *edited : *initial) ==
                 false ||
             sameAnnotations(*restored,
                             flag == BeatmapMutationFlags::Annotations
                                 ? *edited
                                 : *initial) == false ) {
            return false;
        }
    }
    return true;
}

/// @brief 校验多轮客户端、房主和广播端往返不会放大或丢失物件。
bool testRepeatedBidirectionalObjectRoundTrips()
{
    BeatmapDocumentCodec guestDocument;
    BeatmapDocumentCodec hostDocument;
    BeatmapDocumentCodec broadcaster;
    auto                 current = makeCompleteBeatmap("Creator");
    auto                 snapshot =
        guestDocument.encode(*current, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() ||
         !guestDocument.apply(snapshot.value()).has_value() ||
         !hostDocument.apply(snapshot.value()).has_value() ||
         !broadcaster.apply(snapshot.value()).has_value() ) {
        return false;
    }

    for ( std::uint32_t round = 0; round < 32U; ++round ) {
        current = guestDocument.materialize();
        if ( !current ) return false;
        current->m_noteData.notes.front().m_timestamp += 1.0;
        current->m_noteData.notes.front()
            .m_metadata.note_properties[MMM::NoteMetadataType::MMM]["round"] =
            std::to_string(round);
        current->sync();

        auto request = guestDocument.encode(
            *current, BeatmapMutationFlags::Objects, false);
        if ( !request.has_value() ||
             !hostDocument.apply(request.value()).has_value() ) {
            return false;
        }
        auto authoritative = hostDocument.materialize();
        if ( !authoritative ) return false;
        if ( !guestDocument.apply(request.value()).has_value() ||
             !broadcaster.apply(request.value()).has_value() ) {
            return false;
        }
        auto guest      = guestDocument.materialize();
        auto remotePeer = broadcaster.materialize();
        if ( !guest || !remotePeer || !sameObjects(*guest, *remotePeer) ||
             !sameObjects(*guest, *authoritative) ||
             guest->m_noteData.notes.size() != 1U ||
             guest->m_noteData.holds.size() != 1U ||
             guest->m_noteData.flicks.size() != 2U ||
             guest->m_noteData.polylines.size() != 1U ) {
            return false;
        }
    }
    return true;
}

/// @brief 校验多个客户端基于同一基线追加物件时按房主顺序合并而不互相覆盖。
bool testConcurrentObjectDeltasMerge()
{
    BeatmapDocumentCodec hostEncoder;
    BeatmapDocumentCodec firstGuestEncoder;
    BeatmapDocumentCodec secondGuestEncoder;
    BeatmapDocumentCodec authority;
    auto                 initial = makeCompleteBeatmap("Creator");
    auto                 snapshot =
        hostEncoder.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() ||
         !authority.apply(snapshot.value()).has_value() ) {
        return false;
    }
    firstGuestEncoder.synchronizeEncodingBaseline(*initial);
    secondGuestEncoder.synchronizeEncodingBaseline(*initial);

    auto  hostEdit             = makeCompleteBeatmap("Creator");
    auto& hostNote             = hostEdit->m_noteData.notes.emplace_back();
    hostNote.m_timestamp       = 3000.0;
    hostNote.m_track           = 0;
    hostNote.m_collaborationId = "host-added-note";
    hostEdit->sync();

    auto  firstGuestEdit = makeCompleteBeatmap("Creator");
    auto& firstGuestNote = firstGuestEdit->m_noteData.notes.emplace_back();
    firstGuestNote.m_timestamp       = 4000.0;
    firstGuestNote.m_track           = 1;
    firstGuestNote.m_collaborationId = "first-guest-added-note";
    firstGuestEdit->sync();

    auto  secondGuestEdit = makeCompleteBeatmap("Creator");
    auto& secondGuestNote = secondGuestEdit->m_noteData.notes.emplace_back();
    secondGuestNote.m_timestamp       = 5000.0;
    secondGuestNote.m_track           = 2;
    secondGuestNote.m_collaborationId = "second-guest-added-note";
    secondGuestEdit->sync();

    auto hostDelta =
        hostEncoder.encode(*hostEdit, BeatmapMutationFlags::Objects, false);
    auto firstGuestDelta = firstGuestEncoder.encode(
        *firstGuestEdit, BeatmapMutationFlags::Objects, false);
    auto secondGuestDelta = secondGuestEncoder.encode(
        *secondGuestEdit, BeatmapMutationFlags::Objects, false);
    if ( !hostDelta.has_value() || !firstGuestDelta.has_value() ||
         !secondGuestDelta.has_value() ||
         !authority.apply(firstGuestDelta.value()).has_value() ||
         !authority.apply(hostDelta.value()).has_value() ||
         !authority.apply(secondGuestDelta.value()).has_value() ) {
        return false;
    }

    const auto merged = authority.materialize();
    if ( !merged || merged->m_noteData.notes.size() != 4U ) return false;
    constexpr std::array EXPECTED_TIMESTAMPS{ 1000.0, 3000.0, 4000.0, 5000.0 };
    return std::all_of(
        EXPECTED_TIMESTAMPS.begin(),
        EXPECTED_TIMESTAMPS.end(),
        [&merged](double timestamp) {
            return std::any_of(
                merged->m_noteData.notes.begin(),
                merged->m_noteData.notes.end(),
                [timestamp](const MMM::Note& note) {
                    return std::abs(note.m_timestamp - timestamp) < 1e-9;
                });
        });
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

/// @brief 校验授权路径可在不持有文档基线时识别快照与各增量类别。
bool testPayloadInspectionIsNonMutating()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec untouched;
    auto                 initial = makeCompleteBeatmap("Creator A");
    auto snapshot = encoder.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot ) return false;
    const auto inspectedSnapshot = BeatmapDocumentCodec::inspect(*snapshot);
    if ( !inspectedSnapshot || !inspectedSnapshot->isSnapshot ||
         inspectedSnapshot->flags != BeatmapMutationFlags::All ||
         untouched.hasDocument() ) {
        return false;
    }

    initial->m_baseMapMetadata.author = "Creator B";
    auto metadataDelta =
        encoder.encode(*initial, BeatmapMutationFlags::Metadata, false);
    if ( !metadataDelta ) return false;
    const auto inspectedDelta = BeatmapDocumentCodec::inspect(*metadataDelta);
    if ( !inspectedDelta || inspectedDelta->isSnapshot ||
         inspectedDelta->flags != BeatmapMutationFlags::Metadata ||
         untouched.hasDocument() ) {
        return false;
    }

    const MMM::Network::Collaboration::ByteBuffer malformed{ 0xFFU, 0x00U };
    return !BeatmapDocumentCodec::inspect(malformed).has_value();
}

/// @brief 校验没有基础快照时拒绝增量和畸形 CBOR。
bool testInvalidPayloads()
{
    BeatmapDocumentCodec codec;
    auto                 beatmap = makeCompleteBeatmap("Creator");
    codec.synchronizeEncodingBaseline(*beatmap);
    beatmap->m_noteData.notes.front().m_timestamp += 1.0;
    beatmap->sync();
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
    return testCompleteSnapshotRoundTrip() &&
                   testPolylineReferencesPreventDuplicateRootObjects() &&
                   testStrictCategoryIsolation() &&
                   testRepeatedBidirectionalObjectRoundTrips() &&
                   testConcurrentObjectDeltasMerge() && testCategoryDelta() &&
                   testPayloadInspectionIsNonMutating() &&
                   testInvalidPayloads() && testLargeSnapshotCompression()
               ? 0
               : 1;
}
