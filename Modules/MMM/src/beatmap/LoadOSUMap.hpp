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

/**
 * @file LoadOSUMap.hpp
 * @brief 将 osu!mania 分章节文本转换为通用 BeatMap 领域对象。
 *
 * 解析分为两层：OsuFileReader 先按章节保存键值行和有序列表，loadOSUMap 再把
 * General、Metadata、Difficulty、Events、TimingPoints 与 HitObjects 映射到通用
 * 字段。来源格式中没有独立自动采样轨，AudioFilename 会迁移为时间零、首条 BGM
 * 轨上的显式 AudioSampleEvent。来源私有字段同时保留在 MapMetadata，供再次写出。
 *
 * 当前有意限制如下：
 *
 * - 目标模式按 mania 轨道语义解释，不实现 osu!standard 滑条几何。
 * - HitObjects 只构造普通 Note 与 Hold，类型位的其余含义保存在来源属性中。
 * - Events 只解释一个优先视频或背景以及 Break 文本。
 * - 背景文件名中的转义逗号不在当前简单切分器覆盖范围内。
 * - TimingPoints 的继承关系由 Timing 字段表示，不在读取器中合并相邻事件。
 * - 单个 AudioFilename 不代表真实文件已存在，只建立项目资源引用提示。
 * - 返回成功只表示完成结构解析，不等价于视觉、音频或游戏规则验收。
 */

