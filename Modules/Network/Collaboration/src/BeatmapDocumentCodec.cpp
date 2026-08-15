#include "network/collaboration/BeatmapDocumentCodec.h"

#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "mmm/Metadata.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Note.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
using Json = nlohmann::json;

constexpr std::uint32_t DOCUMENT_FORMAT_VERSION = 5;
/// @brief 协作谱面负载固定魔数，对应 ASCII `MMBD`。
constexpr std::array<std::uint8_t, 4> DOCUMENT_PAYLOAD_MAGIC{
    'M', 'M', 'B', 'D'
};
/// @brief 谱面负载外层封装版本。
constexpr std::uint8_t DOCUMENT_PAYLOAD_VERSION = 1;
/// @brief 外层负载头长度。
constexpr std::size_t DOCUMENT_PAYLOAD_HEADER_BYTES = 12;
/// @brief 防止畸形压缩包声明过大的解压内存。
constexpr std::size_t MAX_UNCOMPRESSED_DOCUMENT_BYTES = 64U * 1024U * 1024U;
/// @brief 小负载不压缩，避免固定压缩开销反而增大消息。
constexpr std::size_t DOCUMENT_COMPRESSION_THRESHOLD_BYTES = 1024U;
/// @brief 外层负载压缩标志。
constexpr std::uint8_t DOCUMENT_PAYLOAD_COMPRESSED = 1U;

/// @brief 向负载头写入小端 32 位整数。
void appendUint32(ByteBuffer& output, std::uint32_t value)
{
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 从负载头读取小端 32 位整数。
std::uint32_t readUint32(std::span<const std::uint8_t> input,
                         std::size_t                   offset)
{
    std::uint32_t value = 0;
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        value |= static_cast<std::uint32_t>(input[offset++]) << shift;
    }
    return value;
}

