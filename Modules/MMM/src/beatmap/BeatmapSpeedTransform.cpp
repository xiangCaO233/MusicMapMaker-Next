#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "mmm/SafeParse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace MMM
{
namespace
{

/**
 * @file BeatmapSpeedTransform.cpp
 * @brief 在不修改来源谱面的前提下生成与变速音频同步的领域副本。
 *
 * 时间量按 `原值 / speed` 缩放，BPM 按 `原值 * speed` 缩放；轨道、类型、
 * 资源身份、音量和来源元数据保持不变。Polyline 的子物件引用不能直接复制，
 * 必须先把各子物件复制进目标拥有型容器，再重建父折线引用。自动采样偏移使用
 * 整数毫秒，因此单独执行确定的四舍五入和 int64 饱和处理。
 */

/// @brief 计算倍速后的毫秒时间。
/// @param value 原毫秒时间。
/// @param speed 倍速倍率。
/// @return 缩放后的毫秒时间。
double scaledMilliseconds(double value, double speed)
{
    // speed 已由公共入口验证为正有限值，内部帮助函数无需重复分支。
    return value / speed;
}

/// @brief 计算倍速后的整数毫秒偏移。
/// @param value 原整数毫秒偏移。
/// @param speed 倍速倍率。
/// @return 按最接近整数、半值远离零舍入并限制在 int64 范围内的结果。
std::int64_t scaledOffsetMilliseconds(std::int64_t value, double speed)
{
    // 使用 long double，避免 int64 极值先转换为 double 时损失过多低位。
    const long double scaled =
        static_cast<long double>(value) / static_cast<long double>(speed);
    // std::round 对正负半值都远离零，与资源时间偏移的测试契约一致。
    const long double rounded = std::round(scaled);
    const long double minimum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if ( rounded <= minimum ) {
        // 饱和到整数下界，不能让超范围浮点转整数进入未定义行为。
        return std::numeric_limits<std::int64_t>::min();
    }
    if ( rounded >= maximum ) {
        // 正向极值同样饱和，保持偏移方向与可序列化性。
        return std::numeric_limits<std::int64_t>::max();
    }
    // 范围预检后转换安全，结果保持整数毫秒存储契约。
    return static_cast<std::int64_t>(rounded);
}

/// @brief 缩放单个普通物件时间。
/// @param note 需要修改的物件。
/// @param speed 倍速倍率。
void scaleNoteTime(Note& note, double speed)
{
    // 只修改公共锚点时间，轨道、采样绑定和稳定身份全部保留。
    note.m_timestamp = scaledMilliseconds(note.m_timestamp, speed);
}

/// @brief 缩放单个长条物件时间。
/// @param hold 需要修改的长条。
/// @param speed 倍速倍率。
void scaleHoldTime(Hold& hold, double speed)
{
    // 起点和持续时间使用同一倍率，终点自然按相同比例移动。
    scaleNoteTime(hold, speed);
    hold.m_duration = scaledMilliseconds(hold.m_duration, speed);
}

/// @brief 同步缩放 Malody time 节点保留的局部 delay。
/// @param timing 需要修改的时间线事件。
/// @param speed 倍速倍率。
void scaleMalodyTimingDelay(Timing& timing, double speed)
{
    // delay 是 Malody 私有扩展；其他来源的 Timing 无需创建该属性。
    auto source =
        timing.m_metadata.timing_properties.find(TimingMetadataType::MALODY);
    if ( source == timing.m_metadata.timing_properties.end() ) return;

    // 缺失 delay 表示该时间点没有额外局部偏移，保持原元数据不变。
    auto delay = source->second.find("delay");
    if ( delay == source->second.end() ) return;

    // 属性表保存 JSON 文本，使用非抛出解析避免损坏扩展中断整个变换。
    const auto parsed = nlohmann::json::parse(
        delay->second.begin(), delay->second.end(), nullptr, false);
    if ( parsed.is_discarded() ) return;

    // 数字和历史字符串两种表示都接受，其他 JSON 类型原样保留。
    double delayMs = std::numeric_limits<double>::quiet_NaN();
    if ( parsed.is_number() ) {
        delayMs = parsed.get<double>();
    } else if ( parsed.is_string() ) {
        // safeStod 延续格式导入对数值前缀的兼容解析语义。
        delayMs =
            Internal::safeStod(parsed.get_ref<const std::string&>(), delayMs);
    }
    // 无法确定方向的值不参与缩放，避免写回 NaN 或无穷 JSON。
    if ( !std::isfinite(delayMs) ) return;

    // 统一以 JSON 数字写回，完成历史字符串表示的自然规范化。
    delay->second = nlohmann::json(scaledMilliseconds(delayMs, speed)).dump();
}

/// @brief 缩放单个时间线事件。
/// @param timing 需要修改的时间线事件。
/// @param speed 倍速倍率。
void scaleTiming(Timing& timing, double speed)
{
    // 所有事件锚点和 Malody 局部 delay 都属于绝对时间域。
    timing.m_timestamp = scaledMilliseconds(timing.m_timestamp, speed);
    scaleMalodyTimingDelay(timing, speed);
    if ( timing.m_timingEffect != TimingEffect::BPM ) {
        // SCROLL/JUMP/HS 的效果参数不是节拍频率，变速时保持数值不变。
        return;
    }

    // 优先使用领域 BPM，兼容旧数据时依次尝试效果参数和正拍长反推。
    double bpm = timing.m_bpm;
    if ( bpm <= 0.0 ) {
        bpm = timing.m_timingEffectParameter;
    }
    if ( bpm <= 0.0 && timing.m_beat_length > 0.0 ) {
        // 正拍长单位为毫秒每拍，按 60000 毫秒换算每分钟拍数。
        bpm = 60000.0 / timing.m_beat_length;
    }
    if ( bpm <= 0.0 || !std::isfinite(bpm) ) {
        // 无法恢复有效基准时只缩放时间锚点，不制造虚构 BPM。
        return;
    }

    // BPM 与速度同向增长，并同步更新两个兼容字段和反比拍长。
    timing.m_bpm                   = bpm * speed;
    timing.m_timingEffectParameter = timing.m_bpm;
    timing.m_beat_length           = 60000.0 / timing.m_bpm;
}

/// @brief 复制并缩放折线中的子物件。
/// @param target 接收子物件的新谱面。
/// @param targetPolyline 正在重建引用的目标折线。
/// @param sourceSubNote 原折线子物件。
/// @param speed 倍速倍率。
void copyScaledPolylineSubNote(BeatMap& target, Polyline& targetPolyline,
                               const Note& sourceSubNote, double speed)
{
    // 引用的动态类型决定目标拥有型容器，不能把派生字段切片到 Note。
    if ( sourceSubNote.m_type == NoteType::HOLD ) {
        const auto& sourceHold = static_cast<const Hold&>(sourceSubNote);
        // 先复制完整派生值，再标记为子物件并缩放起点与持续时间。
        Hold hold        = sourceHold;
        hold.m_isSubNote = true;
        scaleHoldTime(hold, speed);
        // 容器取得所有权后，父折线只保存对稳定元素的引用包装器。
        target.m_noteData.holds.push_back(std::move(hold));
        auto& copied = target.m_noteData.holds.back();
        targetPolyline.m_subNotes.push_back(copied);
        // 类型专用视图与通用子物件视图必须指向同一目标实例。
        targetPolyline.m_subHolds.push_back(copied);
        return;
    }

    if ( sourceSubNote.m_type == NoteType::FLICK ) {
        const auto& sourceFlick = static_cast<const Flick&>(sourceSubNote);
        // Flick 的相对轨道增量随值复制保留，仅锚点时间参与缩放。
        Flick flick       = sourceFlick;
        flick.m_isSubNote = true;
        scaleNoteTime(flick, speed);
        target.m_noteData.flicks.push_back(std::move(flick));
        auto& copied = target.m_noteData.flicks.back();
        targetPolyline.m_subNotes.push_back(copied);
        // 重建专用 Flick 引用，避免沿用指向来源谱面的旧地址。
        targetPolyline.m_subFlicks.push_back(copied);
        return;
    }

    // 剩余子节点按普通 Note 复制，Polyline 根节点不会出现在子列表中。
    Note note        = sourceSubNote;
    note.m_isSubNote = true;
    scaleNoteTime(note, speed);
    target.m_noteData.notes.push_back(std::move(note));
    auto& copied = target.m_noteData.notes.back();
    // 普通子节点只进入通用列表，没有额外类型专用索引。
    targetPolyline.m_subNotes.push_back(copied);
}

/// @brief 复制并缩放谱面物件。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledNotes(BeatMap& target, const BeatMap& source, double speed)
{
    // 目标最初复制了谱面元数据但不复制对象容器，此处仍显式清空防止复用。
    target.m_noteData.notes.clear();
    target.m_noteData.holds.clear();
    target.m_noteData.flicks.clear();
    target.m_noteData.polylines.clear();

    for ( const auto& sourceNote : source.m_noteData.notes ) {
        // 子物件稍后按父 Polyline 的顺序复制，避免重复并保留父子关系。
        if ( sourceNote.m_isSubNote ) continue;
        Note note = sourceNote;
        scaleNoteTime(note, speed);
        target.m_noteData.notes.push_back(std::move(note));
    }
    for ( const auto& sourceHold : source.m_noteData.holds ) {
        // 顶层 Hold 直接复制完整值并同步缩放锚点与持续时间。
        if ( sourceHold.m_isSubNote ) continue;
        Hold hold = sourceHold;
        scaleHoldTime(hold, speed);
        target.m_noteData.holds.push_back(std::move(hold));
    }
    for ( const auto& sourceFlick : source.m_noteData.flicks ) {
        // 顶层 Flick 保留方向和元数据，仅缩放公共锚点。
        if ( sourceFlick.m_isSubNote ) continue;
        Flick flick = sourceFlick;
        scaleNoteTime(flick, speed);
        target.m_noteData.flicks.push_back(std::move(flick));
    }

    for ( const auto& sourcePolyline : source.m_noteData.polylines ) {
        // 先复制根对象标量，再清空所有仍指向来源容器的引用列表。
        Polyline polyline = sourcePolyline;
        polyline.m_subNotes.clear();
        polyline.m_subHolds.clear();
        polyline.m_subFlicks.clear();
        scaleNoteTime(polyline, speed);

        // 按父列表原顺序重建目标子节点，维持折线几何顺序。
        for ( const auto& sourceSubNoteRef : sourcePolyline.m_subNotes ) {
            copyScaledPolylineSubNote(
                target, polyline, sourceSubNoteRef.get(), speed);
        }

        if ( !polyline.m_subNotes.empty() ) {
            // 根锚点以首个子节点为权威，兼容来源根字段与节点不一致的旧数据。
            const auto& firstSub = polyline.m_subNotes.front().get();
            polyline.m_timestamp = firstSub.m_timestamp;
            polyline.m_track     = firstSub.m_track;
        }
        // 所有子引用建立完成后再移动根对象进入目标容器。
        target.m_noteData.polylines.push_back(std::move(polyline));
    }

    // 重建统一顶层引用视图和确定性时间顺序。
    target.sync();
}

/// @brief 复制并缩放时间线事件。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledTimings(BeatMap& target, const BeatMap& source, double speed)
{
    // 值复制保留所有来源扩展属性，随后逐事件只改时间相关字段。
    target.m_timings = source.m_timings;
    for ( auto& timing : target.m_timings ) {
        scaleTiming(timing, speed);
    }
}

/// @brief 复制并缩放自动采样时间线。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledAudioSamples(BeatMap& target, const BeatMap& source,
                            double speed)
{
    // 自动采样值复制保留资源、轨道、音量、元数据和协作身份。
    target.m_audioSamples = source.m_audioSamples;
    for ( auto& sample : target.m_audioSamples ) {
        // 锚点保留浮点毫秒精度，偏移按其整数存储契约独立舍入。
        sample.m_timestamp = scaledMilliseconds(sample.m_timestamp, speed);
        sample.m_offsetMs  = scaledOffsetMilliseconds(sample.m_offsetMs, speed);
    }
}

}  // namespace

