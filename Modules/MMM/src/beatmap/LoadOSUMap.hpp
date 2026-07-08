#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace MMM
{

// 原地移除字符串前导空白。
static inline void ltrim(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

// 原地移除字符串尾随空白。
static inline void rtrim(std::string& s)
{
    s.erase(std::find_if(s.rbegin(),
                         s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

// 原地移除字符串两端空白。
static inline void trim(std::string& s)
{
    ltrim(s);
    rtrim(s);
}

class OsuFileReader
{
public:
    // 构造OsuFileReader
    OsuFileReader() = default;
    // 析构OsuFileReader
    ~OsuFileReader() = default;

    // 格式化过的属性
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>,
                       StringHash, std::equal_to<>>
        map_properties;

    // 当前索引
    uint32_t current_timing_index{ 0 };
    uint32_t current_hitobject_index{ 0 };
    uint32_t current_breaks_index{ 0 };

    // 当前章节
    std::string current_chapter;

    // 格式化行
    void parse_line(const std::string& line)
    {
        if ( line.empty() ) return;
        if ( line.front() == '[' && line.back() == ']' ) {
            current_chapter = line.substr(1, line.size() - 2);
        } else {
            //[Events]	谱面显示设定，故事板事件	逗号分隔的列表
            //[TimingPoints]	时间轴设定	逗号分隔的列表
            //[HitObjects]	击打物件	逗号分隔的列表

            if ( current_chapter == "Events" ) {
                auto start_5_string = line.substr(0, 5);
                auto start_1_char   = line.front();
                // 并非一定五个参数
                if ( start_5_string == "Video" ) {
                    map_properties[current_chapter]["background video"] = line;
                } else if ( start_5_string == "Break" ) {
                    map_properties[current_chapter]
                                  ["Break" +
                                   std::to_string(current_breaks_index++)] =
                                      line;
                } else if ( start_1_char == '0' ) {
                    // 通用背景事件。
                    map_properties[current_chapter]["background"] = line;
                }
            } else if ( current_chapter == "TimingPoints" ) {
                map_properties[current_chapter]
                              [std::to_string(current_timing_index++)] = line;
            } else if ( current_chapter == "HitObjects" ) {
                map_properties[current_chapter]
                              [std::to_string(current_hitobject_index++)] =
                                  line;
            } else {
                size_t eq_pos = line.find(':');
                if ( eq_pos != std::string::npos ) {
                    std::string key   = line.substr(0, eq_pos);
                    std::string value = line.substr(eq_pos + 1);
                    // 去掉前导空格
                    value.erase(0, value.find_first_not_of(" \t\n\r\f\v"));
                    // 去掉尾随空格
                    value.erase(value.find_last_not_of(" \t\n\r\f\v") + 1);

                    map_properties[current_chapter][key] = value;
                }
            }
        }
    }

    // 获取数据
    template<typename T>
    T get_value(const std::string& chapter, const std::string& key,
                T default_value = T())
    {
        auto chapter_it = map_properties.find(chapter);
        if ( chapter_it == map_properties.end() ) return default_value;

        auto key_it = chapter_it->second.find(key);
        if ( key_it == chapter_it->second.end() ) return default_value;

        if constexpr ( std::is_same_v<T, std::string> ) {
            // 类型为字符串时整个返回
            return key_it->second;
        } else {
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

inline BeatMap loadOSUMap(std::filesystem::path path)
{
    BeatMap beatMap;
    beatMap.m_baseMapMetadata.map_path = path;
    BaseMapMeta& basemeta              = beatMap.m_baseMapMetadata;
    // 切换谱面路径为绝对路径 (使用 error_code 避免异常)
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }
    auto fname = basemeta.map_path.filename();
    XINFO("载入osu谱面路径:" + Config::pathToUtf8(basemeta.map_path));
    std::ifstream ifs(basemeta.map_path);
    if ( !ifs.is_open() ) {
        XWARN("打开文件[{}]失败", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 创建osu元数据
    using enum MapMetadataType;
    auto& osumeta_props = beatMap.m_metadata.map_properties[OSU];

    /// 开始解析osu文件
    OsuFileReader osureader;
    std::string   read_buffer;
    std::getline(ifs, read_buffer);

    auto cpos = read_buffer.find("format");
    if ( cpos != std::string::npos ) {
        // 取出osu版本
        auto vnum = read_buffer.substr(cpos + 8, read_buffer.size() - 1);
        osumeta_props["file_format_version"] = vnum;
    } else {
        // 清除可能的 EOF 标志
        ifs.clear();
        // 回到文件开头
        ifs.seekg(0, std::ios::beg);
    }

    while ( std::getline(ifs, read_buffer) ) {
        if ( !read_buffer.empty() && read_buffer.back() == '\r' ) {
            read_buffer.pop_back();
        }
        if (  // 未读到内容
            read_buffer.empty() ||
            // 直接结束
            read_buffer[0] == ';' ||
            // 注释
            (read_buffer.size() >= 2 && read_buffer[0] == '/' &&
             read_buffer[1] == '/') )
            continue;
        osureader.parse_line(read_buffer);
    }

    // 读取 osu 谱面的 General 段。
    // 主音频文件相对路径
    auto main_audio_rpath =
        osureader.get_value("General", "AudioFilename", std::string(""));
    // 去掉路径两端空白。
    trim(main_audio_rpath);
    osumeta_props["General::AudioFilename"] = main_audio_rpath;

    basemeta.main_audio_path = Config::utf8ToPath(main_audio_rpath);

    osumeta_props["General::AudioLeadIn"] =
        osureader.get_value("General", "AudioLeadIn", std::string("0"));
    osumeta_props["General::AudioHash"] =
        osureader.get_value("General", "AudioHash", std::string(""));
    osumeta_props["General::PreviewTime"] =
        osureader.get_value("General", "PreviewTime", std::string("-1"));
    osumeta_props["General::Countdown"] =
        osureader.get_value("General", "Countdown", std::string("1"));

    osumeta_props["General::SampleSet"] =
        osureader.get_value("General", "SampleSet", std::string(""));

    osumeta_props["General::StackLeniency"] =
        osureader.get_value("General", "StackLeniency", std::string("0.0"));
    osumeta_props["General::Mode"] =
        osureader.get_value("General", "Mode", std::string("0"));
    osumeta_props["General::LetterboxInBreaks"] = osureader.get_value(
        "General", "LetterboxInBreaks", std::string("false"));
    osumeta_props["General::StoryFireInFront"] =
        osureader.get_value("General", "StoryFireInFront", std::string("true"));
    osumeta_props["General::UseSkinSprites"] =
        osureader.get_value("General", "UseSkinSprites", std::string("false"));
    osumeta_props["General::AlwaysShowPlayfield"] = osureader.get_value(
        "General", "AlwaysShowPlayfield", std::string("false"));
    osumeta_props["General::OverlayPosition"] = osureader.get_value(
        "General", "OverlayPosition", std::string("NoChange"));
    osumeta_props["General::SkinPreference"] =
        osureader.get_value("General", "SkinPreference", std::string(""));
    osumeta_props["General::EpilepsyWarning"] =
        osureader.get_value("General", "EpilepsyWarning", std::string("false"));
    osumeta_props["General::CountdownOffset"] =
        osureader.get_value("General", "CountdownOffset", std::string("0"));
    osumeta_props["General::SpecialStyle"] =
        osureader.get_value("General", "SpecialStyle", std::string("false"));
    osumeta_props["General::WidescreenStoryboard"] = osureader.get_value(
        "General", "WidescreenStoryboard", std::string("false"));
    osumeta_props["General::SamplesMatchPlaybackRate"] = osureader.get_value(
        "General", "SamplesMatchPlaybackRate", std::string("false"));

    // 读取 osu 谱面的 Editor 段。
    // Bookmarks 是逗号分隔的 Integer（整型）数组。
    // 书签（蓝线）的位置（毫秒）
    osumeta_props["Editor::Bookmarks"] =
        osureader.get_value("Editor", "Bookmarks", std::string(""));
    osumeta_props["Editor::DistanceSpacing"] =
        osureader.get_value("Editor", "DistanceSpacing", std::string("0.0"));
    osumeta_props["Editor::BeatDivisor"] =
        osureader.get_value("Editor", "BeatDivisor", std::string("0"));
    osumeta_props["Editor::GridSize"] =
        osureader.get_value("Editor", "GridSize", std::string("0"));
    osumeta_props["Editor::TimelineZoom"] =
        osureader.get_value("Editor", "TimelineZoom", std::string("0.0"));

    // 读取 osu 谱面的 Metadata 段。
    osumeta_props["Metadata::Title"] =
        osureader.get_value("Metadata", "Title", std::string(""));
    basemeta.title = osumeta_props["Metadata::Title"];

    osumeta_props["Metadata::TitleUnicode"] =
        osureader.get_value("Metadata", "TitleUnicode", std::string(""));
    basemeta.title_unicode = osumeta_props["Metadata::TitleUnicode"];

    osumeta_props["Metadata::Artist"] =
        osureader.get_value("Metadata", "Artist", std::string(""));
    basemeta.artist = osumeta_props["Metadata::Artist"];

    osumeta_props["Metadata::ArtistUnicode"] =
        osureader.get_value("Metadata", "ArtistUnicode", std::string(""));
    basemeta.artist_unicode = osumeta_props["Metadata::ArtistUnicode"];

    osumeta_props["Metadata::Creator"] =
        osureader.get_value("Metadata", "Creator", std::string("mmm"));
    basemeta.author = osumeta_props["Metadata::Creator"];

    osumeta_props["Metadata::Version"] =
        osureader.get_value("Metadata", "Version", std::string("[mmm]"));
    basemeta.version = osumeta_props["Metadata::Version"];

    osumeta_props["Metadata::Source"] =
        osureader.get_value("Metadata", "Source", std::string(""));
    // ***Tags	空格分隔的 String（字符串）数组	易于搜索的标签
    osumeta_props["Metadata::Tags"] =
        osureader.get_value("Metadata", "Tags", std::string(""));
    osumeta_props["Metadata::BeatmapID"] =
        osureader.get_value("Metadata", "BeatmapID", std::string("-1"));
    osumeta_props["Metadata::BeatmapSetID"] =
        osureader.get_value("Metadata", "BeatmapSetID", std::string("-1"));

    // 读取 osu 谱面的 Difficulty 段。
    osumeta_props["Difficulty::HPDrainRate"] =
        osureader.get_value("Difficulty", "HPDrainRate", std::string("5.0"));
    auto raw_circle_size =
        osureader.get_value("Difficulty", "CircleSize", std::string("4.0"));
    trim(raw_circle_size);
    osumeta_props["Difficulty::CircleSize"] = raw_circle_size;
    basemeta.track_count                    = static_cast<int32_t>(
        std::round(MMM::Internal::safeStod(raw_circle_size, 4.0)));

    osumeta_props["Difficulty::OverallDifficulty"] = osureader.get_value(
        "Difficulty", "OverallDifficulty", std::string("8.0"));
    osumeta_props["Difficulty::ApproachRate"] =
        osureader.get_value("Difficulty", "ApproachRate", std::string("0.0"));
    osumeta_props["Difficulty::SliderMultiplier"] = osureader.get_value(
        "Difficulty", "SliderMultiplier", std::string("0.0"));
    osumeta_props["Difficulty::SliderTickRate"] =
        osureader.get_value("Difficulty", "SliderTickRate", std::string("0.0"));

    // 生成图名
    basemeta.name =
        fmt::format("[o!m] [{}k] {}", raw_circle_size, basemeta.version);

    // Colour 段暂不写入项目元数据。

    // 读取 Events 段中的背景配置。
    auto background_des = osureader.get_value(
        "Events", "background", std::string("0,0,\"bg.png\",0,0"));
    osumeta_props["Events::background"] = background_des;
    std::string              token;
    std::istringstream       biss(background_des);
    std::vector<std::string> background_paras;
    while ( std::getline(biss, token, ',') ) {
        background_paras.emplace_back(token);
    }
    if ( MMM::Internal::safeAt(background_paras, 0) == "0" ) {
        // 是图片
        basemeta.cover_type = CoverType::IMAGE;
    } else {
        // 是视频
        basemeta.cover_type = CoverType::VIDEO;
    }
    basemeta.video_starttime =
        MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 1));
    auto cover_path = MMM::Internal::safeAt(background_paras, 2, "\"bg.png\"");
    trim(cover_path);
    // 去引号
    if ( cover_path.starts_with('\"') ) {
        cover_path.replace(cover_path.begin(), cover_path.begin() + 1, "");
        cover_path.replace(cover_path.end() - 1, cover_path.end(), "");
    }
    // 去引号再次trim
    trim(cover_path);

    basemeta.main_cover_path = Config::utf8ToPath(cover_path);
    if ( background_paras.size() >= 5 ) {
        basemeta.bgxoffset =
            MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 3));
        basemeta.bgyoffset =
            MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 4));
    } else {
        basemeta.bgxoffset = 0;
        basemeta.bgyoffset = 0;
    }

    // 读取 Events 段中的休息段。
    auto& break_events = osumeta_props["Events::breaks"];
    for ( int i = 0; i < osureader.current_breaks_index; i++ ) {
        auto breaks_des = osureader.get_value(
            "Events", std::to_string(i), std::string("2,0,0"));
        // 添加一个休息段
        break_events.insert(
            break_events.end(), breaks_des.begin(), breaks_des.end());
        break_events.push_back('\n');
    }

    // 读取创建物件
    for ( int i = 0; i < osureader.current_hitobject_index; i++ ) {
        // 按顺序读取物件
        auto note_des =
            osureader.get_value("HitObjects",
                                std::to_string(i),
                                std::string("469,192,1846,1,0,0:0:0:0:"));
        std::istringstream       noteiss(note_des);
        std::vector<std::string> note_paras;
        while ( std::getline(noteiss, token, ',') ) {
            note_paras.emplace_back(token);
        }

        // 创建物件
        if ( static_cast<int32_t>(MMM::Internal::safeStod(
                 MMM::Internal::safeAt(note_paras, 3))) == 128 ) {
            Hold hold;
            hold.from_osu_description(note_paras, basemeta.track_count);
            // 更新谱面时长
            if ( hold.m_timestamp + hold.m_duration > basemeta.map_length )
                basemeta.map_length = hold.m_timestamp + hold.m_duration;
            // 把长条物件加入列表
            beatMap.m_noteData.holds.push_back(hold);
        } else {
            Note note;
            // 使用读取出的参数初始化物件
            note.from_osu_description(note_paras, basemeta.track_count);
            // 更新谱面时长
            if ( int64_t(note.m_timestamp) > basemeta.map_length )
                basemeta.map_length = note.m_timestamp;

            // 加入物件列表
            beatMap.m_noteData.notes.push_back(note);
        }
    }

    // 创建timing
    bool firstBpmSet = false;
    for ( int i = 0; i < osureader.current_timing_index; i++ ) {
        // 按顺序读取timing点
        auto timing_point_des =
            osureader.get_value("TimingPoints",
                                std::to_string(i),
                                std::string("10000,333.33,4,0,0,100,1,1"));
        std::istringstream       timingiss(timing_point_des);
        std::vector<std::string> timing_point_paras;
        while ( std::getline(timingiss, token, ',') ) {
            timing_point_paras.emplace_back(token);
        }

        // 创建timing
        Timing timing;
        // 使用读取出的参数初始化timing
        timing.from_osu_description(timing_point_paras);
        // 添加到timing表
        beatMap.m_timings.push_back(timing);

        // 设置预设 BPM (取第一个红线点)
        if ( !firstBpmSet && timing.m_timingEffect == TimingEffect::BPM ) {
            beatMap.m_baseMapMetadata.preference_bpm = timing.m_bpm;
            firstBpmSet                              = true;
        }
    }

    beatMap.sync();

    return beatMap;
}

}  // namespace MMM
