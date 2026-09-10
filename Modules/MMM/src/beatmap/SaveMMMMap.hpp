#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

/// @file SaveMMMMap.hpp
/// @brief 原生 MMM v3 JSON 的规范序列化实现。
///
/// 顶层结构固定为：
/// - `format_version`：当前写出版本，固定为 3；
/// - `metadata`：基础字段与来源格式扩展字段；
/// - `timing`：统一 Timing 事件数组；
/// - `audio_samples`：与玩家物件独立的自动采样数组；
/// - `note`：顶层玩家物件及其折线子节点；
/// - `annotations`：可独立定位时间或对象的批注数组。
///
/// metadata.base 保存：
/// - 展示名称、标题、艺术家、难度版本和作者；
/// - 主音频提示、封面及背景资源相对路径；
/// - 封面类型、视频起始时间与背景偏移；
/// - 玩家轨道数、BGM 轨道数、偏好 BPM 和谱面时长。
///
/// metadata.extra 按来源命名空间保存字符串属性：
/// - `osu` 对应 osu! 导入时无法进入公共模型的字段；
/// - `malody` 对应 Malody 私有字段；
/// - `rm` 对应 RM/IMD 私有字段；
/// - 未知枚举值不写出，避免生成加载器无法解释的命名空间。
///
/// Timing 条目同时写出时间戳、BPM、拍长、效果名称与效果参数。每项自己的
/// `extra` 数组只保存 osu! 和 Malody 来源属性，使跨格式导入后仍可回写。
///
/// audio_samples 与物件绑定采样职责不同：
/// - 自动采样位于顶层数组，拥有时间、资源偏移、轨道和协作身份；
/// - 玩家绑定采样位于 note.sample，只拥有资源标识与音量；
/// - 两者不能因引用相同音频而合并；
/// - 自动采样按时间、轨道、资源和偏移建立稳定输出次序。
///
/// 玩家物件的基础表示为时间戳、轨道和类型。Hold 增加 duration，Flick 增加
/// dtrack，Polyline 增加 sub_notes。协作身份、简短物件批注和采样绑定均与
/// 基础类型正交，任何类型都可以携带这些字段。
///
/// Polyline 存在两种写出策略：
/// - 折线或节点被批注引用时，完整保存原始节点结构和协作身份；
/// - 没有结构性引用时，清理零长度 Hold 和零位移 Flick，并合并相邻同类段；
/// - 清洗后没有段时退化为 Note；
/// - 清洗后只有一段时退化为对应 Hold 或 Flick；
/// - 两段及以上继续写为 Polyline。
///
/// 保留批注结构是必要的数据契约。若把带目标 ID 的折线降级为单物件，独立
/// annotations 中的 target_id 将无法解析；若合并被引用节点，也会改变批注
/// 所指向的具体对象。因此结构清洗只允许作用于未被批注依赖的折线。
///
/// 原生格式写出维持以下不变量：
/// - 所有资源路径在 JSON 边界转换为 UTF-8；
/// - 来源扩展字段只接受已知命名空间；
/// - 自动采样和批注使用确定的稳定顺序；
/// - 折线子节点不会再作为顶层物件重复写出；
/// - 顶层物件最终按时间戳排序；
/// - 玩家绑定使用 v3 的 `sample` 对象，不生成旧版平铺键；
/// - 自动采样使用 v3 的 `audio_samples`，不混入玩家 note 数组；
/// - 输出文件打开失败时返回 false，不报告虚假的保存成功。
///
/// v3 字段归属速查：
/// - `metadata.base.name`：编辑器展示名；
/// - `metadata.base.title`：标题；
/// - `metadata.base.title_unicode`：Unicode 标题；
/// - `metadata.base.artist`：艺术家；
/// - `metadata.base.artist_unicode`：Unicode 艺术家；
/// - `metadata.base.version`：难度版本；
/// - `metadata.base.author`：谱师；
/// - `metadata.base.song_file_hint`：主音频资源提示；
/// - `metadata.base.cover`：主封面或背景资源；
/// - `metadata.base.cover_img`：静态封面资源；
/// - `metadata.base.cover_type`：图片或视频类型枚举；
/// - `metadata.base.video_starttime`：视频起始偏移；
/// - `metadata.base.bgxoffset`：背景横向偏移；
/// - `metadata.base.bgyoffset`：背景纵向偏移；
/// - `metadata.base.track_count`：玩家轨道数量；
/// - `metadata.base.bgm_track_count`：自动音频轨道数量；
/// - `metadata.base.bpm`：偏好 BPM；
/// - `metadata.base.duration`：谱面声明时长；
/// - `timing[].timestamp`：效果发生时间；
/// - `timing[].bpm`：该点参考 BPM；
/// - `timing[].beat_length`：每拍毫秒数；
/// - `timing[].effect`：效果稳定名称；
/// - `timing[].param`：效果参数；
/// - `audio_samples[].collaboration_id`：自动采样稳定身份；
/// - `audio_samples[].timestamp`：时间线触发位置；
/// - `audio_samples[].offset_ms`：资源内起播偏移；
/// - `audio_samples[].track`：绝对 BGM 轨道；
/// - `audio_samples[].audio_ref`：音频资源标识；
/// - `audio_samples[].volume`：线性音量；
/// - `note[].collaboration_id`：玩家物件稳定身份；
/// - `note[].annotation`：物件内联短批注；
/// - `note[].sample`：玩家命中采样绑定；
/// - `note[].timestamp`：物件起始位置；
/// - `note[].track`：玩家轨道；
/// - `note[].type`：note、hold、flick 或 polyline；
/// - `note[].duration`：Hold 持续时间；
/// - `note[].dtrack`：Flick 轨道位移；
/// - `note[].sub_notes`：Polyline 节点数组；
/// - `annotations[].id`：批注稳定身份；
/// - `annotations[].target_kind`：目标类别；
/// - `annotations[].target_id`：对象目标身份；
/// - `annotations[].timestamp`：目标失效时的回退位置；
/// - `annotations[].author`：批注作者；
/// - `annotations[].content`：批注正文。
///
/// 可选字段省略规则：
/// - 空 collaboration_id 不写出；
/// - 空物件内联 annotation 不写出；
/// - 不存在的玩家 sample 不写出；
/// - 未知来源 Metadata 命名空间不写出；
/// - 已知来源即使属性为空，也保持当前数组对象结构；
/// - 顶层数组始终存在，即使其中没有条目。
///
/// 排序规则只服务确定性写出，不改变逻辑时间：
/// - Timing 沿用 BeatMap 已同步次序；
/// - 自动采样按复合身份稳定排序；
/// - 顶层玩家物件按 timestamp 排序；
/// - Polyline 节点保持 m_subNotes 原始次序；
/// - 批注按 timestamp、id 稳定排序；
/// - 来源扩展属性沿用关联容器迭代次序。
///
/// @brief 保存谱面为原生 MMM v3 JSON。
/// @param beatMap 已同步的谱面对象。
/// @param path 保存路径。
/// @return 文件成功打开且 JSON 完整写出时返回 true。
inline bool saveMMMMap(const BeatMap&               beatMap,
                       const std::filesystem::path& path)
{
    // 保存器只产生当前规范版本；旧版本兼容性由加载器单向承担。
    json root;
    root["format_version"] = 3;

    // 第一阶段构造元数据，先写稳定公共字段，再附加来源命名空间。
    auto& metadata = root["metadata"];

    // 路径字段以 UTF-8 字符串落盘，避免平台原生 path 编码进入 JSON。
    auto& base             = metadata["base"];
    base["name"]           = beatMap.m_baseMapMetadata.name;
    base["title"]          = beatMap.m_baseMapMetadata.title;
    base["title_unicode"]  = beatMap.m_baseMapMetadata.title_unicode;
    base["artist"]         = beatMap.m_baseMapMetadata.artist;
    base["artist_unicode"] = beatMap.m_baseMapMetadata.artist_unicode;
    base["version"]        = beatMap.m_baseMapMetadata.version;
    base["author"]         = beatMap.m_baseMapMetadata.author;
    base["song_file_hint"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.song_file_hint);
    base["cover"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path);
    base["cover_img"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.cover_path);
    base["cover_type"] = static_cast<int>(beatMap.m_baseMapMetadata.cover_type);
    base["video_starttime"] = beatMap.m_baseMapMetadata.video_starttime;
    base["bgxoffset"]       = beatMap.m_baseMapMetadata.bgxoffset;
    base["bgyoffset"]       = beatMap.m_baseMapMetadata.bgyoffset;
    base["track_count"]     = beatMap.m_baseMapMetadata.track_count;
    base["bgm_track_count"] = beatMap.m_baseMapMetadata.bgm_track_count;
    base["bpm"]             = beatMap.m_baseMapMetadata.preference_bpm;
    base["duration"]        = beatMap.m_baseMapMetadata.map_length;

    // extra 使用单键对象数组，键名表示来源，值为该来源的字符串属性对象。
    auto& extra = metadata["extra"];
    extra       = json::array();
    for ( const auto& [type, props] : beatMap.m_metadata.map_properties ) {
        json        sourceObj;
        std::string sourceName;
        // 枚举到磁盘名称的映射在此集中定义，未知来源不会写出空键。
        if ( type == MapMetadataType::OSU )
            sourceName = "osu";
        else if ( type == MapMetadataType::MALODY )
            sourceName = "malody";
        else if ( type == MapMetadataType::RM )
            sourceName = "rm";

        if ( sourceName.empty() ) continue;

        // 属性值保持字符串形式，具体来源格式负责在回写时解释其语义。
        json propObj;
        for ( const auto& [key, val] : props ) {
            propObj[key] = val;
        }
        sourceObj[sourceName] = propObj;
        extra.push_back(sourceObj);
    }

    // 第二阶段按模型次序写 Timing；BeatMap::sync 负责其规范排序。
    auto& timingArr = root["timing"];
    timingArr       = json::array();
    for ( const auto& timing : beatMap.m_timings ) {
        json t;
        t["timestamp"]   = timing.m_timestamp;
        t["bpm"]         = timing.m_bpm;
        t["beat_length"] = timing.m_beat_length;
        // 枚举以稳定名称写出，避免把内部数值布局固化为文件格式 ABI。
        t["effect"] = timingEffectToString(timing.m_timingEffect);
        t["param"]  = timing.m_timingEffectParameter;

        auto& tExtra = t["extra"];
        tExtra       = json::array();
        // Timing 扩展不接受 RM 命名空间，因为 IMD 没有逐点私有属性映射。
        for ( const auto& [type, props] :
              timing.m_metadata.timing_properties ) {
            json        sourceObj;
            std::string sourceName;
            if ( type == TimingMetadataType::OSU )
                sourceName = "osu";
            else if ( type == TimingMetadataType::MALODY )
                sourceName = "malody";

            if ( sourceName.empty() ) continue;

            json propObj;
            for ( const auto& [key, val] : props ) {
                propObj[key] = val;
            }
            sourceObj[sourceName] = propObj;
            tExtra.push_back(sourceObj);
        }
        timingArr.push_back(t);
    }

    // 第三阶段写自动采样。先建立观察指针视图，排序不改变 BeatMap 原数组。
    auto& sampleArr = root["audio_samples"];
    sampleArr       = json::array();
    std::vector<const AudioSampleEvent*> sortedSamples;
    sortedSamples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        sortedSamples.push_back(&sample);
    }
    // 时间近似相同时依次使用轨道、资源和偏移决胜，保证结果可重复。
    std::stable_sort(
        sortedSamples.begin(),
        sortedSamples.end(),
        [](const AudioSampleEvent* lhs, const AudioSampleEvent* rhs) {
            if ( std::abs(lhs->m_timestamp - rhs->m_timestamp) > 1e-6 ) {
                return lhs->m_timestamp < rhs->m_timestamp;
            }
            if ( lhs->m_track != rhs->m_track ) {
                return lhs->m_track < rhs->m_track;
            }
            if ( lhs->m_audioResourceId != rhs->m_audioResourceId ) {
                return lhs->m_audioResourceId < rhs->m_audioResourceId;
            }
            return lhs->m_offsetMs < rhs->m_offsetMs;
        });
    for ( const AudioSampleEvent* sample : sortedSamples ) {
        // collaboration_id 为空时省略，其他播放字段始终显式写出。
        json sampleJson;
        if ( !sample->m_collaborationId.empty() ) {
            sampleJson["collaboration_id"] = sample->m_collaborationId;
        }
        sampleJson["timestamp"] = sample->m_timestamp;
        sampleJson["offset_ms"] = sample->m_offsetMs;
        sampleJson["track"]     = sample->m_track;
        sampleJson["audio_ref"] = sample->m_audioResourceId;
        sampleJson["volume"]    = sample->m_volume;

        // 自动采样扩展目前支持 Malody 来源和 MMM 编辑器自有属性。
        auto& sampleExtra = sampleJson["extra"];
        sampleExtra       = json::array();
        for ( const auto& [type, props] :
              sample->m_metadata.sample_properties ) {
            std::string sourceName;
            if ( type == SampleMetadataType::MALODY )
                sourceName = "malody";
            else if ( type == SampleMetadataType::MMM )
                sourceName = "mmm";

            // 未知命名空间不能产生无法识别的空来源对象。
            if ( sourceName.empty() ) continue;

            json propertyObject;
            for ( const auto& [key, value] : props ) {
                propertyObject[key] = value;
            }
            json sourceObject;
            sourceObject[sourceName] = propertyObject;
            sampleExtra.push_back(sourceObject);
        }
        sampleArr.push_back(sampleJson);
    }

    // 第四阶段写玩家物件。note 数组只接收顶层对象，折线节点内嵌到父项。
    auto& noteArr = root["note"];
    noteArr       = json::array();

    // 先收集所有对象型批注目标，决定哪些折线结构禁止清洗或折叠。
    std::unordered_set<std::string> annotatedObjectIds;
    // 集合同时包含玩家物件和自动采样目标 ID；只有折线及其节点会查询它，
    // 因而自动采样 ID 不会错误影响无关物件，除非输入本身违反 ID 唯一约束。
    for ( const auto& annotation : beatMap.m_annotations ) {
        // 时间点批注不依赖对象身份；空 target_id 也不能保护任何结构。
        if ( annotation.m_targetKind !=
                 BeatmapAnnotationTargetKind::TIMESTAMP &&
             !annotation.m_targetId.empty() ) {
            annotatedObjectIds.insert(annotation.m_targetId);
        }
    }

    /// @brief 将一个顶层玩家物件序列化为规范 JSON。
    /// @param note Note、Hold、Flick 或 Polyline 的多态引用。
    /// @return 完整物件对象；Polyline 可能按结构引用情况执行安全清洗。
    auto serializeNote = [&](const Note& note) {
        json n;
        n["timestamp"] = note.m_timestamp;
        n["track"]     = note.m_track;
        // 协作身份和内联批注都是可选字段，空值不写出以保持文件紧凑。
        if ( !note.m_collaborationId.empty() ) {
            n["collaboration_id"] = note.m_collaborationId;
        }
        if ( !note.m_annotation.empty() ) {
            n["annotation"] = note.m_annotation;
        }
        // v3 将资源和音量组合为 sample 对象，杜绝旧版两个平铺键不同步。
        if ( const auto binding = note.getSampleBinding() ) {
            n["sample"] = { { "audio_ref", binding->m_audioResourceId },
                            { "volume", binding->m_volume } };
        }

        // 只有派生字段在类型分支中写出，共同字段已经在分支前统一处理。
        switch ( note.m_type ) {
        case NoteType::NOTE: n["type"] = "note"; break;
        case NoteType::HOLD: {
            n["type"]     = "hold";
            n["duration"] = static_cast<const Hold&>(note).m_duration;
            break;
        }
        case NoteType::FLICK: {
            n["type"]   = "flick";
            n["dtrack"] = static_cast<const Flick&>(note).m_dtrack;
            break;
        }
        case NoteType::POLYLINE: {
            const auto& poly = static_cast<const Polyline&>(note);

            // 折线自身、任一节点存在内联批注，或独立批注引用其 ID 时，必须
            // 保存原始节点结构，不能让几何清洗破坏批注目标身份。
            const bool preserveAnnotatedStructure =
                !poly.m_annotation.empty() ||
                annotatedObjectIds.contains(poly.m_collaborationId) ||
                std::any_of(poly.m_subNotes.begin(),
                            poly.m_subNotes.end(),
                            [&](const auto& subNote) {
                                return !subNote.get().m_annotation.empty() ||
                                       annotatedObjectIds.contains(
                                           subNote.get().m_collaborationId);
                            });
            if ( preserveAnnotatedStructure ) {
                // 保留路径逐节点写出类型、派生字段、身份、采样和内联批注。
                // 该路径不执行零段过滤或同类合并，优先保证所有 target_id
                // 可解析。
                n["type"]         = "polyline";
                json subNotesJson = json::array();
                for ( const auto& subNoteRef : poly.m_subNotes ) {
                    const Note& subNote = subNoteRef.get();
                    json        subJson;
                    subJson["timestamp"] = subNote.m_timestamp;
                    subJson["track"]     = subNote.m_track;
                    if ( !subNote.m_collaborationId.empty() ) {
                        subJson["collaboration_id"] = subNote.m_collaborationId;
                    }
                    // 子节点只允许基础可绘制类型，不递归嵌套 Polyline。
                    if ( subNote.m_type == NoteType::HOLD ) {
                        subJson["type"] = "hold";
                        subJson["duration"] =
                            static_cast<const Hold&>(subNote).m_duration;
                    } else if ( subNote.m_type == NoteType::FLICK ) {
                        subJson["type"] = "flick";
                        subJson["dtrack"] =
                            static_cast<const Flick&>(subNote).m_dtrack;
                    } else {
                        subJson["type"] = "note";
                    }
                    // 节点自己的绑定优先保留，不能提升到父折线后丢失作用范围。
                    if ( const auto binding = subNote.getSampleBinding() ) {
                        subJson["sample"] = { { "audio_ref",
                                                binding->m_audioResourceId },
                                              { "volume", binding->m_volume } };
                    }
                    if ( !subNote.m_annotation.empty() ) {
                        subJson["annotation"] = subNote.m_annotation;
                    }
                    subNotesJson.push_back(std::move(subJson));
                }
                n["sub_notes"] = std::move(subNotesJson);
                break;
            }

            // 无批注依赖时进入规范化路径，先把节点投影为可合并的轻量段。
            struct CleanSeg {
                /// @brief 段的基础类型，只使用 Hold 或 Flick。
                NoteType type;
                /// @brief 段的起始时间戳。
                double timestamp;
                /// @brief Hold 持续时间，Flick 时为零。
                double duration;
                /// @brief 段起始轨道。
                int track;
                /// @brief Flick 位移，Hold 时为零。
                int dtrack;
                /// @brief 清洗过程中随子物件保留的命中采样绑定。
                std::optional<AudioSampleBinding> sampleBinding;
            };
            std::vector<CleanSeg> cleanSubs;
            // 普通 Note 节点不形成持续或滑动段，清洗模型只收集 Hold 与 Flick。
            for ( const auto& subNoteRef : poly.m_subNotes ) {
                const Note& sn = subNoteRef.get();
                if ( sn.m_type == NoteType::HOLD ) {
                    double dur = static_cast<const Hold&>(sn).m_duration;
                    // 亚阈值 Hold 不产生可见长度，忽略后再判断是否可降级。
                    if ( dur < 1e-4 ) continue;
                    cleanSubs.push_back({ NoteType::HOLD,
                                          sn.m_timestamp,
                                          dur,
                                          (int)sn.m_track,
                                          0,
                                          sn.getSampleBinding() });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    cleanSubs.push_back(
                        { NoteType::FLICK,
                          sn.m_timestamp,
                          0.0,
                          (int)sn.m_track,
                          static_cast<const Flick&>(sn).m_dtrack,
                          sn.getSampleBinding() });
                }
            }

            // 合并可能产生新的零段或相邻同类段，因此迭代到固定点。
            bool changed = true;
            // changed
            // 表示上一轮是否删除或合并过元素；一整轮无变化即到达固定点。
            while ( changed ) {
                changed = false;

                // 首先移除零长度 Hold 和零位移 Flick，防止无效段参与合并。
                auto it = std::remove_if(
                    cleanSubs.begin(), cleanSubs.end(), [](const auto& s) {
                        if ( s.type == NoteType::HOLD )
                            return s.duration < 1e-4;
                        if ( s.type == NoteType::FLICK ) return s.dtrack == 0;
                        return false;
                    });
                if ( it != cleanSubs.end() ) {
                    // erase 后数组相邻关系变化，必须再运行一轮合并与零值检查。
                    cleanSubs.erase(it, cleanSubs.end());
                    changed = true;
                }

                // 然后合并相邻同类段；删除元素后保持同一索引继续检查链式合并。
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == next.type ) {
                            if ( curr.type == NoteType::HOLD ) {
                                // 连续 Hold
                                // 的时长相加；首个缺少绑定时继承后段绑定。
                                curr.duration += next.duration;
                                if ( !curr.sampleBinding ) {
                                    curr.sampleBinding = next.sampleBinding;
                                }
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                // 不递增 i，使扩展后的 curr 继续尝试吸收下一个
                                // Hold。
                                changed = true;
                                continue;
                            } else if ( curr.type == NoteType::FLICK ) {
                                // 连续 Flick
                                // 的位移相加，反向段可能在下一轮归零删除。
                                curr.dtrack += next.dtrack;
                                if ( !curr.sampleBinding ) {
                                    curr.sampleBinding = next.sampleBinding;
                                }
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                // 位移相加可能为零，下一轮过滤阶段会移除抵消结果。
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }

            // 根据固定点结果选择最小但语义等价的原生物件表示。
            if ( cleanSubs.empty() ) {
                // 没有有效段时保留父物件的点击位置并降级为普通 Note。
                n["type"] = "note";
            } else if ( cleanSubs.size() == 1 ) {
                // 单段无需 Polyline 容器，时间与轨道改用该有效段的起点。
                const auto& s = cleanSubs[0];
                if ( s.type == NoteType::HOLD ) {
                    n["type"]     = "hold";
                    n["duration"] = s.duration;
                } else if ( s.type == NoteType::FLICK ) {
                    n["type"]   = "flick";
                    n["dtrack"] = s.dtrack;
                } else {
                    n["type"] = "note";
                }
                n["timestamp"] = s.timestamp;
                n["track"]     = s.track;
                // 父对象没有绑定时才提升唯一子段绑定，避免覆盖更明确的父绑定。
                if ( !note.getSampleBinding() && s.sampleBinding ) {
                    n["sample"] = { { "audio_ref",
                                      s.sampleBinding->m_audioResourceId },
                                    { "volume", s.sampleBinding->m_volume } };
                }
            } else {
                // 多段继续以内嵌节点数组表示，并逐段保留采样绑定。
                n["type"]         = "polyline";
                json subNotesJson = json::array();
                for ( const auto& s : cleanSubs ) {
                    json snj;
                    snj["timestamp"] = s.timestamp;
                    snj["track"]     = s.track;
                    if ( s.type == NoteType::HOLD ) {
                        snj["type"]     = "hold";
                        snj["duration"] = s.duration;
                    } else if ( s.type == NoteType::FLICK ) {
                        snj["type"]   = "flick";
                        snj["dtrack"] = s.dtrack;
                    }
                    if ( s.sampleBinding ) {
                        snj["sample"] = {
                            { "audio_ref", s.sampleBinding->m_audioResourceId },
                            { "volume", s.sampleBinding->m_volume }
                        };
                    }
                    subNotesJson.push_back(snj);
                }
                n["sub_notes"] = subNotesJson;
            }
            break;
        }
        }

        // 类型规范化完成后写来源扩展，使扩展始终挂在最终顶层物件上。
        auto& nExtra = n["extra"];
        nExtra       = json::array();
        for ( const auto& [type, props] : note.m_metadata.note_properties ) {
            json        sourceObj;
            std::string sourceName;
            // 已知来源包含 osu!、Malody 与 MMM 编辑器自有命名空间。
            if ( type == NoteMetadataType::OSU )
                sourceName = "osu";
            else if ( type == NoteMetadataType::MALODY )
                sourceName = "malody";
            else if ( type == NoteMetadataType::MMM )
                sourceName = "mmm";

            if ( sourceName.empty() ) continue;

            json propObj;
            for ( const auto& [key, val] : props ) {
                propObj[key] = val;
            }
            sourceObj[sourceName] = propObj;
            nExtra.push_back(sourceObj);
        }
        return n;
    };

    // 子节点也存在于各类型拥有容器，用地址集合排除其顶层重复写出。
    std::set<const Note*> subNotesSet;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        // 折线引用只做身份收集，不读取或复制节点内容。
        for ( const auto& subNoteRef : poly.m_subNotes ) {
            subNotesSet.insert(&subNoteRef.get());
        }
    }

    // 先按拥有容器收集所有顶层对象，再统一排序，避免类型容器决定磁盘顺序。
    std::vector<json> serializedNotes;
    for ( const auto& note : beatMap.m_noteData.notes ) {
        // 各基础类型容器只输出不属于任何折线的对象。
        if ( subNotesSet.find(&note) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(note));
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        if ( subNotesSet.find(&hold) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(hold));
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        if ( subNotesSet.find(&flick) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(flick));
    }
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        // Polyline 容器本身始终作为顶层对象输出一次。
        serializedNotes.push_back(serializeNote(poly));
    }

    // 顶层按时间戳排序，保证相同模型在不同构造顺序下产生稳定文件。
    std::sort(serializedNotes.begin(),
              serializedNotes.end(),
              [](const json& a, const json& b) {
                  return a["timestamp"].get<double>() <
                         b["timestamp"].get<double>();
              });

    // 排序后的对象依次移动到最终数组；这里保留现有复制语义以维持简单边界。
    for ( auto& n : serializedNotes ) {
        noteArr.push_back(n);
    }

    // 第五阶段独立写批注，按回退时间和稳定 ID 排序。
    auto& annotationArray = root["annotations"];
    annotationArray       = json::array();
    std::vector<const BeatmapAnnotation*> sortedAnnotations;
    sortedAnnotations.reserve(beatMap.m_annotations.size());
    for ( const auto& annotation : beatMap.m_annotations ) {
        sortedAnnotations.push_back(&annotation);
    }
    // 时间相同的批注以 ID 决胜，稳定排序保留完全同键项的原次序。
    std::stable_sort(
        sortedAnnotations.begin(),
        sortedAnnotations.end(),
        [](const BeatmapAnnotation* lhs, const BeatmapAnnotation* rhs) {
            if ( std::abs(lhs->m_timestamp - rhs->m_timestamp) > 1e-6 ) {
                return lhs->m_timestamp < rhs->m_timestamp;
            }
            return lhs->m_id < rhs->m_id;
        });
    for ( const BeatmapAnnotation* annotation : sortedAnnotations ) {
        // 内部枚举映射为稳定字符串，未知值安全回退为时间点目标。
        std::string targetKind = "timestamp";
        if ( annotation->m_targetKind ==
             BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
            targetKind = "player_object";
        } else if ( annotation->m_targetKind ==
                    BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
            targetKind = "audio_sample";
        }
        annotationArray.push_back({ { "id", annotation->m_id },
                                    { "target_kind", targetKind },
                                    { "target_id", annotation->m_targetId },
                                    { "timestamp", annotation->m_timestamp },
                                    { "author", annotation->m_author },
                                    { "content", annotation->m_content } });
    }

    // 完整内存 JSON 构造成功后才打开目标文件，避免中途逻辑失败留下半结构。
    std::ofstream file(path);
    if ( !file.is_open() ) {
        XERROR("Failed to open file for saving mmm map: {}",
               Config::pathToUtf8(path));
        return false;
    }

    // 四空格缩进便于版本控制审阅；JSON 内容语义不依赖空白。
    file << root.dump(4);
    return true;
}

}  // namespace MMM