/// @brief 将 CBOR 文档封装为可选 DEFLATE 压缩的有界二进制负载。
std::expected<ByteBuffer, BeatmapDocumentError> encodeDocumentPayload(
    const Json& document)
{
    const ByteBuffer raw = Json::to_cbor(document);
    if ( raw.empty() || raw.size() > MAX_UNCOMPRESSED_DOCUMENT_BYTES ||
         raw.size() > std::numeric_limits<std::uint32_t>::max() ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }

    ByteBuffer body       = raw;
    bool       compressed = false;
    if ( raw.size() >= DOCUMENT_COMPRESSION_THRESHOLD_BYTES ) {
        mz_ulong compressedSize =
            mz_compressBound(static_cast<mz_ulong>(raw.size()));
        ByteBuffer candidate(compressedSize);
        if ( mz_compress2(candidate.data(),
                          &compressedSize,
                          raw.data(),
                          static_cast<mz_ulong>(raw.size()),
                          MZ_BEST_SPEED) == MZ_OK &&
             compressedSize < raw.size() ) {
            candidate.resize(static_cast<std::size_t>(compressedSize));
            body       = std::move(candidate);
            compressed = true;
        }
    }

    ByteBuffer output;
    output.reserve(DOCUMENT_PAYLOAD_HEADER_BYTES + body.size());
    output.insert(output.end(),
                  DOCUMENT_PAYLOAD_MAGIC.begin(),
                  DOCUMENT_PAYLOAD_MAGIC.end());
    output.push_back(DOCUMENT_PAYLOAD_VERSION);
    output.push_back(compressed ? DOCUMENT_PAYLOAD_COMPRESSED : 0U);
    output.push_back(0U);
    output.push_back(0U);
    appendUint32(output, static_cast<std::uint32_t>(raw.size()));
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

/// @brief 校验外层头并解压 CBOR 文档。
std::expected<Json, BeatmapDocumentError> decodeDocumentPayload(
    std::span<const std::uint8_t> payload)
{
    if ( payload.size() <= DOCUMENT_PAYLOAD_HEADER_BYTES ||
         !std::equal(DOCUMENT_PAYLOAD_MAGIC.begin(),
                     DOCUMENT_PAYLOAD_MAGIC.end(),
                     payload.begin()) ||
         payload[4] != DOCUMENT_PAYLOAD_VERSION || payload[6] != 0U ||
         payload[7] != 0U ||
         (payload[5] & ~DOCUMENT_PAYLOAD_COMPRESSED) != 0U ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    const std::size_t rawSize = readUint32(payload, 8);
    if ( rawSize == 0 || rawSize > MAX_UNCOMPRESSED_DOCUMENT_BYTES ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }

    const auto body = payload.subspan(DOCUMENT_PAYLOAD_HEADER_BYTES);
    ByteBuffer raw;
    if ( (payload[5] & DOCUMENT_PAYLOAD_COMPRESSED) != 0U ) {
        raw.resize(rawSize);
        mz_ulong decodedSize = static_cast<mz_ulong>(raw.size());
        if ( mz_uncompress(raw.data(),
                           &decodedSize,
                           body.data(),
                           static_cast<mz_ulong>(body.size())) != MZ_OK ||
             decodedSize != rawSize ) {
            return std::unexpected(BeatmapDocumentError::InvalidPayload);
        }
    } else {
        if ( body.size() != rawSize ) {
            return std::unexpected(BeatmapDocumentError::InvalidPayload);
        }
        raw.assign(body.begin(), body.end());
    }

    Json document = Json::from_cbor(raw, true, false);
    if ( document.is_discarded() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    return document;
}

template<typename Enum>
Json encodePropertyMap(
    const std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    Json result = Json::array();
    for ( const auto& [source, values] : properties ) {
        Json encodedValues = Json::object();
        for ( const auto& [key, value] : values ) {
            encodedValues[key] = value;
        }
        result.push_back(Json{
            { "source", static_cast<std::uint32_t>(source) },
            { "values", std::move(encodedValues) },
        });
    }
    return result;
}

template<typename Enum>
bool decodePropertyMap(
    const Json& source,
    std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    if ( !source.is_array() ) return false;
    properties.clear();
    for ( const auto& entry : source ) {
        if ( !entry.is_object() ) return false;
        const auto sourceIt = entry.find("source");
        const auto valuesIt = entry.find("values");
        if ( sourceIt == entry.end() || !sourceIt->is_number_unsigned() ||
             valuesIt == entry.end() || !valuesIt->is_object() ) {
            return false;
        }
        auto& output =
            properties[static_cast<Enum>(sourceIt->get<std::uint32_t>())];
        for ( auto valueIt = valuesIt->begin(); valueIt != valuesIt->end();
              ++valueIt ) {
            if ( !valueIt.value().is_string() ) return false;
            output.emplace(valueIt.key(), valueIt.value().get<std::string>());
        }
    }
    return true;
}

Json encodeNoteMetadata(const ::MMM::NoteMetadata& metadata)
{
    return encodePropertyMap(metadata.note_properties);
}

Json encodeTimingMetadata(const ::MMM::TimingMetadata& metadata)
{
    return encodePropertyMap(metadata.timing_properties);
}

Json encodeSampleMetadata(const ::MMM::SampleMetadata& metadata)
{
    return encodePropertyMap(metadata.sample_properties);
}

Json encodeSampleBinding(
    const std::optional<::MMM::AudioSampleBinding>& binding)
{
    if ( !binding ) return nullptr;
    return Json{
        { "resource", binding->m_audioResourceId },
        { "volume", binding->m_volume },
    };
}

bool decodeSampleBinding(const Json&                               source,
                         std::optional<::MMM::AudioSampleBinding>& binding)
{
    if ( source.is_null() ) {
        binding.reset();
        return true;
    }
    if ( !source.is_object() ) return false;
    const auto resourceIt = source.find("resource");
    const auto volumeIt   = source.find("volume");
    if ( resourceIt == source.end() || !resourceIt->is_string() ||
         volumeIt == source.end() || !volumeIt->is_number() ) {
        return false;
    }
    binding = ::MMM::AudioSampleBinding{ resourceIt->get<std::string>(),
                                         volumeIt->get<float>() };
    return true;
}

Json encodeNote(const ::MMM::Note& note)
{
    Json result{
        { "type", static_cast<std::uint32_t>(note.m_type) },
        { "timestamp", note.m_timestamp },
        { "track", note.m_track },
        { "collaboration_id", note.m_collaborationId },
        { "binding", encodeSampleBinding(note.m_sampleBinding) },
        { "metadata", encodeNoteMetadata(note.m_metadata) },
    };
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        result["duration"] = static_cast<const ::MMM::Hold&>(note).m_duration;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        result["dtrack"] = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }
    return result;
}

/// @brief 编码玩家物件，并按 Polyline 实际引用排除重复的根物件副本。
/// @param beatmap 待编码谱面。
/// @return 可写入协作文档的物件数组。
Json encodeObjects(const ::MMM::BeatMap& beatmap)
{
    std::unordered_set<const ::MMM::Note*> polylineSubNotes;
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        for ( const auto& subNote : polyline.m_subNotes ) {
            polylineSubNotes.insert(&subNote.get());
        }
    }

    Json result = Json::array();
    for ( const auto& note : beatmap.m_noteData.notes ) {
        if ( !note.m_isSubNote && !polylineSubNotes.contains(&note) ) {
            result.push_back(encodeNote(note));
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        if ( !hold.m_isSubNote && !polylineSubNotes.contains(&hold) ) {
            result.push_back(encodeNote(hold));
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        if ( !flick.m_isSubNote && !polylineSubNotes.contains(&flick) ) {
            result.push_back(encodeNote(flick));
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        Json encoded         = encodeNote(polyline);
        encoded["sub_notes"] = Json::array();
        for ( const auto& subNote : polyline.m_subNotes ) {
            encoded["sub_notes"].push_back(encodeNote(subNote.get()));
        }
        result.push_back(std::move(encoded));
    }
    return result;
}

/// @brief 编码一个根物件，并在折线中内嵌其子物件。
Json encodeRootObject(const ::MMM::Note& note)
{
    Json encoded = encodeNote(note);
    if ( note.m_type != ::MMM::NoteType::POLYLINE ) return encoded;
    encoded["sub_notes"] = Json::array();
    const auto& polyline = static_cast<const ::MMM::Polyline&>(note);
    for ( const auto& subNote : polyline.m_subNotes ) {
        encoded["sub_notes"].push_back(encodeNote(subNote.get()));
    }
    return encoded;
}

/// @brief 玩家物件领域字段的双 64 位快速指纹。
struct ObjectFingerprint {
    std::uint64_t first{ 0x243F6A8885A308D3ULL };
    std::uint64_t second{ 0x13198A2E03707344ULL };

    bool operator==(const ObjectFingerprint&) const = default;
};

/// @brief 把一个整数混入物件指纹。
void mixFingerprint(ObjectFingerprint& fingerprint, std::uint64_t value)
{
    fingerprint.first ^= value + 0x9E3779B97F4A7C15ULL +
                         (fingerprint.first << 6U) + (fingerprint.first >> 2U);
    fingerprint.second =
        std::rotl(fingerprint.second ^ value, 27) * 0x94D049BB133111EBULL;
}

/// @brief 把字符串字节混入物件指纹。
void mixFingerprint(ObjectFingerprint& fingerprint, std::string_view value)
{
    mixFingerprint(fingerprint, value.size());
    for ( const unsigned char byte : value ) {
        fingerprint.first = (fingerprint.first ^ byte) * 0x100000001B3ULL;
        fingerprint.second =
            (fingerprint.second + byte) * 0x9E3779B185EBCA87ULL;
    }
}

/// @brief 把无序属性表以顺序无关方式混入物件指纹。
template<typename Enum>
void mixPropertyMapFingerprint(
    ObjectFingerprint& fingerprint,
    const std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    std::uint64_t aggregateFirst  = 0;
    std::uint64_t aggregateSecond = 0;
    std::size_t   entryCount      = 0;
    for ( const auto& [source, values] : properties ) {
        for ( const auto& [key, value] : values ) {
            ObjectFingerprint entry;
            mixFingerprint(entry, static_cast<std::uint32_t>(source));
            mixFingerprint(entry, key);
            mixFingerprint(entry, value);
            aggregateFirst ^=
                std::rotl(entry.first, static_cast<int>(entry.second & 63U));
            aggregateSecond +=
                entry.second ^ (entry.first * 0xD6E8FEB86659FD93ULL);
            ++entryCount;
        }
    }
    mixFingerprint(fingerprint, entryCount);
    mixFingerprint(fingerprint, aggregateFirst);
    mixFingerprint(fingerprint, aggregateSecond);
}

/// @brief 计算一个根物件及其折线子物件的领域字段指纹。
ObjectFingerprint fingerprintObject(const ::MMM::Note& note)
{
    ObjectFingerprint result;
    mixFingerprint(result, static_cast<std::uint32_t>(note.m_type));
    mixFingerprint(result, std::bit_cast<std::uint64_t>(note.m_timestamp));
    mixFingerprint(result, static_cast<std::uint32_t>(note.m_track));
    mixFingerprint(result, note.m_collaborationId);
    mixPropertyMapFingerprint(result, note.m_metadata.note_properties);
    mixFingerprint(result, note.m_sampleBinding.has_value() ? 1U : 0U);
    if ( note.m_sampleBinding ) {
        mixFingerprint(result, note.m_sampleBinding->m_audioResourceId);
        mixFingerprint(
            result,
            std::bit_cast<std::uint32_t>(note.m_sampleBinding->m_volume));
    }
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        mixFingerprint(result,
                       std::bit_cast<std::uint64_t>(
                           static_cast<const ::MMM::Hold&>(note).m_duration));
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        mixFingerprint(result,
                       static_cast<std::uint32_t>(
                           static_cast<const ::MMM::Flick&>(note).m_dtrack));
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        const auto& polyline = static_cast<const ::MMM::Polyline&>(note);
        mixFingerprint(result, polyline.m_subNotes.size());
        for ( const auto& subNote : polyline.m_subNotes ) {
            const auto child = fingerprintObject(subNote.get());
            mixFingerprint(result, child.first);
            mixFingerprint(result, child.second);
        }
    }
    return result;
}

/// @brief 编码基线中一个可按稳定 ID 快速比较的物件。
struct CachedEncodedObject {
    ObjectFingerprint fingerprint;
    Json              encoded;
    std::uint64_t     seenGeneration{ 0 };
};

using ObjectEncodingCache =
    std::unordered_map<std::string, CachedEncodedObject>;

/// @brief 一次快速物件增量及编码成功后需要提交的缓存变化。
struct CachedObjectDelta {
    Json                                                     delta;
    std::vector<std::pair<std::string, CachedEncodedObject>> changed;
    std::vector<std::string>                                 removed;
};

Json encodeTimelines(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& timing : beatmap.m_timings ) {
        result.push_back(Json{
            { "timestamp", timing.m_timestamp },
            { "bpm", timing.m_bpm },
            { "beat_length", timing.m_beat_length },
            { "effect", static_cast<std::uint32_t>(timing.m_timingEffect) },
            { "value", timing.m_timingEffectParameter },
            { "metadata", encodeTimingMetadata(timing.m_metadata) },
        });
    }
    return result;
}

Json encodeAudioSamples(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& sample : beatmap.m_audioSamples ) {
        result.push_back(Json{
            { "collaboration_id", sample.m_collaborationId },
            { "timestamp", sample.m_timestamp },
            { "offset_ms", sample.m_offsetMs },
            { "track", sample.m_track },
            { "resource", sample.m_audioResourceId },
            { "volume", sample.m_volume },
            { "metadata", encodeSampleMetadata(sample.m_metadata) },
        });
    }
    return result;
}

/// @brief 编码独立于物件几何的谱面多批注。
/// @param beatmap 待编码谱面。
/// @return 以批注稳定标识寻址的记录数组。
Json encodeAnnotations(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& annotation : beatmap.m_annotations ) {
        result.push_back(Json{
            { "collaboration_id", annotation.m_id },
            { "target_kind",
              static_cast<std::uint32_t>(annotation.m_targetKind) },
            { "target_id", annotation.m_targetId },
            { "timestamp", annotation.m_timestamp },
            { "author", annotation.m_author },
            { "content", annotation.m_content },
        });
    }
    return result;
}