/// @brief 原地移除字符串前导空白。
/// @param s 待规整字符串。
static inline void ltrim(std::string& s)
{
    // unsigned char 避免负 char 传给 isspace 产生未定义行为。
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

/// @brief 原地移除字符串尾随空白。
/// @param s 待规整字符串。
static inline void rtrim(std::string& s)
{
    // 反向找到首个非空白字符，再转换为正向删除区间起点。
    s.erase(std::find_if(s.rbegin(),
                         s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

/// @brief 原地移除字符串两端空白。
/// @param s 待规整字符串。
static inline void trim(std::string& s)
{
    // 先去前缀再去后缀，保持中间字段原文不变。
    ltrim(s);
    rtrim(s);
}

/// @brief 收集 osu! 文本章节并保留有序事件、时间点和物件行。
/// @details
/// 普通章节按 `key:value` 保存；Events、TimingPoints 与 HitObjects 没有唯一键，
/// 因此使用递增字符串索引保持文件顺序。该类只做结构分组，不解释领域语义。
/// @par 章节约束
/// - 章节名只接受整行方括号形式，不对大小写做规范化。
/// - 普通键值使用首个冒号分隔，值中的后续冒号保持原样。
/// - TimingPoints 和 HitObjects 允许重复行，必须按索引全部保留。
/// - Events 当前只提升背景、视频与 Break，其余故事板行不进入领域模型。
/// - 重复普通键以后出现的值覆盖前值，匹配常见配置读取习惯。
/// - 该读取器不验证字段数量，领域转换统一通过 SafeParse 处理缺失值。
class OsuFileReader
{
public:
    /// @brief 构造空章节读取器。
    OsuFileReader() = default;
    /// @brief 释放已收集的章节文本。
    ~OsuFileReader() = default;

    /// @brief 按章节和字段键保存的原始值。
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>,
                       StringHash, std::equal_to<>>
        map_properties;

    /// @brief 下一条 TimingPoints 行的顺序索引。
    uint32_t current_timing_index{ 0 };
    /// @brief 下一条 HitObjects 行的顺序索引。
    uint32_t current_hitobject_index{ 0 };
    /// @brief 下一条 Break 事件的顺序索引。
    uint32_t current_breaks_index{ 0 };

    /// @brief 最近读到的方括号章节名。
    std::string current_chapter;

    /// @brief 将单个非空、非注释文本行加入当前章节。
    /// @param line 已移除行尾 CR 的来源行。
    void parse_line(const std::string& line)
    {
        // 空行由外层通常提前过滤，此处仍防御直接调用。
        if ( line.empty() ) return;
        if ( line.front() == '[' && line.back() == ']' ) {
            // 方括号之间的文本成为后续普通行的章节归属。
            current_chapter = line.substr(1, line.size() - 2);
        } else {
            //[Events]	谱面显示设定，故事板事件	逗号分隔的列表
            //[TimingPoints]	时间轴设定	逗号分隔的列表
            //[HitObjects]	击打物件	逗号分隔的列表

            if ( current_chapter == "Events" ) {
                // Events 由首字段区分视频、休息和通用背景，不按冒号解析。
                auto start_5_string = line.substr(0, 5);
                auto start_1_char   = line.front();
                // 视频事件可能使用文本 Video 或数字 1 两种等价格式。
                if ( line.starts_with("Video,") || line.starts_with("1,") ) {
                    map_properties[current_chapter]["background video"] = line;
                } else if ( start_5_string == "Break" ) {
                    // Break 没有稳定键，以递增编号保留全部事件顺序。
                    map_properties[current_chapter]
                                  ["Break" +
                                   std::to_string(current_breaks_index++)] =
                                      line;
                } else if ( start_1_char == '0' ) {
                    // 通用背景事件。
                    map_properties[current_chapter]["background"] = line;
                }
            } else if ( current_chapter == "TimingPoints" ) {
                // 时间点顺序影响继承语义，不能按内容键去重。
                map_properties[current_chapter]
                              [std::to_string(current_timing_index++)] = line;
            } else if ( current_chapter == "HitObjects" ) {
                // 物件同样使用文件顺序索引，允许完全相同的重复行。
                map_properties[current_chapter]
                              [std::to_string(current_hitobject_index++)] =
                                  line;
            } else {
                // 普通章节只接受首个冒号作为键值分隔符，值内冒号保持原样。
                size_t eq_pos = line.find(':');
                if ( eq_pos != std::string::npos ) {
                    std::string key   = line.substr(0, eq_pos);
                    std::string value = line.substr(eq_pos + 1);
                    // 只规整值两端空白，字段键按 osu! 规范原样保存。
                    value.erase(0, value.find_first_not_of(" \t\n\r\f\v"));
                    // 去掉尾随空格
                    value.erase(value.find_last_not_of(" \t\n\r\f\v") + 1);

                    // 同章节重复键以后出现的值为准，匹配常见配置解析行为。
                    map_properties[current_chapter][key] = value;
                }
            }
        }
    }

    /// @brief 读取并转换已收集的章节字段。
    /// @param chapter 章节名。
    /// @param key 字段名或顺序索引。
    /// @param default_value 缺失字段的回退值。
    /// @return 字符串原文或流转换后的目标类型。
    template<typename T>
    T get_value(const std::string& chapter, const std::string& key,
                T default_value = T())
    {
        // 两级查询不创建默认章节或字段，读取不会改变解析结果。
        auto chapter_it = map_properties.find(chapter);
        if ( chapter_it == map_properties.end() ) return default_value;

        auto key_it = chapter_it->second.find(key);
        if ( key_it == chapter_it->second.end() ) return default_value;

        if constexpr ( std::is_same_v<T, std::string> ) {
            // 字符串保留完整原文，其他类型才进行流转换。
            return key_it->second;
        } else {
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

/// @brief 加载 osu! 谱面并迁移其单音频字段。
/// @param path 谱面文件路径。
/// @return 解析出的谱面数据。
/// @details
/// 文件访问和绝对路径转换均使用 error_code 或返回值表达失败。未知或缺失字段
/// 使用格式默认值；玩家轨道数由 CircleSize 四舍五入得到，首个红线时间点提供
/// 参考 BPM。最终调用 sync 重建按时间排序的统一物件视图。
/// @par 映射规则
/// - General::AudioFilename 同时填充旧路径提示和显式自动采样。
/// - Metadata 展示字段提升到 BaseMapMeta，其他键留在 OSU 属性域。
/// - Difficulty::CircleSize 作为 mania 玩家轨道数量来源。
/// - 图片背景保存偏移，视频背景保存开始时间并优先于图片事件。
/// - HitObject 128 映射为 Hold，其他当前支持对象映射为普通 Note。
/// - 红线映射 BPM，绿线映射 SCROLL，首个红线提供参考 BPM。
/// - AudioFilename 非空时自动声明至少一条 BGM 轨。
/// @par 失败边界
/// - 路径不存在、权限失败或文件无法打开时返回空 BeatMap。
/// - 相对路径绝对化失败不阻止继续使用原路径尝试读取。
/// - 首行没有格式签名时会回退重读，不能丢失首个章节。
/// - 空行与两类注释行跳过，不进入来源元数据。
/// - 缺失普通字段使用各自格式默认值，不访问越界数组。
/// - 无效数值由 SafeParse 收敛，不从加载边界抛出异常。
/// - 未解析故事板内容不被表述为完整 osu! storyboard 支持。
inline BeatMap loadOSUMap(std::filesystem::path path)
{
    // 先把调用方路径写入元数据，后续成功转换为绝对路径时再替换。
    BeatMap beatMap;
    beatMap.m_baseMapMetadata.map_path = path;
    BaseMapMeta& basemeta              = beatMap.m_baseMapMetadata;
    // 相对路径转绝对路径只用于确定资源基准；失败时保留原路径继续尝试打开。
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }
    // 保留文件名供后续兼容逻辑使用，路径日志统一转换为 UTF-8。
    auto fname = basemeta.map_path.filename();
    XINFO("载入osu谱面路径:" + Config::pathToUtf8(basemeta.map_path));
    std::ifstream ifs(basemeta.map_path);
    if ( !ifs.is_open() ) {
        XWARN("打开文件[{}]失败", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 来源私有字段集中保存到 OSU 属性域，避免污染其他格式同名键。
    using enum MapMetadataType;
    auto& osumeta_props = beatMap.m_metadata.map_properties[OSU];

    // 首行可能是格式签名；其余有效行交给结构读取器按章节分组。
    OsuFileReader osureader;
    std::string   read_buffer;
    std::getline(ifs, read_buffer);

    // 签名只提取版本尾段，不要求固定的 v 前缀。
    auto cpos = read_buffer.find("format");
    if ( cpos != std::string::npos ) {
        // 取出osu版本
        auto vnum = read_buffer.substr(cpos + 8, read_buffer.size() - 1);
        osumeta_props["file_format_version"] = vnum;
    } else {
        // 首行不是签名时回退到开头，不能丢失它所属的章节或字段。
        ifs.clear();
        // 回到文件开头
        ifs.seekg(0, std::ios::beg);
    }

    while ( std::getline(ifs, read_buffer) ) {
        // 兼容 CRLF：getline 移除 LF 后显式删除末尾 CR。
        if ( !read_buffer.empty() && read_buffer.back() == '\r' ) {
            read_buffer.pop_back();
        }
        // 空行、分号注释和双斜线注释不进入章节属性表。
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
    // AudioFilename 保留相对文本，稍后迁移为首 BGM 轨显式采样。
    auto main_audio_rpath =
        osureader.get_value("General", "AudioFilename", std::string(""));
    // 去掉路径两端空白。
    trim(main_audio_rpath);
    osumeta_props["General::AudioFilename"] = main_audio_rpath;

    // 旧路径与新歌曲提示同时填充，兼容尚未迁移的调用方。
    basemeta.main_audio_path = Config::utf8ToPath(main_audio_rpath);
    basemeta.song_file_hint  = basemeta.main_audio_path;

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

    // Metadata 的常用展示字段提升到 BaseMapMeta，私有字段仍完整保留。
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
    // mania 的 CircleSize 表示键数；先规整空白，再按最近整数转换。
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

    // 内部显示名组合来源标识、键数原文和难度版本，避免覆盖歌曲标题。
    basemeta.name =
        fmt::format("[o!m] [{}k] {}", raw_circle_size, basemeta.version);

    // Colour 段暂不写入项目元数据。

    // 读取 Events 段中的背景配置。
    // osu! 同时存在图片和视频事件时以视频为准；数字 1 与 Video 均表示视频。
    // 视频优先于图片背景；没有事件时使用默认图片占位描述。
    auto background_des =
        osureader.get_value("Events", "background video", std::string{});
    if ( background_des.empty() ) {
        background_des = osureader.get_value(
            "Events", "background", std::string("0,0,\"bg.png\",0,0"));
    }
    osumeta_props["Events::background"] = background_des;
    std::string              token;
    std::istringstream       biss(background_des);
    std::vector<std::string> background_paras;
    // 事件字段按逗号切分；当前兼容范围不解析文件名内部转义逗号。
    while ( std::getline(biss, token, ',') ) {
        background_paras.emplace_back(token);
    }
    auto backgroundType = MMM::Internal::safeAt(background_paras, 0);
    trim(backgroundType);
    if ( backgroundType != "Video" && backgroundType != "1" ) {
        // 只有 Video 和数字 1 归类视频，其余事件按静态图片处理。
        basemeta.cover_type = CoverType::IMAGE;
    } else {
        // 是视频
        basemeta.cover_type = CoverType::VIDEO;
    }
    basemeta.video_starttime =
        MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 1));
    auto cover_path = MMM::Internal::safeAt(background_paras, 2, "\"bg.png\"");
    trim(cover_path);
    // 文件名通常由双引号包围，领域路径只保存去引号后的文本。
    if ( cover_path.starts_with('\"') ) {
        cover_path.replace(cover_path.begin(), cover_path.begin() + 1, "");
        cover_path.replace(cover_path.end() - 1, cover_path.end(), "");
    }
    // 去引号再次trim
    trim(cover_path);

    basemeta.main_cover_path = Config::utf8ToPath(cover_path);
    if ( background_paras.size() >= 5 ) {
        // 静态背景可携带 X/Y 偏移；视频或短事件缺失时回退零值。
        basemeta.bgxoffset =
            MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 3));
        basemeta.bgyoffset =
            MMM::Internal::safeStoi(MMM::Internal::safeAt(background_paras, 4));
    } else {
        basemeta.bgxoffset = 0;
        basemeta.bgyoffset = 0;
    }

    // 休息段以原始多行文本保存，当前领域模型不拆分其开始与结束字段。
    auto& break_events = osumeta_props["Events::breaks"];
    for ( int i = 0; i < osureader.current_breaks_index; i++ ) {
        auto breaks_des = osureader.get_value(
            "Events", std::to_string(i), std::string("2,0,0"));
        // 每条事件补换行，保存器可直接按原顺序写回 Events 段。
        break_events.insert(
            break_events.end(), breaks_des.begin(), breaks_des.end());
        break_events.push_back('\n');
    }

    // HitObjects 按来源顺序解析为 Hold 或普通 Note 两类当前可表达对象。
    for ( int i = 0; i < osureader.current_hitobject_index; i++ ) {
        // 缺失索引使用合法默认行，使截断输入仍得到可诊断的领域值。
        auto note_des =
            osureader.get_value("HitObjects",
                                std::to_string(i),
                                std::string("469,192,1846,1,0,0:0:0:0:"));
        std::istringstream       noteiss(note_des);
        std::vector<std::string> note_paras;
        while ( std::getline(noteiss, token, ',') ) {
            note_paras.emplace_back(token);
        }

        // osu!mania 类型值 128 表示 Hold，其余当前统一按普通 Note 导入。
        if ( static_cast<int32_t>(MMM::Internal::safeStod(
                 MMM::Internal::safeAt(note_paras, 3))) == 128 ) {
            Hold hold;
            hold.from_osu_description(note_paras, basemeta.track_count);
            // Hold 内容末尾由起点加持续时间决定。
            if ( hold.m_timestamp + hold.m_duration > basemeta.map_length )
                basemeta.map_length = hold.m_timestamp + hold.m_duration;
            // 把长条物件加入列表
            beatMap.m_noteData.holds.push_back(hold);
        } else {
            Note note;
            // 普通物件复用 Note 的字段级安全解析和 HitSample 迁移。
            note.from_osu_description(note_paras, basemeta.track_count);
            // 更新谱面时长
            if ( int64_t(note.m_timestamp) > basemeta.map_length )
                basemeta.map_length = note.m_timestamp;

            // 加入物件列表
            beatMap.m_noteData.notes.push_back(note);
        }
    }

    // TimingPoints 保持来源顺序，首个红线为谱面参考 BPM。
    bool firstBpmSet = false;
    for ( int i = 0; i < osureader.current_timing_index; i++ ) {
        // 缺失行使用合法红线默认值，避免字段访问越界。
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
        // Timing 自身区分红线 BPM 与绿线 SCROLL 语义。
        timing.from_osu_description(timing_point_paras);
        // 添加到timing表
        beatMap.m_timings.push_back(timing);

        // 绿线没有独立 BPM，不能作为 preference_bpm 来源。
        if ( !firstBpmSet && timing.m_timingEffect == TimingEffect::BPM ) {
            beatMap.m_baseMapMetadata.preference_bpm = timing.m_bpm;
            firstBpmSet                              = true;
        }
    }

    // osu! 单个 AudioFilename 迁移为时间零、首条 BGM 轨的一倍音量采样。
    if ( !basemeta.song_file_hint.empty() ) {
        AudioSampleEvent& sample = beatMap.m_audioSamples.emplace_back();
        sample.m_timestamp       = 0.0;
        sample.m_offsetMs        = 0;
        // BGM 轨紧跟玩家轨，非法负键数先收敛到零。
        sample.m_track =
            static_cast<uint32_t>(std::max(0, basemeta.track_count));
        sample.m_audioResourceId = Config::pathToUtf8(basemeta.song_file_hint);
        sample.m_volume          = 1.0F;
        // 有全局音频时至少声明一条可承载该采样的 BGM 轨。
        basemeta.bgm_track_count = std::max(1, basemeta.bgm_track_count);
    }

    // 分类容器填充完成后建立确定性顶层物件视图。
    beatMap.sync();

    // 返回对象仍保留来源绝对路径，项目层可据此解析相对媒体文件。
    // 解析器不在此处验证音频与背景资源是否实际存在。
    // 调用方需要以容器内容判断空谱面，而不是依赖异常状态。
    return beatMap;
}

}  // namespace MMM
