#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace MMM
{

inline bool saveOSUMap(const BeatMap& beatMap, std::filesystem::path path)
{
    using enum MapMetadataType;
    auto map_it = beatMap.m_metadata.map_properties.find(OSU);
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>
                empty_props;
    const auto& osumeta_props =
        (map_it != beatMap.m_metadata.map_properties.end()) ? map_it->second
                                                            : empty_props;

    auto get_prop = [&](const std::string& key,
                        const std::string& default_val) -> std::string {
        if ( auto it = osumeta_props.find(key); it != osumeta_props.end() ) {
            return it->second;
        }
        return default_val;
    };

    std::ofstream ofs(path);
    if ( !ofs.is_open() ) {
        XWARN("打开文件[{}]进行写出失败", Config::pathToUtf8(path));
        return false;
    }

    auto format_ver = get_prop("file_format_version", "v14");
    if ( !format_ver.starts_with("v") ) format_ver = "v" + format_ver;
    ofs << "osu file format " << format_ver << "\n\n";

    ofs << "[General]\n";
    std::string audio_path =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.main_audio_path);
    if ( audio_path.empty() )
        audio_path = get_prop("General::AudioFilename", "");
    ofs << "AudioFilename: " << audio_path << "\n";

    ofs << "AudioLeadIn: " << get_prop("General::AudioLeadIn", "0") << "\n";
    if ( !get_prop("General::AudioHash", "").empty() ) {
        ofs << "AudioHash: " << get_prop("General::AudioHash", "") << "\n";
    }
    ofs << "PreviewTime: " << get_prop("General::PreviewTime", "-1") << "\n";
    ofs << "Countdown: " << get_prop("General::Countdown", "1") << "\n";
    ofs << "SampleSet: " << get_prop("General::SampleSet", "Normal") << "\n";
    ofs << "StackLeniency: " << get_prop("General::StackLeniency", "0.7")
        << "\n";
    ofs << "Mode: " << get_prop("General::Mode", "3")
        << "\n";  // default to mania 3
    ofs << "LetterboxInBreaks: " << get_prop("General::LetterboxInBreaks", "0")
        << "\n";
    ofs << "StoryFireInFront: " << get_prop("General::StoryFireInFront", "1")
        << "\n";
    ofs << "UseSkinSprites: " << get_prop("General::UseSkinSprites", "0")
        << "\n";
    ofs << "AlwaysShowPlayfield: "
        << get_prop("General::AlwaysShowPlayfield", "0") << "\n";
    ofs << "OverlayPosition: "
        << get_prop("General::OverlayPosition", "NoChange") << "\n";
    ofs << "SkinPreference: " << get_prop("General::SkinPreference", "")
        << "\n";
    ofs << "EpilepsyWarning: " << get_prop("General::EpilepsyWarning", "0")
        << "\n";
    ofs << "CountdownOffset: " << get_prop("General::CountdownOffset", "0")
        << "\n";
    ofs << "SpecialStyle: " << get_prop("General::SpecialStyle", "0") << "\n";
    ofs << "WidescreenStoryboard: "
        << get_prop("General::WidescreenStoryboard", "0") << "\n";
    ofs << "SamplesMatchPlaybackRate: "
        << get_prop("General::SamplesMatchPlaybackRate", "0") << "\n";
    ofs << "\n";

    ofs << "[Editor]\n";
    if ( !get_prop("Editor::Bookmarks", "").empty() ) {
        ofs << "Bookmarks: " << get_prop("Editor::Bookmarks", "") << "\n";
    }
    ofs << "DistanceSpacing: " << get_prop("Editor::DistanceSpacing", "0.0")
        << "\n";
    ofs << "BeatDivisor: " << get_prop("Editor::BeatDivisor", "4") << "\n";
    ofs << "GridSize: " << get_prop("Editor::GridSize", "16") << "\n";
    ofs << "TimelineZoom: " << get_prop("Editor::TimelineZoom", "1") << "\n";
    ofs << "\n";

    ofs << "[Metadata]\n";
    ofs << "Title:"
        << (beatMap.m_baseMapMetadata.title.empty()
                ? get_prop("Metadata::Title", "")
                : beatMap.m_baseMapMetadata.title)
        << "\n";
    ofs << "TitleUnicode:"
        << (beatMap.m_baseMapMetadata.title_unicode.empty()
                ? get_prop("Metadata::TitleUnicode", "")
                : beatMap.m_baseMapMetadata.title_unicode)
        << "\n";
    ofs << "Artist:"
        << (beatMap.m_baseMapMetadata.artist.empty()
                ? get_prop("Metadata::Artist", "")
                : beatMap.m_baseMapMetadata.artist)
        << "\n";
    ofs << "ArtistUnicode:"
        << (beatMap.m_baseMapMetadata.artist_unicode.empty()
                ? get_prop("Metadata::ArtistUnicode", "")
                : beatMap.m_baseMapMetadata.artist_unicode)
        << "\n";
    ofs << "Creator:"
        << (beatMap.m_baseMapMetadata.author.empty()
                ? get_prop("Metadata::Creator", "mmm")
                : beatMap.m_baseMapMetadata.author)
        << "\n";
    ofs << "Version:"
        << (beatMap.m_baseMapMetadata.version.empty()
                ? get_prop("Metadata::Version", "[mmm]")
                : beatMap.m_baseMapMetadata.version)
        << "\n";
    ofs << "Source:" << get_prop("Metadata::Source", "") << "\n";
    ofs << "Tags:" << get_prop("Metadata::Tags", "") << "\n";
    ofs << "BeatmapID:" << get_prop("Metadata::BeatmapID", "0") << "\n";
    ofs << "BeatmapSetID:" << get_prop("Metadata::BeatmapSetID", "-1") << "\n";
    ofs << "\n";

    ofs << "[Difficulty]\n";
    ofs << "HPDrainRate:" << get_prop("Difficulty::HPDrainRate", "5") << "\n";
    ofs << "CircleSize:" << beatMap.m_baseMapMetadata.track_count << "\n";
    ofs << "OverallDifficulty:"
        << get_prop("Difficulty::OverallDifficulty", "8") << "\n";
    ofs << "ApproachRate:" << get_prop("Difficulty::ApproachRate", "5") << "\n";
    ofs << "SliderMultiplier:"
        << get_prop("Difficulty::SliderMultiplier", "1.4") << "\n";
    ofs << "SliderTickRate:" << get_prop("Difficulty::SliderTickRate", "1")
        << "\n";
    ofs << "\n";

    ofs << "[Events]\n";
    ofs << "//Background and Video events\n";
    if ( beatMap.m_baseMapMetadata.cover_type == CoverType::IMAGE ) {
        ofs << "0,0,\""
            << Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path)
            << "\"," << beatMap.m_baseMapMetadata.bgxoffset << ","
            << beatMap.m_baseMapMetadata.bgyoffset << "\n";
    } else {
        ofs << "Video," << beatMap.m_baseMapMetadata.video_starttime << ",\""
            << Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path)
            << "\"\n";
    }

    ofs << "//Break Periods\n";
    if ( auto it = osumeta_props.find("Events::breaks");
         it != osumeta_props.end() ) {
        ofs << it->second;
        if ( !it->second.empty() && it->second.back() != '\n' ) {
            ofs << "\n";
        }
    }
    ofs << "//Storyboard Layer 0 (Background)\n";
    ofs << "//Storyboard Layer 1 (Fail)\n";
    ofs << "//Storyboard Layer 2 (Pass)\n";
    ofs << "//Storyboard Layer 3 (Foreground)\n";
    ofs << "//Storyboard Layer 4 (Overlay)\n";
    ofs << "//Storyboard Sound Samples\n";
    ofs << "\n";

    ofs << "[TimingPoints]\n";
    for ( auto& timing_const : beatMap.m_timings ) {
        Timing timing =
            timing_const;  // create a mutable copy to call to_osu_description()

        if ( timing.m_timingEffect == TimingEffect::JUMP ||
             timing.m_timingEffect == TimingEffect::HS ) {
            continue;
        }

        if ( timing.m_timingEffect == TimingEffect::SCROLL ) {
            double scrollMultiplier = normalizeScrollMultiplier(
                timing.m_timingEffectParameter, timing.m_beat_length);
            timing.m_timingEffectParameter = scrollMultiplier;
            timing.m_beat_length           = scrollMultiplier;
        }

        ofs << timing.to_osu_description() << "\n";
    }
    ofs << "\n";

    ofs << "[HitObjects]\n";
    std::unordered_set<const Note*> polyline_subnotes;
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            polyline_subnotes.insert(&subNoteRef.get());
        }
    }

    std::vector<std::reference_wrapper<Note>> export_notes;
    export_notes.reserve(beatMap.m_allNotes.size());
    for ( const auto& noteRef : beatMap.m_allNotes ) {
        Note& note = noteRef.get();
        if ( polyline_subnotes.contains(&note) ) continue;

        if ( note.m_type == NoteType::POLYLINE ) {
            Polyline& polyline = static_cast<Polyline&>(note);
            for ( const auto& subNoteRef : polyline.m_subNotes ) {
                Note& subNote = subNoteRef.get();
                if ( subNote.m_type == NoteType::HOLD ) {
                    export_notes.push_back(subNote);
                }
            }
            continue;
        }

        export_notes.push_back(note);
    }

    std::stable_sort(export_notes.begin(),
                     export_notes.end(),
                     [](const std::reference_wrapper<Note>& a_ref,
                        const std::reference_wrapper<Note>& b_ref) {
                         const Note& a = a_ref.get();
                         const Note& b = b_ref.get();
                         if ( std::abs(a.m_timestamp - b.m_timestamp) > 1e-4 )
                             return a.m_timestamp < b.m_timestamp;
                         if ( a.m_track != b.m_track )
                             return a.m_track < b.m_track;
                         return a.m_type < b.m_type;
                     });

    for ( const auto& noteRef : export_notes ) {
        Note& note = noteRef.get();
        if ( note.m_type == NoteType::HOLD ) {
            Hold& hold = static_cast<Hold&>(note);
            ofs << hold.to_osu_description(
                       beatMap.m_baseMapMetadata.track_count)
                << "\n";
        } else {
            ofs << note.to_osu_description(
                       beatMap.m_baseMapMetadata.track_count)
                << "\n";
        }
    }

    XINFO("Successfully saved osu map to {}", Config::pathToUtf8(path));
    return true;
}

}  // namespace MMM