Json encodeMetadata(const ::MMM::BeatMap& beatmap)
{
    const auto& base = beatmap.m_baseMapMetadata;
    Json        result{
        { "name", base.name },
        { "title", base.title },
        { "title_unicode", base.title_unicode },
        { "artist", base.artist },
        { "artist_unicode", base.artist_unicode },
        { "map_path", Config::pathToUtf8(base.map_path) },
        { "main_audio_path", Config::pathToUtf8(base.main_audio_path) },
        { "song_file_hint", Config::pathToUtf8(base.song_file_hint) },
        { "main_cover_path", Config::pathToUtf8(base.main_cover_path) },
        { "cover_path", Config::pathToUtf8(base.cover_path) },
        { "cover_type", static_cast<std::uint32_t>(base.cover_type) },
        { "video_starttime", base.video_starttime },
        { "bgxoffset", base.bgxoffset },
        { "bgyoffset", base.bgyoffset },
        { "version", base.version },
        { "author", base.author },
        { "preference_bpm", base.preference_bpm },
        { "track_count", base.track_count },
        { "bgm_track_count", base.bgm_track_count },
        { "map_length", base.map_length },
        { "extra", encodePropertyMap(beatmap.m_metadata.map_properties) },
    };
    return result;
}

template<typename Value>
bool readValue(const Json& source, std::string_view key, Value& value)
{
    const auto iterator = source.find(key);
    if ( iterator == source.end() ) return false;
    if constexpr ( std::is_same_v<Value, bool> ) {
        if ( !iterator->is_boolean() ) return false;
    } else if constexpr ( std::is_same_v<Value, std::string> ) {
        if ( !iterator->is_string() ) return false;
    } else if constexpr ( std::is_floating_point_v<Value> ) {
        if ( !iterator->is_number() ) return false;
    } else if constexpr ( std::is_unsigned_v<Value> ) {
        if ( !iterator->is_number_unsigned() ) return false;
    } else if constexpr ( std::is_integral_v<Value> ) {
        if ( !iterator->is_number_integer() ) return false;
    }
    value = iterator->get<Value>();
    return true;
}

bool decodeCommonNote(const Json& source, ::MMM::Note& note)
{
    std::uint32_t rawType    = 0;
    const auto    metadataIt = source.find("metadata");
    const auto    bindingIt  = source.find("binding");
    if ( !source.is_object() || !readValue(source, "type", rawType) ||
         rawType > static_cast<std::uint32_t>(::MMM::NoteType::POLYLINE) ||
         !readValue(source, "timestamp", note.m_timestamp) ||
         !readValue(source, "track", note.m_track) ||
         !readValue(source, "collaboration_id", note.m_collaborationId) ||
         metadataIt == source.end() || bindingIt == source.end() ||
         !decodePropertyMap(*metadataIt, note.m_metadata.note_properties) ||
         !decodeSampleBinding(*bindingIt, note.m_sampleBinding) ) {
        return false;
    }
    note.m_type = static_cast<::MMM::NoteType>(rawType);
    return true;
}

bool appendDecodedNote(const Json& source, ::MMM::BeatMap& beatmap,
                       ::MMM::Polyline* parent)
{
    std::uint32_t rawType = 0;
    if ( !source.is_object() || !readValue(source, "type", rawType) ||
         rawType > static_cast<std::uint32_t>(::MMM::NoteType::POLYLINE) ) {
        return false;
    }
    const auto   type    = static_cast<::MMM::NoteType>(rawType);
    ::MMM::Note* decoded = nullptr;
    if ( type == ::MMM::NoteType::NOTE ) {
        auto& note = beatmap.m_noteData.notes.emplace_back();
        decoded    = &note;
    } else if ( type == ::MMM::NoteType::HOLD ) {
        auto& hold = beatmap.m_noteData.holds.emplace_back();
        if ( !readValue(source, "duration", hold.m_duration) ) return false;
        decoded = &hold;
    } else if ( type == ::MMM::NoteType::FLICK ) {
        auto& flick = beatmap.m_noteData.flicks.emplace_back();
        if ( !readValue(source, "dtrack", flick.m_dtrack) ) return false;
        decoded = &flick;
    } else if ( parent == nullptr ) {
        auto& polyline = beatmap.m_noteData.polylines.emplace_back();
        decoded        = &polyline;
    } else {
        return false;
    }
    if ( !decodeCommonNote(source, *decoded) ) return false;
    if ( parent != nullptr ) {
        decoded->m_isSubNote = true;
        parent->m_subNotes.emplace_back(*decoded);
        if ( type == ::MMM::NoteType::HOLD ) {
            parent->m_subHolds.emplace_back(
                static_cast<::MMM::Hold&>(*decoded));
        } else if ( type == ::MMM::NoteType::FLICK ) {
            parent->m_subFlicks.emplace_back(
                static_cast<::MMM::Flick&>(*decoded));
        }
        return true;
    }
    if ( type != ::MMM::NoteType::POLYLINE ) return true;

    auto&      polyline   = static_cast<::MMM::Polyline&>(*decoded);
    const auto subNotesIt = source.find("sub_notes");
    if ( subNotesIt == source.end() || !subNotesIt->is_array() ) return false;
    for ( const auto& subNote : *subNotesIt ) {
        if ( !appendDecodedNote(subNote, beatmap, &polyline) ) return false;
    }
    if ( !polyline.m_subNotes.empty() ) {
        polyline.m_timestamp = polyline.m_subNotes.front().get().m_timestamp;
        polyline.m_track     = polyline.m_subNotes.front().get().m_track;
    }
    return true;
}

bool decodeObjects(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ) return false;
    for ( const auto& note : source ) {
        if ( !appendDecodedNote(note, beatmap, nullptr) ) return false;
    }
    return true;
}