/// @brief 计算所有可触发内容的最晚有效时间。
/// @details
/// 分别扫描 Timing、自动采样、顶层物件及 Polyline 子节点；非有限时间被忽略，
/// Hold 以非负持续时间计算终点。初值为零，因此纯负时间内容不会产生负谱面长度。
double BeatmapSpeedTransform::calculateContentEndTime(const BeatMap& beatmap)
{
    double endTime = 0.0;
    // Timing 自身锚点也属于可见内容，即使谱面没有玩家物件。
    for ( const auto& timing : beatmap.m_timings ) {
        if ( std::isfinite(timing.m_timestamp) ) {
            endTime = std::max(endTime, timing.m_timestamp);
        }
    }
    for ( const auto& sample : beatmap.m_audioSamples ) {
        // 自动采样按锚点加有符号偏移后的真实触发时间参与长度计算。
        const double effectiveTimestamp = sample.effectiveTimestamp();
        if ( std::isfinite(effectiveTimestamp) ) {
            endTime = std::max(endTime, effectiveTimestamp);
        }
    }
    for ( const auto& note : beatmap.m_noteData.notes ) {
        // 普通物件结束于锚点，不需要查询派生持续时间。
        if ( std::isfinite(note.m_timestamp) ) {
            endTime = std::max(endTime, note.m_timestamp);
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        // Flick 的滑动方向不扩展时间范围，仍以自身时间作为终点。
        if ( std::isfinite(flick.m_timestamp) ) {
            endTime = std::max(endTime, flick.m_timestamp);
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        // Hold 必须同时具有有限起点和持续时间才能形成可信终点。
        if ( std::isfinite(hold.m_timestamp) &&
             std::isfinite(hold.m_duration) ) {
            // 负持续时间按零处理，不让损坏数据把结束点移到起点之前。
            endTime = std::max(
                endTime, hold.m_timestamp + std::max(0.0, hold.m_duration));
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        // 根时间先作为兜底，随后子节点给出实际折线内容末尾。
        if ( std::isfinite(polyline.m_timestamp) ) {
            endTime = std::max(endTime, polyline.m_timestamp);
        }
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();
            double      subEnd  = subNote.m_timestamp;
            if ( subNote.m_type == NoteType::HOLD ) {
                // 只有 Hold 子节点扩展终点，其他节点结束于自身锚点。
                const auto& subHold = static_cast<const Hold&>(subNote);
                subEnd += std::max(0.0, subHold.m_duration);
            }
            if ( std::isfinite(subEnd) ) {
                endTime = std::max(endTime, subEnd);
            }
        }
    }
    // 返回领域内容边界，不沿用来源元数据可能过长或过短的 map_length。
    return endTime;
}

/// @brief 创建完整独立且与目标音频速度同步的谱面副本。
/// @details
/// 先验证倍率并复制谱面级元数据，再分别重建 Timing、Sample 和 Note 容器。
/// 来源对象不被修改；成功结果的 map_length 始终由缩放后实际内容重新计算。
BeatmapSpeedTransformResult BeatmapSpeedTransform::createSpeedVersion(
    const BeatMap& source, const BeatmapSpeedTransformOptions& options)
{
    BeatmapSpeedTransformResult result;
    // 零、负数、NaN 和无穷都无法建立可逆的时间缩放关系。
    if ( options.speed <= 0.0 || !std::isfinite(options.speed) ) {
        result.errorMessage = "Invalid speed multiplier";
        return result;
    }

    // 先值复制通用及来源元数据，后续仅覆盖新版本明确指定的字段。
    result.beatmap.m_baseMapMetadata = source.m_baseMapMetadata;
    result.beatmap.m_metadata        = source.m_metadata;

    auto& meta = result.beatmap.m_baseMapMetadata;
    if ( !options.name.empty() ) {
        // 空名称表示沿用来源，允许调用方只修改速度和输出路径。
        meta.name = options.name;
    }
    if ( !options.version.empty() ) {
        meta.version = options.version;
    }
    meta.map_path = options.mapPath;
    // 旧单音频路径不再是权威来源，新音频仅作为歌曲文件提示保存。
    meta.main_audio_path.clear();
    meta.song_file_hint = options.audioPath;
    if ( meta.preference_bpm > 0.0 && std::isfinite(meta.preference_bpm) ) {
        // 参考 BPM 只有在来源有效时才随速度增长，未知值保持原哨兵。
        meta.preference_bpm *= options.speed;
    }
    if ( meta.video_starttime != 0 ) {
        // 视频起播偏移是整数毫秒，使用最近整数与谱面时间共同缩放。
        meta.video_starttime =
            static_cast<int32_t>(std::llround(scaledMilliseconds(
                static_cast<double>(meta.video_starttime), options.speed)));
    }

    // 各拥有型容器分别复制，Polyline 子引用在 Note 阶段重建。
    copyScaledTimings(result.beatmap, source, options.speed);
    copyScaledAudioSamples(result.beatmap, source, options.speed);
    copyScaledNotes(result.beatmap, source, options.speed);
    // 忽略来源声明长度，以缩放后所有实际内容的最晚时间为准。
    meta.map_length = calculateContentEndTime(result.beatmap);

    // 所有复制和引用重建完成后才发布成功结果。
    result.success = true;
    return result;
}

}  // namespace MMM
