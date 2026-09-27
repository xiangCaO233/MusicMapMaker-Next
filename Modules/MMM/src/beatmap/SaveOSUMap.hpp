#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace MMM
{

/**
 * @file SaveOSUMap.hpp
 * @brief 将通用 BeatMap 降级写出为 osu!mania 文本谱面。
 *
 * osu! 格式只能隐式表达一个全局音频和一条 BGM 轨，因此写出前必须验证自动
 * 采样的数量、时间、偏移、轨道、音量和资源引用。普通 Note、Hold 与可展开的
 * Polyline Hold 子节点写入 HitObjects；无法由当前展开规则表达的采样绑定会整体
 * 拒绝，不能静默丢失。JUMP、HS 和非法 SCROLL 不写入 TimingPoints。
 *
 * 来源 OSU 元数据用于保留通用模型未提升的字段；BaseMapMeta 中的标题、作者、
 * 轨道、背景等通用字段优先。函数在完成全部可表达性预检后才打开目标文件，
 * 避免已知失败留下部分输出。
 */

/// @brief 将谱面保存为 osu! 文本格式。
/// @param beatMap 待保存谱面。
/// @param path 输出路径。
/// @return 保存成功时返回 true；采样时间线无法无损表达时返回 false。
/// @details
/// 写出顺序固定为文件头、General、Editor、Metadata、Difficulty、Events、
/// TimingPoints 与 HitObjects。物件按时间、轨道和类型稳定排序，保证相同领域
/// 数据得到确定文本。函数不修改输入谱面，需调用非 const 格式接口时使用值副本。
/// @par 格式限制
/// - 最多一个时间零、零偏移、一倍音量的全局自动采样。
/// - 自动采样必须位于第一条 BGM 轨，显式 BGM 轨数只能为零或一。
/// - Polyline 根及非 Hold 子节点的采样绑定当前不可无损表达。
/// - JUMP、HS 不存在对应 osu! TimingPoint 表示，当前跳过。
/// - SCROLL 倍率必须为正，非法事件不写入输出。
/// - Flick 按普通瞬时 HitObject 降级，其方向不在 osu!mania 中持久化。
/// @par 字段优先级
/// - 文件格式版本沿用来源 OSU 元数据，缺失时使用 v14。
/// - AudioFilename 只来自合法的显式自动采样资源 ID。
/// - General 与 Editor 私有字段沿用来源值，缺失时使用稳定默认值。
/// - 标题、艺术家、作者和难度优先使用 BaseMapMeta 非空字段。
/// - Metadata 私有 Source、Tags 与在线 ID 继续沿用来源属性。
/// - CircleSize 始终由当前玩家轨道数生成，不能沿用旧来源键数。
/// - 背景类型、路径、视频起点和图片偏移来自当前通用元数据。
/// - Break 事件按来源多行文本原样追加，其他故事板当前不重建。
/// - Timing 的音效组等私有字段由 Timing::to_osu_description 合并。
/// - HitSample 自定义文件名由当前 Note 采样绑定覆盖旧来源值。
/// @par 提交边界
/// - 所有无法表达的采样和 BGM 轨条件在打开目标文件前检查。
/// - 目标文件打开失败返回 false，不尝试备用路径或格式。
/// - 写出过程不修改 const BeatMap，历史非 const API 只接收值副本。
/// - 生成顺序固定，便于文本比较和版本控制审阅。
/// - 函数当前依赖 ofstream 生命周期关闭文件，不执行额外原子替换。
/// - 到达成功日志表示字段循环完成，不等价于磁盘持久性 fsync 保证。
/// @par 物件降级
/// - 独立 Note 写为普通 HitObject。
/// - 独立 Hold 写为带绝对终点的 Hold HitObject。
/// - 顶层 Flick 使用普通 HitObject，滑动增量无法保存。
/// - Polyline 根不写出，Hold 子节点作为独立 Hold 展开。
/// - Polyline 非 Hold 子节点当前不写出，因此有采样绑定时预检拒绝。
/// - m_allNotes 必须已由 BeatMap::sync 建立，保存器不隐式重建视图。
/// - 相同时间对象以轨道和类型次键排序，完全同键保持原相对顺序。
/// @par 失败定位
/// - 绑定错误优先检查 Polyline 根与子节点可表达性扫描。
/// - BGM 轨数错误与自动采样数量错误分别报告，不混为单音频失败。
/// - 自动采样详细诊断列出六个必须满足的字段。
/// - 文件打开错误记录 UTF-8 目标路径。
/// - 未知来源属性不会导致失败，而是使用对应章节默认值。
/// - 非正 SCROLL 被跳过；这属于损坏事件降级而非整个保存失败。
inline bool saveOSUMap(const BeatMap& beatMap, std::filesystem::path path)
{
    using enum MapMetadataType;

    /// @brief 判断玩家物件是否包含 osu! HitObject 无法无损表达的采样绑定。
    ///
    /// 普通 Note、Hold 及顶层 Flick 均可复用 HitSample 的 sampleFile
    /// 字段；Polyline 根节点不会直接写出，非 Hold 子节点也不会进入当前
    /// osu! 展开结果，因此仅拒绝这些确实会丢失的绑定。
    const auto hasUnsupportedNoteSampleBinding = [&beatMap]() {
        // 只需检查 Polyline 域；顶层 Note、Hold 与 Flick 可复用 HitSample
        // 文件名。
        return std::any_of(
            beatMap.m_noteData.polylines.begin(),
            beatMap.m_noteData.polylines.end(),
            [](const Polyline& polyline) {
                // 根折线不直接写出 HitObject，其绑定没有可承载位置。
                if ( polyline.getSampleBinding() ) return true;
                return std::any_of(
                    polyline.m_subNotes.begin(),
                    polyline.m_subNotes.end(),
                    [](const auto& noteRef) {
                        const Note& note = noteRef.get();
                        // 只有 Hold 子节点会按当前展开规则进入 HitObjects。
                        return note.m_type != NoteType::HOLD &&
                               note.getSampleBinding().has_value();
                    });
            });
    };
    if ( hasUnsupportedNoteSampleBinding() ) {
        // 可表达性失败发生在创建目标文件之前，保留已有文件内容不变。
        XERROR(
            "osu! 导出失败：Polyline 根节点或非 Hold 子节点的采样绑定"
            "无法由当前 HitObject 展开逻辑无损表达");
        return false;
    }

    // 没有全局音频时 osu! 隐式表达零条 BGM 轨，有音频时恰好一条。
    const int representableBgmTrackCount =
        beatMap.m_audioSamples.empty() ? 0 : 1;
    if ( beatMap.m_baseMapMetadata.bgm_track_count !=
         representableBgmTrackCount ) {
        // 多条或空占位 BGM 轨无法在 osu! 文件中无损保留。
        XERROR(
            "osu! 导出失败：格式只能由单音频隐式表达 {} 条 BGM "
            "轨，当前谱面显式保存了 {} 条",
            representableBgmTrackCount,
            beatMap.m_baseMapMetadata.bgm_track_count);
        return false;
    }

    // 来源私有字段可选；非 osu! 来源谱面使用下面各节的标准默认值。
    auto map_it = beatMap.m_metadata.map_properties.find(OSU);
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>
                empty_props;
    const auto& osumeta_props =
        (map_it != beatMap.m_metadata.map_properties.end()) ? map_it->second
                                                            : empty_props;

    /// @brief 读取保留的 osu! 私有字段并提供规范默认值。
    /// @param key 带章节前缀的属性键。
    /// @param default_val 来源未保存该字段时的输出值。
    /// @return 保留原文或默认文本。
    auto get_prop = [&](const std::string& key,
                        const std::string& default_val) -> std::string {
        // 查找不插入属性，保存操作不会改变 const 输入谱面的元数据。
        if ( auto it = osumeta_props.find(key); it != osumeta_props.end() ) {
            return it->second;
        }
        return default_val;
    };

    // 指针只观察 beatMap 拥有的稳定 vector 元素，写出期间不修改容器。
    const AudioSampleEvent* legacyAudioSample = nullptr;
    if ( beatMap.m_audioSamples.size() > 1 ) {
        // osu! General 只有一个 AudioFilename，多个事件不能选择性丢弃。
        XERROR(
            "osu! 导出失败：格式只能表达一个全局音频，当前有 {} 个自动采样对象",
            beatMap.m_audioSamples.size());
        return false;
    }
    if ( !beatMap.m_audioSamples.empty() ) {
        // 唯一事件必须等价于 osu! 的隐式全局音频播放语义。
        legacyAudioSample = &beatMap.m_audioSamples.front();
        // 第一条 BGM 轨紧跟玩家轨，负玩家轨数防御性收敛到零。
        const uint32_t firstBgmTrack = static_cast<uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        // 所有约束一次检查，失败日志输出实际字段便于定位不可表达原因。
        if ( legacyAudioSample->m_audioResourceId.empty() ||
             !std::isfinite(legacyAudioSample->m_timestamp) ||
             std::abs(legacyAudioSample->m_timestamp) > 1e-6 ||
             legacyAudioSample->m_offsetMs != 0 ||
             legacyAudioSample->m_track != firstBgmTrack ||
             !std::isfinite(legacyAudioSample->m_volume) ||
             std::abs(legacyAudioSample->m_volume - 1.0F) > 1e-6F ) {
            XERROR(
                "osu! 导出失败：自动采样必须是 timestamp=0、offset=0、"
                "track={}、volume=1 且音频引用非空；当前为 "
                "timestamp={}、offset={}、track={}、volume={}、ref='{}'",
                firstBgmTrack,
                legacyAudioSample->m_timestamp,
                legacyAudioSample->m_offsetMs,
                legacyAudioSample->m_track,
                legacyAudioSample->m_volume,
                legacyAudioSample->m_audioResourceId);
            return false;
        }
    }

    // 全部领域可表达性预检完成后才创建或截断目标文件。
    std::ofstream ofs(path);
    if ( !ofs.is_open() ) {
        XWARN("打开文件[{}]进行写出失败", Config::pathToUtf8(path));
        return false;
    }

    // 来源版本允许带或不带 v，输出文件头统一补齐标准前缀。
    auto format_ver = get_prop("file_format_version", "v14");
    if ( !format_ver.starts_with("v") ) format_ver = "v" + format_ver;
    ofs << "osu file format " << format_ver << "\n\n";

    // General 保存音频引用和运行方式等不属于通用领域模型的原始字段。
    ofs << "[General]\n";
    std::string audio_path;
    if ( legacyAudioSample != nullptr ) {
        // 使用显式自动采样资源 ID，不再从旧 main_audio_path 合成音频。
        audio_path = legacyAudioSample->m_audioResourceId;
    }
    ofs << "AudioFilename: " << audio_path << "\n";

    // AudioLeadIn 与 PreviewTime 保留来源编辑和试听时间语义。
    ofs << "AudioLeadIn: " << get_prop("General::AudioLeadIn", "0") << "\n";
    if ( !get_prop("General::AudioHash", "").empty() ) {
        ofs << "AudioHash: " << get_prop("General::AudioHash", "") << "\n";
    }
    ofs << "PreviewTime: " << get_prop("General::PreviewTime", "-1") << "\n";
    ofs << "Countdown: " << get_prop("General::Countdown", "1") << "\n";
    // SampleSet 和 StackLeniency 没有通用字段，按来源原文回写。
    ofs << "SampleSet: " << get_prop("General::SampleSet", "Normal") << "\n";
    ofs << "StackLeniency: " << get_prop("General::StackLeniency", "0.7")
        << "\n";
    // 缺失 Mode 时默认写出 mania 的稳定数值 3。
    ofs << "Mode: " << get_prop("General::Mode", "3")
        << "\n";  // default to mania 3
    ofs << "LetterboxInBreaks: " << get_prop("General::LetterboxInBreaks", "0")
        << "\n";
    // 故事板显示开关属于 General 私有配置，不由背景通用字段推导。
    ofs << "StoryFireInFront: " << get_prop("General::StoryFireInFront", "1")
        << "\n";
    ofs << "UseSkinSprites: " << get_prop("General::UseSkinSprites", "0")
        << "\n";
    ofs << "AlwaysShowPlayfield: "
        << get_prop("General::AlwaysShowPlayfield", "0") << "\n";
    ofs << "OverlayPosition: "
        << get_prop("General::OverlayPosition", "NoChange") << "\n";
    // 皮肤与光敏提示保持来源值，跨格式新谱面使用安全默认值。
    ofs << "SkinPreference: " << get_prop("General::SkinPreference", "")
        << "\n";
    ofs << "EpilepsyWarning: " << get_prop("General::EpilepsyWarning", "0")
        << "\n";
    ofs << "CountdownOffset: " << get_prop("General::CountdownOffset", "0")
        << "\n";
    ofs << "SpecialStyle: " << get_prop("General::SpecialStyle", "0") << "\n";
    // 宽屏与采样速率匹配选项放在 General 尾部，保持标准章节结构。
    ofs << "WidescreenStoryboard: "
        << get_prop("General::WidescreenStoryboard", "0") << "\n";
    ofs << "SamplesMatchPlaybackRate: "
        << get_prop("General::SamplesMatchPlaybackRate", "0") << "\n";
    ofs << "\n";

    // Editor 字段只影响 osu! 编辑体验，全部从来源属性或默认值恢复。
    ofs << "[Editor]\n";
    if ( !get_prop("Editor::Bookmarks", "").empty() ) {
        // 空书签字段省略整行，避免写出无意义的尾随键。
        ofs << "Bookmarks: " << get_prop("Editor::Bookmarks", "") << "\n";
    }
    // 网格间距、分拍和缩放仅影响 osu! 编辑器视图。
    ofs << "DistanceSpacing: " << get_prop("Editor::DistanceSpacing", "0.0")
        << "\n";
    ofs << "BeatDivisor: " << get_prop("Editor::BeatDivisor", "4") << "\n";
    ofs << "GridSize: " << get_prop("Editor::GridSize", "16") << "\n";
    ofs << "TimelineZoom: " << get_prop("Editor::TimelineZoom", "1") << "\n";
    ofs << "\n";

    // 通用展示元数据优先；为空时才回退来源 OSU 属性，保留转换兼容性。
    ofs << "[Metadata]\n";
    // Unicode 备用标题与普通标题分别保存，不能互相覆盖。
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
    // 艺术家字段采用与标题相同的通用值优先规则。
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
    // Creator 和 Version 对应通用作者与难度版本。
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
    // 在线来源、标签和 ID 没有通用模型字段，只能从 OSU 元数据保留。
    ofs << "Source:" << get_prop("Metadata::Source", "") << "\n";
    ofs << "Tags:" << get_prop("Metadata::Tags", "") << "\n";
    ofs << "BeatmapID:" << get_prop("Metadata::BeatmapID", "0") << "\n";
    ofs << "BeatmapSetID:" << get_prop("Metadata::BeatmapSetID", "-1") << "\n";
    ofs << "\n";

    // CircleSize 由当前玩家轨道数生成，其余难度字段沿用来源或默认值。
    ofs << "[Difficulty]\n";
    // 键数由当前谱面生成，其他难度数值保留原文本精度。
    ofs << "HPDrainRate:" << get_prop("Difficulty::HPDrainRate", "5") << "\n";
    ofs << "CircleSize:" << beatMap.m_baseMapMetadata.track_count << "\n";
    ofs << "OverallDifficulty:"
        << get_prop("Difficulty::OverallDifficulty", "8") << "\n";
    ofs << "ApproachRate:" << get_prop("Difficulty::ApproachRate", "5") << "\n";
    // Slider 参数虽不参与 mania 物件几何，仍保留以支持来源往返。
    ofs << "SliderMultiplier:"
        << get_prop("Difficulty::SliderMultiplier", "1.4") << "\n";
    ofs << "SliderTickRate:" << get_prop("Difficulty::SliderTickRate", "1")
        << "\n";
    ofs << "\n";

    // Events 当前生成单个背景或视频事件，并原样附加已保留的 Break 行。
    ofs << "[Events]\n";
    ofs << "//Background and Video events\n";
    if ( beatMap.m_baseMapMetadata.cover_type == CoverType::IMAGE ) {
        // 图片事件包含 X/Y 偏移，并将 filesystem::path 转成 UTF-8 文本。
        ofs << "0,0,\""
            << Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path)
            << "\"," << beatMap.m_baseMapMetadata.bgxoffset << ","
            << beatMap.m_baseMapMetadata.bgyoffset << "\n";
    } else {
        // 视频事件保存开始毫秒和路径，不写图片偏移字段。
        ofs << "Video," << beatMap.m_baseMapMetadata.video_starttime << ",\""
            << Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path)
            << "\"\n";
    }

    ofs << "//Break Periods\n";
    if ( auto it = osumeta_props.find("Events::breaks");
         it != osumeta_props.end() ) {
        // Break 已按多行文本保留，仅在缺少末尾换行时补齐章节边界。
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

    // Timing 保持领域顺序写出；不具备 osu! 表示的效果在此明确过滤。
    ofs << "[TimingPoints]\n";
    for ( auto& timing_const : beatMap.m_timings ) {
        // 创建值副本调用历史非 const 转换接口，不修改输入谱面的元数据。
        Timing timing = timing_const;

        if ( timing.m_timingEffect == TimingEffect::JUMP ||
             timing.m_timingEffect == TimingEffect::HS ) {
            // JUMP 和 HS 没有当前支持的 osu! TimingPoint 等价表示。
            continue;
        }

        if ( timing.m_timingEffect == TimingEffect::SCROLL ) {
            if ( timing.m_timingEffectParameter <= 0.0 ) {
                // 非正倍率无法转换为有限负 beatLength，跳过损坏事件。
                continue;
            }
        }

        // Timing 自身负责红线、绿线字段和来源音效元数据的组合。
        ofs << timing.to_osu_description() << "\n";
    }
    ofs << "\n";

    // 先建立全部 Polyline 子节点地址集合，避免统一视图中的子项重复输出。
    ofs << "[HitObjects]\n";
    std::unordered_set<const Note*> polyline_subnotes;
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // 地址只在本次 const 遍历中使用，beatMap 容器不会发生扩容。
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            polyline_subnotes.insert(&subNoteRef.get());
        }
    }

    // 临时引用列表描述实际可写 HitObject，不取得领域对象所有权。
    std::vector<std::reference_wrapper<Note>> export_notes;
    export_notes.reserve(beatMap.m_allNotes.size());
    for ( const auto& noteRef : beatMap.m_allNotes ) {
        Note& note = noteRef.get();
        // 已在父折线中处理的子节点不能作为独立顶层物件再次写出。
        if ( polyline_subnotes.contains(&note) ) continue;

        if ( note.m_type == NoteType::POLYLINE ) {
            // 根折线本身无 osu!mania 表示，只展开其中可表达的 Hold 子节点。
            Polyline& polyline = static_cast<Polyline&>(note);
            for ( const auto& subNoteRef : polyline.m_subNotes ) {
                Note& subNote = subNoteRef.get();
                if ( subNote.m_type == NoteType::HOLD ) {
                    // 非 Hold 子节点的绑定已预检；其几何当前按降级规则省略。
                    export_notes.push_back(subNote);
                }
            }
            continue;
        }

        // 顶层 Note、Hold 与 Flick 均可经 Note/Hold 转换器写出。
        export_notes.push_back(note);
    }

    // 确定排序不依赖分类容器顺序，重复同键对象继续保持来源相对次序。
    std::stable_sort(export_notes.begin(),
                     export_notes.end(),
                     [](const std::reference_wrapper<Note>& a_ref,
                        const std::reference_wrapper<Note>& b_ref) {
                         const Note& a = a_ref.get();
                         const Note& b = b_ref.get();
                         // 容差内时间视为同组，再使用轨道与类型次键。
                         if ( std::abs(a.m_timestamp - b.m_timestamp) > 1e-4 )
                             return a.m_timestamp < b.m_timestamp;
                         if ( a.m_track != b.m_track )
                             return a.m_track < b.m_track;
                         return a.m_type < b.m_type;
                     });

    for ( const auto& noteRef : export_notes ) {
        Note& note = noteRef.get();
        if ( note.m_type == NoteType::HOLD ) {
            // 只有真实 Hold 才访问持续时间派生字段。
            Hold& hold = static_cast<Hold&>(note);
            ofs << hold.to_osu_description(
                       beatMap.m_baseMapMetadata.track_count)
                << "\n";
        } else {
            // 普通 Note 与降级 Flick 共用瞬时 HitObject 写出格式。
            ofs << note.to_osu_description(
                       beatMap.m_baseMapMetadata.track_count)
                << "\n";
        }
    }

    // 流在函数退出时关闭；到达此处表示所有预检与字段写入路径完成。
    XINFO("Successfully saved osu map to {}", Config::pathToUtf8(path));
    // 调用方可根据 true 继续登记输出资源，false 分支均已在此前返回。
    // 文件内容的重新加载一致性由独立 OSUConsistencyTest 覆盖。
    return true;
}

}  // namespace MMM