bool decodeTimelines(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ) return false;
    for ( const auto& entry : source ) {
        auto&         timing     = beatmap.m_timings.emplace_back();
        std::uint32_t effect     = 0;
        const auto    metadataIt = entry.find("metadata");
        if ( !entry.is_object() ||
             !readValue(entry, "timestamp", timing.m_timestamp) ||
             !readValue(entry, "bpm", timing.m_bpm) ||
             !readValue(entry, "beat_length", timing.m_beat_length) ||
             !readValue(entry, "effect", effect) || effect > 3U ||
             !readValue(entry, "value", timing.m_timingEffectParameter) ||
             metadataIt == entry.end() ||
             !decodePropertyMap(*metadataIt,
                                timing.m_metadata.timing_properties) ) {
            return false;
        }
        timing.m_timingEffect = static_cast<::MMM::TimingEffect>(effect);
    }
    return true;
}

bool decodeAudioSamples(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ) return false;
    std::unordered_set<std::string> identities;
    identities.reserve(source.size());
    for ( const auto& entry : source ) {
        auto&      sample     = beatmap.m_audioSamples.emplace_back();
        const auto metadataIt = entry.find("metadata");
        if ( !entry.is_object() ||
             !readValue(entry, "collaboration_id", sample.m_collaborationId) ||
             sample.m_collaborationId.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             (!sample.m_collaborationId.empty() &&
              !identities.insert(sample.m_collaborationId).second) ||
             !readValue(entry, "timestamp", sample.m_timestamp) ||
             !readValue(entry, "offset_ms", sample.m_offsetMs) ||
             !readValue(entry, "track", sample.m_track) ||
             !readValue(entry, "resource", sample.m_audioResourceId) ||
             !readValue(entry, "volume", sample.m_volume) ||
             metadataIt == entry.end() ||
             !decodePropertyMap(*metadataIt,
                                sample.m_metadata.sample_properties) ) {
            return false;
        }
    }
    return true;
}

/// @brief 解码独立时间戳与物件多批注。
/// @param source 批注数组。
/// @param beatmap 接收批注的谱面。
/// @return 结构、长度和稳定标识均有效时返回 true。
bool decodeAnnotations(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ||
         source.size() > ::MMM::MAX_BEATMAP_ANNOTATION_COUNT ) {
        return false;
    }

    std::unordered_set<std::string> annotationIdentities;
    annotationIdentities.reserve(source.size());
    for ( const auto& entry : source ) {
        ::MMM::BeatmapAnnotation annotation;
        std::uint32_t            targetKind = 0U;
        if ( !entry.is_object() ||
             !readValue(entry, "collaboration_id", annotation.m_id) ||
             !readValue(entry, "target_kind", targetKind) ||
             targetKind >
                 static_cast<std::uint32_t>(
                     ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE) ||
             !readValue(entry, "target_id", annotation.m_targetId) ||
             !readValue(entry, "timestamp", annotation.m_timestamp) ||
             !readValue(entry, "author", annotation.m_author) ||
             !readValue(entry, "content", annotation.m_content) ||
             annotation.m_id.empty() ||
             annotation.m_id.size() > ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             annotation.m_targetId.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             !std::isfinite(annotation.m_timestamp) ||
             annotation.m_content.empty() ||
             annotation.m_content.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES ||
             (!annotation.m_author.empty() &&
              Config::normalizeCreatorIdentity(annotation.m_author) !=
                  annotation.m_author) ||
             !annotationIdentities.insert(annotation.m_id).second ) {
            return false;
        }
        annotation.m_targetKind =
            static_cast<::MMM::BeatmapAnnotationTargetKind>(targetKind);
        if ( annotation.m_targetKind !=
                 ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP &&
             annotation.m_targetId.empty() ) {
            return false;
        }
        beatmap.m_annotations.push_back(std::move(annotation));
    }
    return true;
}

bool decodeMetadata(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_object() ) return false;
    auto&         base = beatmap.m_baseMapMetadata;
    std::string   mapPath;
    std::string   mainAudioPath;
    std::string   songFileHint;
    std::string   mainCoverPath;
    std::string   coverPath;
    std::uint32_t coverType = 0;
    const auto    extraIt   = source.find("extra");
    if ( !readValue(source, "name", base.name) ||
         !readValue(source, "title", base.title) ||
         !readValue(source, "title_unicode", base.title_unicode) ||
         !readValue(source, "artist", base.artist) ||
         !readValue(source, "artist_unicode", base.artist_unicode) ||
         !readValue(source, "map_path", mapPath) ||
         !readValue(source, "main_audio_path", mainAudioPath) ||
         !readValue(source, "song_file_hint", songFileHint) ||
         !readValue(source, "main_cover_path", mainCoverPath) ||
         !readValue(source, "cover_path", coverPath) ||
         !readValue(source, "cover_type", coverType) || coverType > 1U ||
         !readValue(source, "video_starttime", base.video_starttime) ||
         !readValue(source, "bgxoffset", base.bgxoffset) ||
         !readValue(source, "bgyoffset", base.bgyoffset) ||
         !readValue(source, "version", base.version) ||
         !readValue(source, "author", base.author) ||
         !readValue(source, "preference_bpm", base.preference_bpm) ||
         !readValue(source, "track_count", base.track_count) ||
         !readValue(source, "bgm_track_count", base.bgm_track_count) ||
         !readValue(source, "map_length", base.map_length) ||
         extraIt == source.end() ||
         !decodePropertyMap(*extraIt, beatmap.m_metadata.map_properties) ) {
        return false;
    }
    base.map_path        = Config::utf8ToPath(mapPath);
    base.main_audio_path = Config::utf8ToPath(mainAudioPath);
    base.song_file_hint  = Config::utf8ToPath(songFileHint);
    base.main_cover_path = Config::utf8ToPath(mainCoverPath);
    base.cover_path      = Config::utf8ToPath(coverPath);
    base.cover_type      = static_cast<::MMM::CoverType>(coverType);
    return true;
}

/// @brief 编码不含协议头的完整谱面分类文档。
Json makeDocument(const ::MMM::BeatMap& beatmap)
{
    return Json{
        { "objects", encodeObjects(beatmap) },
        { "timelines", encodeTimelines(beatmap) },
        { "audio_samples", encodeAudioSamples(beatmap) },
        { "metadata", encodeMetadata(beatmap) },
        { "annotations", encodeAnnotations(beatmap) },
    };
}

/// @brief 只编码本次增量实际涉及的谱面类别。
/// @param beatmap 当前完整谱面。
/// @param flags 本次需要参与增量比较的类别。
/// @return 仅包含指定类别的临时文档。
Json makeMutationDocument(const ::MMM::BeatMap&       beatmap,
                          ::MMM::BeatmapMutationFlags flags)
{
    Json result = Json::object();
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Timelines) ) {
        result["timelines"] = encodeTimelines(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::AudioSamples) ) {
        result["audio_samples"] = encodeAudioSamples(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Metadata) ) {
        result["metadata"] = encodeMetadata(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Annotations) ) {
        result["annotations"] = encodeAnnotations(beatmap);
    }
    return result;
}

