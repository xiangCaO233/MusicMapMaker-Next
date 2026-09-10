#pragma once
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Flick.h"
#include "mmm/note/Hold.h"
#include <cmath>

namespace MMM::Test
{

/// @file TestHelper.hpp
/// @brief 为多种谱面格式往返测试提供统一的逻辑模型比较器。
///
/// 比较器关注格式间承诺保留的公共语义，不要求不同格式拥有相同的原始文本或
/// 私有扩展字段。时间误差按字段表示能力设置，避免整数毫秒格式因舍入被误判。
///
/// 公共比较契约包括：
/// - 玩家轨道数；
/// - 可选的 BGM 轨道数；
/// - Timing 数量与顺序；
/// - Timing 时间、BPM 和效果类型；
/// - 统一物件总数与顺序；
/// - 物件时间、轨道和基础类型；
/// - 玩家物件采样绑定的存在性；
/// - 玩家物件采样资源与音量；
/// - Hold 持续时间；
/// - Flick 目标轨偏移；
/// - 可选的自动采样完整字段。
///
/// 比较器不负责以下内容：
/// - 标题、作者等允许由目标格式规范化的展示元数据；
/// - JSON 键顺序、缩进或二进制字节级布局；
/// - 目标格式本身无法表达的私有属性；
/// - 音频资源实际存在或能够解码；
/// - 未调用 sync 时各派生索引的构造顺序。
///
/// 调用约束：
/// - 两个 BeatMap 都应完成格式加载与 sync；
/// - 调用者根据目标格式能力决定是否比较自动采样；
/// - 数量比较必须先于对应数组的逐项访问；
/// - 新增公共语义字段时，应在这里补充跨格式可比较条件；
/// - 格式专属容差应保持最小，不能掩盖实际对象错位。
///
/// 失败策略是遇到第一处差异立即返回，并在日志中提供索引与两侧值。
/// 这种策略让格式一致性测试保持确定输出，后续差异应在修复前一项后继续观察。
/// 本头只供测试目标使用，不应成为生产代码的模型等价性定义。
/// Polyline 节点连续性等结构约束由各格式专用比较器补充验证。
/// 自动采样扩展元数据不属于当前公共比较层。

/// @brief 比较两个已同步 BeatMap 的核心逻辑内容。
/// @param m1 基准谱面。
/// @param m2 往返后重新加载的谱面。
/// @param compareAudioSamples 格式可无损表达自动采样时设为 true。
/// @return 元数据、Timing、物件及按需启用的自动采样均一致时返回 true。
inline bool compareBeatMaps(const MMM::BeatMap& m1, const MMM::BeatMap& m2,
                            bool compareAudioSamples = false)
{
    // 轨道数决定物件布局，是所有格式都必须保留的基础元数据。
    if ( m1.m_baseMapMetadata.track_count !=
         m2.m_baseMapMetadata.track_count ) {
        XERROR("Track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.track_count,
               m2.m_baseMapMetadata.track_count);
        return false;
    }
    // BGM 轨只有在目标格式能完整表示自动采样时才属于可比较契约。
    if ( compareAudioSamples && m1.m_baseMapMetadata.bgm_track_count !=
                                    m2.m_baseMapMetadata.bgm_track_count ) {
        XERROR("BGM track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.bgm_track_count,
               m2.m_baseMapMetadata.bgm_track_count);
        return false;
    }

    // Timing 先比较数量，再按同步后的稳定顺序逐项比较。
    if ( m1.m_timings.size() != m2.m_timings.size() ) {
        XERROR("Timing count mismatch: {} vs {}",
               m1.m_timings.size(),
               m2.m_timings.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_timings.size(); ++i ) {
        // 相同索引表示同步后的相同逻辑事件；本辅助函数不在比较时重新排序。
        const auto& t1 = m1.m_timings[i];
        const auto& t2 = m2.m_timings[i];
        // 某些格式仅存整数毫秒，允许最多 2 ms 的连续换算舍入误差；BPM 保留
        // 两位以上精度，效果类型则必须严格一致。
        if ( std::abs(t1.m_timestamp - t2.m_timestamp) > 2.0 ||
             std::abs(t1.m_bpm - t2.m_bpm) > 1e-2 ||
             t1.m_timingEffect != t2.m_timingEffect ) {
            XERROR(
                "Timing mismatch at index {}: t1={}, bpm1={} | t2={}, bpm2={}",
                i,
                t1.m_timestamp,
                t1.m_bpm,
                t2.m_timestamp,
                t2.m_bpm);
            return false;
        }
    }

    // m_allNotes 是跨类型统一视图，数量差异意味着物件丢失或重复构建。
    if ( m1.m_allNotes.size() != m2.m_allNotes.size() ) {
        XERROR("Total note count mismatch: {} vs {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_allNotes.size(); ++i ) {
        // 基础字段通过后才读取派生类型，保证 static_cast 的类型前提成立。
        // 同步后按相同顺序比较基础身份，再根据实际类型补查派生字段。
        const Note& n1 = m1.m_allNotes[i].get();
        const Note& n2 = m2.m_allNotes[i].get();
        if ( std::abs(n1.m_timestamp - n2.m_timestamp) > 1e-3 ||
             n1.m_track != n2.m_track || n1.m_type != n2.m_type ) {
            XERROR(
                "Note mismatch at index {}: t1={}, tr1={}, typ1={} | t2={}, "
                "tr2={}, typ2={}",
                i,
                n1.m_timestamp,
                n1.m_track,
                (int)n1.m_type,
                n2.m_timestamp,
                n2.m_track,
                (int)n2.m_type);
            return false;
        }
        // 玩家物件绑定属于 Note 语义，与顶层自动采样列表分开验证。
        const auto binding1 = n1.getSampleBinding();
        const auto binding2 = n2.getSampleBinding();
        // 可选值的存在性、资源身份和音量都必须一致；不存在时不解引用。
        if ( binding1.has_value() != binding2.has_value() ||
             (binding1 &&
              (binding1->m_audioResourceId != binding2->m_audioResourceId ||
               std::abs(binding1->m_volume - binding2->m_volume) > 1e-6F)) ) {
            XERROR(
                "Note sample binding mismatch at index {}: ref1={}, vol1={} | "
                "ref2={}, vol2={}",
                i,
                binding1 ? binding1->m_audioResourceId : std::string{},
                binding1 ? binding1->m_volume : 0.0F,
                binding2 ? binding2->m_audioResourceId : std::string{},
                binding2 ? binding2->m_volume : 0.0F);
            return false;
        }
        if ( n1.m_type == NoteType::HOLD ) {
            // Hold 时长同样受整数毫秒格式影响，使用小于一个毫秒的容差。
            if ( std::abs(static_cast<const Hold&>(n1).m_duration -
                          static_cast<const Hold&>(n2).m_duration) > 0.2 ) {
                XERROR("Hold duration mismatch at index {}: {} vs {}",
                       i,
                       static_cast<const Hold&>(n1).m_duration,
                       static_cast<const Hold&>(n2).m_duration);
                return false;
            }
        } else if ( n1.m_type == NoteType::FLICK ) {
            // Flick 的目标轨道偏移是离散值，不允许近似比较。
            if ( static_cast<const Flick&>(n1).m_dtrack !=
                 static_cast<const Flick&>(n2).m_dtrack ) {
                XERROR("Flick dtrack mismatch at index {}: {} vs {}",
                       i,
                       static_cast<const Flick&>(n1).m_dtrack,
                       static_cast<const Flick&>(n2).m_dtrack);
                return false;
            }
        }
        // 普通 Note 与 Polyline
        // 的额外结构由格式分区测试承担，本层只比较公共项。
    }

    // 自动采样是可选比较层；RM 等隐式单音频格式不能承诺保留完整列表。
    if ( !compareAudioSamples ) return true;
    // 数量先行检查保证后续相同索引访问安全。
    if ( m1.m_audioSamples.size() != m2.m_audioSamples.size() ) {
        XERROR("Audio sample count mismatch: {} vs {}",
               m1.m_audioSamples.size(),
               m2.m_audioSamples.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_audioSamples.size(); ++i ) {
        // 自动采样数组次序属于同步后模型契约，发生错位时日志打印完整身份字段。
        // 调用方应先同步排序，因此逐索引比较时间、偏移、轨道、资源和音量。
        const AudioSampleEvent& s1 = m1.m_audioSamples[i];
        const AudioSampleEvent& s2 = m2.m_audioSamples[i];
        // effectiveTimestamp 补充验证时间戳与资源内偏移的组合播放位置。
        if ( std::abs(s1.m_timestamp - s2.m_timestamp) > 1e-3 ||
             s1.m_offsetMs != s2.m_offsetMs ||
             std::abs(s1.effectiveTimestamp() - s2.effectiveTimestamp()) >
                 1e-3 ||
             s1.m_track != s2.m_track ||
             s1.m_audioResourceId != s2.m_audioResourceId ||
             std::abs(s1.m_volume - s2.m_volume) > 1e-6F ) {
            XERROR(
                "Audio sample mismatch at index {}: "
                "t1={}, off1={}, tr1={}, ref1={}, vol1={} | "
                "t2={}, off2={}, tr2={}, ref2={}, vol2={}",
                i,
                s1.m_timestamp,
                s1.m_offsetMs,
                s1.m_track,
                s1.m_audioResourceId,
                s1.m_volume,
                s2.m_timestamp,
                s2.m_offsetMs,
                s2.m_track,
                s2.m_audioResourceId,
                s2.m_volume);
            return false;
        }
    }
    return true;
}

}  // namespace MMM::Test
