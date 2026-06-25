#include "logic/EditorClipboardProtocol.h"

#include "log/colorful-log.h"
#include <cmath>
#include <glm/glm.hpp>
#include <optional>
#include <string>

namespace
{
using MMM::Logic::ClipboardItem;
using MMM::Logic::TimelineClipboardItem;

/// @brief Compare finite doubles with a small tolerance.
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief Compare optional colors exactly enough for protocol round-trip
/// values.
bool sameColor(const std::optional<glm::vec4>& lhs,
               const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) {
        return false;
    }
    if ( !lhs ) {
        return true;
    }
    return near(lhs->r, rhs->r) && near(lhs->g, rhs->g) &&
           near(lhs->b, rhs->b) && near(lhs->a, rhs->a);
}

/// @brief Read one note metadata value without throwing.
std::optional<std::string> noteMetadataValue(const MMM::NoteMetadata& metadata,
                                             MMM::NoteMetadataType    source,
                                             const std::string&       key)
{
    const auto sourceIt = metadata.note_properties.find(source);
    if ( sourceIt == metadata.note_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief Read one timing metadata value without throwing.
std::optional<std::string> timingMetadataValue(
    const MMM::TimingMetadata& metadata, MMM::TimingMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.timing_properties.find(source);
    if ( sourceIt == metadata.timing_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief Verify note clipboard payload serialization and parsing.
bool testNoteRoundTrip()
{
    ClipboardItem item;
    item.note.m_type       = MMM::NoteType::POLYLINE;
    item.note.m_timestamp  = 12.5;
    item.note.m_duration   = 1.25;
    item.note.m_trackIndex = 2;
    item.note.m_dtrack     = 1;
    item.note.m_metadata
        .note_properties[MMM::NoteMetadataType::MMM]["authorNote"] =
        "copy\tline\n%";
    item.note.m_customColors.tap = glm::vec4{ 0.1F, 0.2F, 0.3F, 1.0F };

    MMM::Logic::NoteComponent::SubNote hold;
    hold.type       = MMM::NoteType::HOLD;
    hold.timestamp  = 13.0;
    hold.duration   = 0.75;
    hold.trackIndex = 3;
    hold.dtrack     = 0;
    hold.metadata.note_properties[MMM::NoteMetadataType::OSU]["edge"] = "hold";
    hold.customColors.head = glm::vec4{ 0.4F, 0.5F, 0.6F, 1.0F };

    MMM::Logic::NoteComponent::SubNote flick;
    flick.type       = MMM::NoteType::FLICK;
    flick.timestamp  = 14.0;
    flick.duration   = 0.0;
    flick.trackIndex = 5;
    flick.dtrack     = -1;
    flick.metadata.note_properties[MMM::NoteMetadataType::MALODY]["sound"] =
        "snap";
    flick.customColors.flickArrow = glm::vec4{ 0.7F, 0.8F, 0.9F, 1.0F };

    item.note.m_subNotes  = { hold, flick };
    item.startBeat        = 24.0;
    item.endBeat          = 26.0;
    item.subStartBeats    = { 25.0, 26.0 };
    item.subEndBeats      = { 25.5, 26.0 };
    item.hasBeatPositions = true;

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeNotes({ item });
    if ( !text.starts_with(MMM::Logic::EditorClipboardProtocol::MAGIC) ||
         text.find('{') != std::string::npos ||
         text.find("\"format\"") != std::string::npos ) {
        XERROR("Note clipboard protocol still looks like JSON");
        return false;
    }

    auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text);
    if ( !parsed || parsed->notes.size() != 1 || !parsed->timelines.empty() ) {
        XERROR("Note clipboard protocol did not parse one note item");
        return false;
    }

    const auto& parsedItem = parsed->notes.front();
    const auto& note       = parsedItem.note;
    if ( note.m_type != MMM::NoteType::POLYLINE ||
         !near(note.m_timestamp, 12.5) || !near(note.m_duration, 1.25) ||
         note.m_trackIndex != 2 || note.m_dtrack != 1 ||
         note.m_subNotes.size() != 2 ) {
        XERROR("Note clipboard protocol changed core note fields");
        return false;
    }
    if ( !sameColor(note.m_customColors.tap, item.note.m_customColors.tap) ) {
        XERROR("Note clipboard protocol changed note color override");
        return false;
    }
    const auto noteMetadata = noteMetadataValue(
        note.m_metadata, MMM::NoteMetadataType::MMM, "authorNote");
    if ( !noteMetadata || *noteMetadata != "copy\tline\n%" ) {
        XERROR("Note clipboard protocol changed note metadata");
        return false;
    }
    if ( note.m_subNotes[0].type != MMM::NoteType::HOLD ||
         !near(note.m_subNotes[0].timestamp, 13.0) ||
         !sameColor(note.m_subNotes[0].customColors.head,
                    hold.customColors.head) ) {
        XERROR("Note clipboard protocol changed first sub note");
        return false;
    }
    if ( note.m_subNotes[1].type != MMM::NoteType::FLICK ||
         note.m_subNotes[1].dtrack != -1 ||
         !sameColor(note.m_subNotes[1].customColors.flickArrow,
                    flick.customColors.flickArrow) ) {
        XERROR("Note clipboard protocol changed second sub note");
        return false;
    }
    if ( !parsedItem.hasBeatPositions || !near(parsedItem.startBeat, 24.0) ||
         !near(parsedItem.endBeat, 26.0) ||
         parsedItem.subStartBeats.size() != 2 ||
         !near(parsedItem.subStartBeats[1], 26.0) ||
         parsedItem.subEndBeats.size() != 2 ||
         !near(parsedItem.subEndBeats[0], 25.5) ) {
        XERROR("Note clipboard protocol changed beat offsets");
        return false;
    }

    return true;
}

/// @brief Verify timeline clipboard payload serialization and parsing.
bool testTimelineRoundTrip()
{
    TimelineClipboardItem item;
    item.timeline.m_timestamp = 48.0;
    item.timeline.m_effect    = MMM::TimingEffect::BPM;
    item.timeline.m_value     = 180.0;
    item.timeline.m_metadata
        .timing_properties[MMM::TimingMetadataType::OSU]["inherited"] = "0";
    item.relativeTime                                                 = 1.5;
    item.relativeBeat                                                 = 3.0;
    item.hasBeatPosition                                              = true;

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeTimelines({ item });
    if ( !text.starts_with(MMM::Logic::EditorClipboardProtocol::MAGIC) ||
         text.find('{') != std::string::npos ||
         text.find("\"format\"") != std::string::npos ) {
        XERROR("Timeline clipboard protocol still looks like JSON");
        return false;
    }

    auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text);
    if ( !parsed || parsed->timelines.size() != 1 || !parsed->notes.empty() ) {
        XERROR("Timeline clipboard protocol did not parse one timeline item");
        return false;
    }

    const auto& parsedItem = parsed->timelines.front();
    if ( !near(parsedItem.timeline.m_timestamp, 48.0) ||
         parsedItem.timeline.m_effect != MMM::TimingEffect::BPM ||
         !near(parsedItem.timeline.m_value, 180.0) ||
         !near(parsedItem.relativeTime, 1.5) ||
         !near(parsedItem.relativeBeat, 3.0) || !parsedItem.hasBeatPosition ) {
        XERROR("Timeline clipboard protocol changed timeline fields");
        return false;
    }
    const auto timingMetadata =
        timingMetadataValue(parsedItem.timeline.m_metadata,
                            MMM::TimingMetadataType::OSU,
                            "inherited");
    if ( !timingMetadata || *timingMetadata != "0" ) {
        XERROR("Timeline clipboard protocol changed metadata");
        return false;
    }

    return true;
}

/// @brief Verify plain text is ignored by the MMM clipboard parser.
bool testPlainTextIgnored()
{
    if ( MMM::Logic::EditorClipboardProtocol::parse("plain text") ) {
        XERROR("Clipboard protocol parser accepted plain text");
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    if ( !testNoteRoundTrip() ) return 1;
    if ( !testTimelineRoundTrip() ) return 1;
    if ( !testPlainTextIgnored() ) return 1;
    return 0;
}