/// @brief 计算两个数组间保留重复项数量的增删集合。
Json makeArrayDelta(const Json& before, const Json& after)
{
    std::unordered_map<std::string, std::int64_t> countDifference;
    countDifference.reserve(before.size() + after.size());
    for ( const auto& value : before ) --countDifference[value.dump()];
    for ( const auto& value : after ) ++countDifference[value.dump()];

    Json added = Json::array();
    for ( const auto& value : after ) {
        auto& remaining = countDifference[value.dump()];
        if ( remaining <= 0 ) continue;
        added.push_back(value);
        --remaining;
    }

    Json removed = Json::array();
    for ( const auto& value : before ) {
        auto& remaining = countDifference[value.dump()];
        if ( remaining >= 0 ) continue;
        removed.push_back(value);
        ++remaining;
    }
    return Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
}

/// @brief 读取物件或注释条目的稳定协作标识。
/// @param value 协作文档数组中的单个条目。
/// @return 标识合法且非空时返回其只读地址。
const std::string* collaborationIdentity(const Json& value)
{
    if ( !value.is_object() ) return nullptr;
    const auto identity = value.find("collaboration_id");
    if ( identity == value.end() || !identity->is_string() ||
         identity->get_ref<const std::string&>().empty() ) {
        return nullptr;
    }
    return &identity->get_ref<const std::string&>();
}

/// @brief 只编码相对基线实际变化的玩家物件。
/// @param cache 上一次成功编码后的稳定 ID 物件缓存。
/// @param generation 本轮扫描标识。
/// @param beatmap 当前完整谱面。
/// @return 标识完整且唯一时返回轻量增量，否则返回空以触发兼容路径。
std::optional<CachedObjectDelta> makeObjectIdentityDelta(
    ObjectEncodingCache& cache, std::uint64_t generation,
    const ::MMM::BeatMap& beatmap)
{
    std::unordered_set<std::string_view> newIdentities;
    Json                                 added   = Json::array();
    Json                                 removed = Json::array();
    CachedObjectDelta                    result;
    bool                                 valid  = true;
    const auto                           append = [&](const ::MMM::Note& note) {
        if ( !valid || note.m_collaborationId.empty() ) {
            valid = false;
            return;
        }
        const auto fingerprint = fingerprintObject(note);
        const auto previous    = cache.find(note.m_collaborationId);
        if ( previous != cache.end() ) {
            if ( previous->second.seenGeneration == generation ) {
                valid = false;
                return;
            }
            previous->second.seenGeneration = generation;
            if ( previous->second.fingerprint == fingerprint ) return;
            removed.push_back(previous->second.encoded);
        } else if ( !newIdentities.emplace(note.m_collaborationId).second ) {
            valid = false;
            return;
        }
        auto encoded = encodeRootObject(note);
        added.push_back(encoded);
        result.changed.emplace_back(
            note.m_collaborationId,
            CachedEncodedObject{ fingerprint, std::move(encoded), generation });
    };

    std::unordered_set<const ::MMM::Note*> polylineSubNotes;
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        for ( const auto& subNote : polyline.m_subNotes ) {
            polylineSubNotes.insert(&subNote.get());
        }
    }
    for ( const auto& note : beatmap.m_noteData.notes ) {
        if ( !note.m_isSubNote && !polylineSubNotes.contains(&note) ) {
            append(note);
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        if ( !hold.m_isSubNote && !polylineSubNotes.contains(&hold) ) {
            append(hold);
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        if ( !flick.m_isSubNote && !polylineSubNotes.contains(&flick) ) {
            append(flick);
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines )
        append(polyline);
    if ( !valid ) return std::nullopt;

    for ( const auto& [identity, cached] : cache ) {
        if ( cached.seenGeneration == generation ) continue;
        removed.push_back(cached.encoded);
        result.removed.push_back(identity);
    }
    result.delta = Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
    return result;
}

/// @brief 提交一次已成功编码的物件缓存变化。
void commitObjectIdentityDelta(ObjectEncodingCache& cache,
                               CachedObjectDelta&   delta)
{
    for ( const auto& identity : delta.removed ) cache.erase(identity);
    for ( auto& [identity, object] : delta.changed ) {
        cache.insert_or_assign(std::move(identity), std::move(object));
    }
}

/// @brief 从完整谱面建立稳定 ID 物件编码缓存。
std::optional<ObjectEncodingCache> makeObjectEncodingCache(
    const ::MMM::BeatMap& beatmap)
{
    ObjectEncodingCache cache;
    auto                initial = makeObjectIdentityDelta(cache, 1U, beatmap);
    if ( !initial ) return std::nullopt;
    commitObjectIdentityDelta(cache, *initial);
    return cache;
}

/// @brief 将稳定 ID 物件增量应用到编码基线数组。
/// @param target 当前编码基线的物件数组。
/// @param delta 已成功发送的物件增量。
/// @return 输入结构完整时返回 true。
bool applyIdentityDeltaToArray(Json& target, const Json& delta)
{
    const auto added   = delta.find("added");
    const auto removed = delta.find("removed");
    if ( !target.is_array() || !delta.is_object() || added == delta.end() ||
         !added->is_array() || removed == delta.end() ||
         !removed->is_array() ) {
        return false;
    }
    std::unordered_map<std::string_view, const Json*> replacements;
    replacements.reserve(added->size());
    for ( const auto& value : *added ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity || !replacements.emplace(*identity, &value).second ) {
            return false;
        }
    }
    for ( const auto& value : *removed ) {
        if ( !collaborationIdentity(value) ) return false;
    }
    const auto findByIdentity = [&](std::string_view identity) {
        return std::find_if(
            target.begin(), target.end(), [identity](const Json& item) {
                const auto* itemIdentity = collaborationIdentity(item);
                return itemIdentity && *itemIdentity == identity;
            });
    };
    std::unordered_set<std::string_view> appliedReplacements;
    appliedReplacements.reserve(added->size());
    for ( const auto& value : *removed ) {
        const auto& identity    = *collaborationIdentity(value);
        const auto  replacement = replacements.find(identity);
        auto        existing    = findByIdentity(identity);
        if ( replacement != replacements.end() ) {
            if ( existing != target.end() ) {
                *existing = *replacement->second;
                appliedReplacements.emplace(identity);
            }
        } else if ( existing != target.end() ) {
            target.erase(existing);
        }
    }
    for ( const auto& value : *added ) {
        const auto& identity = *collaborationIdentity(value);
        if ( appliedReplacements.contains(identity) ) continue;
        auto existing = findByIdentity(identity);
        if ( existing != target.end() ) {
            *existing = value;
        } else {
            target.push_back(value);
        }
    }
    return true;
}

/// @brief 按稳定协作标识计算物件或注释的增删集合。
/// @param before 编码基线数组。
/// @param after 当前状态数组。
/// @return 标识缺失或重复时回退到兼容的完整值比较结果。
Json makeIdentityArrayDelta(const Json& before, const Json& after)
{
    std::unordered_map<std::string_view, const Json*> beforeByIdentity;
    std::unordered_map<std::string_view, const Json*> afterByIdentity;
    beforeByIdentity.reserve(before.size());
    afterByIdentity.reserve(after.size());
    for ( const auto& value : before ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity ||
             !beforeByIdentity.emplace(*identity, &value).second ) {
            return makeArrayDelta(before, after);
        }
    }
    for ( const auto& value : after ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity || !afterByIdentity.emplace(*identity, &value).second ) {
            return makeArrayDelta(before, after);
        }
    }

    Json added   = Json::array();
    Json removed = Json::array();
    for ( const auto& value : after ) {
        const auto& identity = *collaborationIdentity(value);
        const auto  previous = beforeByIdentity.find(identity);
        if ( previous == beforeByIdentity.end() ||
             *previous->second != value ) {
            added.push_back(value);
            if ( previous != beforeByIdentity.end() ) {
                removed.push_back(*previous->second);
            }
        }
    }
    for ( const auto& value : before ) {
        const auto& identity = *collaborationIdentity(value);
        if ( !afterByIdentity.contains(identity) ) removed.push_back(value);
    }
    return Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
}

/// @brief 判断数组增量是否没有任何净变化。
bool arrayDeltaEmpty(const Json& delta)
{
    return delta.at("added").empty() && delta.at("removed").empty();
}

/// @brief 基于逻辑线程上一次实际状态构造可合并的分类增量。
std::optional<Json> makeIncrementalPatch(const Json&                 baseline,
                                         const Json&                 current,
                                         ::MMM::BeatmapMutationFlags flags,
                                         std::optional<Json> objectDelta)
{
    Json patch{
        { "version", DOCUMENT_FORMAT_VERSION },
        { "snapshot", false },
    };
    bool       changed          = false;
    const auto appendArrayDelta = [&](std::string_view            category,
                                      std::string_view            deltaKey,
                                      ::MMM::BeatmapMutationFlags flag) {
        if ( !hasBeatmapMutationFlag(flags, flag) ) return;
        const bool identityAddressed =
            category == "objects" || category == "annotations";
        Json delta;
        if ( flag == ::MMM::BeatmapMutationFlags::Objects && objectDelta ) {
            delta = std::move(*objectDelta);
        } else {
            delta = identityAddressed
                        ? makeIdentityArrayDelta(baseline.at(category),
                                                 current.at(category))
                        : makeArrayDelta(baseline.at(category),
                                         current.at(category));
        }
        if ( arrayDeltaEmpty(delta) ) return;
        patch[std::string(deltaKey)] = std::move(delta);
        changed                      = true;
    };
    appendArrayDelta(
        "objects", "objects_delta", ::MMM::BeatmapMutationFlags::Objects);
    appendArrayDelta(
        "timelines", "timelines_delta", ::MMM::BeatmapMutationFlags::Timelines);
    appendArrayDelta("audio_samples",
                     "audio_samples_delta",
                     ::MMM::BeatmapMutationFlags::AudioSamples);
    appendArrayDelta("annotations",
                     "annotations_delta",
                     ::MMM::BeatmapMutationFlags::Annotations);
    if ( hasBeatmapMutationFlag(flags, ::MMM::BeatmapMutationFlags::Metadata) &&
         baseline.at("metadata") != current.at("metadata") ) {
        patch["metadata"] = current.at("metadata");
        changed           = true;
    }
    if ( !changed ) return std::nullopt;
    return patch;
}
}  // namespace

class BeatmapDocumentCodec::Impl
{
public:
    Json                document = Json::object();
    bool                hasDocument{ false };
    Json                encodingBaseline = Json::object();
    bool                hasEncodingBaseline{ false };
    ObjectEncodingCache objectEncodingCache;
    bool                hasObjectEncodingCache{ false };
    std::uint64_t       objectEncodingGeneration{ 1 };
};

/// @brief 后台已经完成全部扫描和编码的玩家物件基线。
class BeatmapDocumentCodec::ObjectEncodingBaseline
{
public:
    /// @brief 编码基线中的完整玩家物件数组。
    Json objects = Json::array();
    /// @brief 按稳定标识索引的领域物件编码缓存。
    ObjectEncodingCache cache;
};

BeatmapDocumentCodec::BeatmapDocumentCodec() : m_impl(std::make_unique<Impl>())
{
}

BeatmapDocumentCodec::~BeatmapDocumentCodec() = default;

std::expected<ByteBuffer, BeatmapDocumentError> BeatmapDocumentCodec::encode(
    const ::MMM::BeatMap& beatmap, ::MMM::BeatmapMutationFlags flags,
    bool snapshot)
{
    if ( !snapshot && flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    Json current =
        snapshot ? makeDocument(beatmap) : makeMutationDocument(beatmap, flags);
    if ( snapshot ) {
        Json payload        = current;
        payload["version"]  = DOCUMENT_FORMAT_VERSION;
        payload["snapshot"] = true;
        auto encoded        = encodeDocumentPayload(payload);
        if ( encoded.has_value() ) {
            m_impl->encodingBaseline    = current;
            m_impl->hasEncodingBaseline = true;
            auto cache                  = makeObjectEncodingCache(beatmap);
            if ( cache ) {
                m_impl->objectEncodingCache    = std::move(*cache);
                m_impl->hasObjectEncodingCache = true;
            } else {
                m_impl->objectEncodingCache.clear();
                m_impl->hasObjectEncodingCache = false;
            }
            m_impl->objectEncodingGeneration = 1;
        }
        return encoded;
    }
    if ( !m_impl->hasEncodingBaseline ) {
        if ( !m_impl->hasDocument ) {
            return std::unexpected(BeatmapDocumentError::MissingSnapshot);
        }
        m_impl->encodingBaseline    = m_impl->document;
        m_impl->hasEncodingBaseline = true;
    }
    std::optional<CachedObjectDelta> cachedObjectDelta;
    std::optional<Json>              objectDelta;
    if ( hasBeatmapMutationFlag(flags, ::MMM::BeatmapMutationFlags::Objects) ) {
        if ( m_impl->hasObjectEncodingCache ) {
            ++m_impl->objectEncodingGeneration;
            if ( m_impl->objectEncodingGeneration == 0 ) {
                for ( auto& [identity, object] : m_impl->objectEncodingCache ) {
                    static_cast<void>(identity);
                    object.seenGeneration = 0;
                }
                m_impl->objectEncodingGeneration = 1;
            }
            cachedObjectDelta =
                makeObjectIdentityDelta(m_impl->objectEncodingCache,
                                        m_impl->objectEncodingGeneration,
                                        beatmap);
            if ( cachedObjectDelta ) {
                objectDelta = cachedObjectDelta->delta;
            }
        }
        if ( !objectDelta ) current["objects"] = encodeObjects(beatmap);
    }
    const bool usesFastObjectDelta = objectDelta.has_value();
    auto       patch               = makeIncrementalPatch(
        m_impl->encodingBaseline, current, flags, std::move(objectDelta));
    if ( !patch.has_value() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto encoded = encodeDocumentPayload(*patch);
    if ( !encoded.has_value() ) return encoded;

    const auto updateBaseline = [&](std::string_view            category,
                                    ::MMM::BeatmapMutationFlags flag) {
        if ( hasBeatmapMutationFlag(flags, flag) ) {
            if ( flag == ::MMM::BeatmapMutationFlags::Objects ) {
                const auto delta = patch->find("objects_delta");
                if ( usesFastObjectDelta && delta != patch->end() ) {
                    if ( applyIdentityDeltaToArray(
                             m_impl->encodingBaseline["objects"], *delta) &&
                         cachedObjectDelta ) {
                        commitObjectIdentityDelta(m_impl->objectEncodingCache,
                                                  *cachedObjectDelta);
                    } else {
                        m_impl->objectEncodingCache.clear();
                        m_impl->hasObjectEncodingCache = false;
                    }
                } else if ( !usesFastObjectDelta ) {
                    m_impl->encodingBaseline["objects"] = current.at("objects");
                    auto cache = makeObjectEncodingCache(beatmap);
                    if ( cache ) {
                        m_impl->objectEncodingCache      = std::move(*cache);
                        m_impl->hasObjectEncodingCache   = true;
                        m_impl->objectEncodingGeneration = 1;
                    } else {
                        m_impl->objectEncodingCache.clear();
                        m_impl->hasObjectEncodingCache = false;
                    }
                }
                return;
            }
            m_impl->encodingBaseline[std::string(category)] =
                current.at(category);
        }
    };
    updateBaseline("objects", ::MMM::BeatmapMutationFlags::Objects);
    updateBaseline("timelines", ::MMM::BeatmapMutationFlags::Timelines);
    updateBaseline("audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples);
    updateBaseline("metadata", ::MMM::BeatmapMutationFlags::Metadata);
    updateBaseline("annotations", ::MMM::BeatmapMutationFlags::Annotations);
    return encoded;
}

void BeatmapDocumentCodec::synchronizeEncodingBaseline(
    const ::MMM::BeatMap& beatmap)
{
    m_impl->encodingBaseline    = makeDocument(beatmap);
    m_impl->hasEncodingBaseline = true;
    auto cache                  = makeObjectEncodingCache(beatmap);
    if ( cache ) {
        m_impl->objectEncodingCache    = std::move(*cache);
        m_impl->hasObjectEncodingCache = true;
    } else {
        m_impl->objectEncodingCache.clear();
        m_impl->hasObjectEncodingCache = false;
    }
    m_impl->objectEncodingGeneration = 1;
}

std::shared_ptr<BeatmapDocumentCodec::ObjectEncodingBaseline>
BeatmapDocumentCodec::prepareObjectEncodingBaseline(
    const ::MMM::BeatMap& beatmap)
{
    auto cache = makeObjectEncodingCache(beatmap);
    if ( !cache ) return nullptr;

    auto baseline     = std::make_shared<ObjectEncodingBaseline>();
    baseline->objects = encodeObjects(beatmap);
    baseline->cache   = std::move(*cache);
    return baseline;
}

void BeatmapDocumentCodec::synchronizeObjectEncodingBaseline(
    std::shared_ptr<ObjectEncodingBaseline> baseline)
{
    if ( !baseline || !m_impl->hasEncodingBaseline ) return;
    m_impl->encodingBaseline["objects"] = std::move(baseline->objects);
    m_impl->objectEncodingCache         = std::move(baseline->cache);
    m_impl->hasObjectEncodingCache      = true;
    m_impl->objectEncodingGeneration    = 1;
}

std::expected<BeatmapPatchResult, BeatmapDocumentError>
BeatmapDocumentCodec::inspect(std::span<const std::uint8_t> payload)
{
    if ( payload.empty() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto decoded = decodeDocumentPayload(payload);
    if ( !decoded.has_value() || !decoded->is_object() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    const Json&   patch    = decoded.value();
    std::uint32_t version  = 0;
    bool          snapshot = false;
    if ( !readValue(patch, "version", version) ||
         version != DOCUMENT_FORMAT_VERSION ||
         !readValue(patch, "snapshot", snapshot) ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }

    BeatmapPatchResult result;
    result.isSnapshot          = snapshot;
    bool       valid           = true;
    const auto inspectCategory = [&](std::string_view            key,
                                     ::MMM::BeatmapMutationFlags flag,
                                     bool                        mustBeArray) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( (mustBeArray && !iterator->is_array()) ||
             (!mustBeArray && !iterator->is_object()) ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };
    const auto inspectDelta = [&](std::string_view            key,
                                  ::MMM::BeatmapMutationFlags flag) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( !iterator->is_object() ) {
            valid = false;
            return;
        }
        const auto added   = iterator->find("added");
        const auto removed = iterator->find("removed");
        if ( added == iterator->end() || !added->is_array() ||
             removed == iterator->end() || !removed->is_array() ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };

    if ( snapshot ) {
        inspectCategory("objects", ::MMM::BeatmapMutationFlags::Objects, true);
        inspectCategory(
            "timelines", ::MMM::BeatmapMutationFlags::Timelines, true);
        inspectCategory(
            "audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples, true);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectCategory(
            "annotations", ::MMM::BeatmapMutationFlags::Annotations, true);
    } else {
        inspectDelta("objects_delta", ::MMM::BeatmapMutationFlags::Objects);
        inspectDelta("timelines_delta", ::MMM::BeatmapMutationFlags::Timelines);
        inspectDelta("audio_samples_delta",
                     ::MMM::BeatmapMutationFlags::AudioSamples);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectDelta("annotations_delta",
                     ::MMM::BeatmapMutationFlags::Annotations);
    }

    if ( !valid ||
         (snapshot && result.flags != ::MMM::BeatmapMutationFlags::All) ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }
    if ( result.flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    return result;
}

std::expected<BeatmapPatchResult, BeatmapDocumentError>
BeatmapDocumentCodec::apply(std::span<const std::uint8_t> payload)
{
    if ( payload.empty() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto decoded = decodeDocumentPayload(payload);
    if ( !decoded.has_value() || !decoded->is_object() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    const Json&   patch    = decoded.value();
    std::uint32_t version  = 0;
    bool          snapshot = false;
    if ( !readValue(patch, "version", version) ||
         version != DOCUMENT_FORMAT_VERSION ||
         !readValue(patch, "snapshot", snapshot) ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    if ( !snapshot && !m_impl->hasDocument ) {
        return std::unexpected(BeatmapDocumentError::MissingSnapshot);
    }

    const bool         updateEncodingBaseline = m_impl->hasEncodingBaseline;
    BeatmapPatchResult result;
    result.isSnapshot          = snapshot;
    bool       valid           = true;
    const auto inspectCategory = [&](std::string_view            key,
                                     ::MMM::BeatmapMutationFlags flag,
                                     bool                        mustBeArray) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( (mustBeArray && !iterator->is_array()) ||
             (!mustBeArray && !iterator->is_object()) ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };
    const auto inspectArrayDelta = [&](std::string_view            deltaKey,
                                       std::string_view            category,
                                       ::MMM::BeatmapMutationFlags flag) {
        const auto deltaIt = patch.find(deltaKey);
        if ( deltaIt == patch.end() ) return;
        const auto addedIt   = deltaIt->find("added");
        const auto removedIt = deltaIt->find("removed");
        const auto targetIt  = m_impl->document.find(category);
        if ( !deltaIt->is_object() || addedIt == deltaIt->end() ||
             !addedIt->is_array() || removedIt == deltaIt->end() ||
             !removedIt->is_array() || targetIt == m_impl->document.end() ||
             !targetIt->is_array() ) {
            valid = false;
            return;
        }
        if ( m_impl->hasEncodingBaseline ) {
            const auto baselineIt = m_impl->encodingBaseline.find(category);
            if ( baselineIt == m_impl->encodingBaseline.end() ||
                 !baselineIt->is_array() ) {
                valid = false;
                return;
            }
        }
        result.flags |= flag;
    };

    if ( snapshot ) {
        inspectCategory("objects", ::MMM::BeatmapMutationFlags::Objects, true);
        inspectCategory(
            "timelines", ::MMM::BeatmapMutationFlags::Timelines, true);
        inspectCategory(
            "audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples, true);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectCategory(
            "annotations", ::MMM::BeatmapMutationFlags::Annotations, true);
    } else {
        inspectArrayDelta(
            "objects_delta", "objects", ::MMM::BeatmapMutationFlags::Objects);
        inspectArrayDelta("timelines_delta",
                          "timelines",
                          ::MMM::BeatmapMutationFlags::Timelines);
        inspectArrayDelta("audio_samples_delta",
                          "audio_samples",
                          ::MMM::BeatmapMutationFlags::AudioSamples);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectArrayDelta("annotations_delta",
                          "annotations",
                          ::MMM::BeatmapMutationFlags::Annotations);
    }

    if ( !valid ||
         (snapshot && result.flags != ::MMM::BeatmapMutationFlags::All) ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }
    if ( result.flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }

    const auto applyArrayDelta = [&](Json&            document,
                                     std::string_view deltaKey,
                                     std::string_view category) {
        const auto deltaIt = patch.find(deltaKey);
        if ( deltaIt == patch.end() ) return;
        const auto addedIt   = deltaIt->find("added");
        const auto removedIt = deltaIt->find("removed");
        auto&      target    = document[std::string(category)];
        const bool replacesByIdentity =
            category == "objects" || category == "annotations";
        if ( replacesByIdentity &&
             applyIdentityDeltaToArray(target, *deltaIt) ) {
            return;
        }
        const auto erasePrevious = [&](const Json& value) {
            const auto existing =
                std::find(target.begin(), target.end(), value);
            if ( existing != target.end() ) target.erase(existing);
        };
        for ( const auto& removed : *removedIt ) {
            erasePrevious(removed);
        }
        for ( const auto& added : *addedIt ) {
            target.push_back(added);
        }
    };

    if ( snapshot ) {
        Json next             = Json::object();
        next["objects"]       = patch.at("objects");
        next["timelines"]     = patch.at("timelines");
        next["audio_samples"] = patch.at("audio_samples");
        next["metadata"]      = patch.at("metadata");
        next["annotations"]   = patch.at("annotations");
        m_impl->document      = std::move(next);
        if ( updateEncodingBaseline ) {
            m_impl->encodingBaseline = m_impl->document;
        }
    } else {
        const auto applyIncremental = [&](Json& document) {
            applyArrayDelta(document, "objects_delta", "objects");
            applyArrayDelta(document, "timelines_delta", "timelines");
            applyArrayDelta(document, "audio_samples_delta", "audio_samples");
            const auto metadata = patch.find("metadata");
            if ( metadata != patch.end() ) document["metadata"] = *metadata;
            applyArrayDelta(document, "annotations_delta", "annotations");
        };
        applyIncremental(m_impl->document);
        if ( updateEncodingBaseline ) {
            applyIncremental(m_impl->encodingBaseline);
        }
    }
    m_impl->hasDocument = true;
    m_impl->objectEncodingCache.clear();
    m_impl->hasObjectEncodingCache   = false;
    m_impl->objectEncodingGeneration = 1;
    return result;
}

std::shared_ptr<::MMM::BeatMap> BeatmapDocumentCodec::materialize() const
{
    if ( !m_impl->hasDocument ) return nullptr;
    const auto objectsIt     = m_impl->document.find("objects");
    const auto timelinesIt   = m_impl->document.find("timelines");
    const auto samplesIt     = m_impl->document.find("audio_samples");
    const auto metadataIt    = m_impl->document.find("metadata");
    const auto annotationsIt = m_impl->document.find("annotations");
    if ( objectsIt == m_impl->document.end() ||
         timelinesIt == m_impl->document.end() ||
         samplesIt == m_impl->document.end() ||
         metadataIt == m_impl->document.end() ||
         annotationsIt == m_impl->document.end() ) {
        return nullptr;
    }

    auto beatmap = std::make_shared<::MMM::BeatMap>();
    if ( !decodeMetadata(*metadataIt, *beatmap) ||
         !decodeObjects(*objectsIt, *beatmap) ||
         !decodeAnnotations(*annotationsIt, *beatmap) ||
         !decodeTimelines(*timelinesIt, *beatmap) ||
         !decodeAudioSamples(*samplesIt, *beatmap) ) {
        return nullptr;
    }
    beatmap->sync();
    return beatmap;
}

std::unique_ptr<BeatmapDocumentCodec>
BeatmapDocumentCodec::cloneDocument() const
{
    if ( !m_impl->hasDocument ) return nullptr;
    auto clone                 = std::make_unique<BeatmapDocumentCodec>();
    clone->m_impl->document    = m_impl->document;
    clone->m_impl->hasDocument = true;
    return clone;
}

std::optional<std::vector<std::string>>
BeatmapDocumentCodec::changedObjectIdentitiesComparedTo(
    const BeatmapDocumentCodec& previous) const
{
    if ( !m_impl->hasDocument || !previous.m_impl->hasDocument ) {
        return std::nullopt;
    }
    const auto currentObjects  = m_impl->document.find("objects");
    const auto previousObjects = previous.m_impl->document.find("objects");
    if ( currentObjects == m_impl->document.end() ||
         previousObjects == previous.m_impl->document.end() ||
         !currentObjects->is_array() || !previousObjects->is_array() ) {
        return std::nullopt;
    }

    std::unordered_map<std::string_view, const Json*> currentByIdentity;
    std::unordered_map<std::string_view, const Json*> previousByIdentity;
    currentByIdentity.reserve(currentObjects->size());
    previousByIdentity.reserve(previousObjects->size());
    const auto indexObjects = [](const Json& objects, auto& index) {
        for ( const auto& object : objects ) {
            const auto* identity = collaborationIdentity(object);
            if ( !identity || !index.emplace(*identity, &object).second ) {
                return false;
            }
        }
        return true;
    };
    if ( !indexObjects(*currentObjects, currentByIdentity) ||
         !indexObjects(*previousObjects, previousByIdentity) ) {
        return std::nullopt;
    }

    std::vector<std::string> changed;
    changed.reserve(currentObjects->size() + previousObjects->size());
    for ( const auto& object : *currentObjects ) {
        const auto& identity = *collaborationIdentity(object);
        const auto  before   = previousByIdentity.find(identity);
        if ( before == previousByIdentity.end() || *before->second != object ) {
            changed.push_back(identity);
        }
    }
    for ( const auto& object : *previousObjects ) {
        const auto& identity = *collaborationIdentity(object);
        if ( !currentByIdentity.contains(identity) ) {
            changed.push_back(identity);
        }
    }
    return changed;
}

std::expected<ByteBuffer, BeatmapDocumentError>
BeatmapDocumentCodec::encodeCurrentSnapshot() const
{
    if ( !m_impl->hasDocument ) {
        return std::unexpected(BeatmapDocumentError::MissingSnapshot);
    }
    Json snapshot        = m_impl->document;
    snapshot["version"]  = DOCUMENT_FORMAT_VERSION;
    snapshot["snapshot"] = true;
    return encodeDocumentPayload(snapshot);
}

bool BeatmapDocumentCodec::hasDocument() const
{
    return m_impl->hasDocument;
}

void BeatmapDocumentCodec::reset()
{
    m_impl->document            = Json::object();
    m_impl->hasDocument         = false;
    m_impl->encodingBaseline    = Json::object();
    m_impl->hasEncodingBaseline = false;
    m_impl->objectEncodingCache.clear();
    m_impl->hasObjectEncodingCache   = false;
    m_impl->objectEncodingGeneration = 1;
}
}  // namespace MMM::Network::Collaboration
