#include "network/collaboration/BeatmapDocumentCodec.h"

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace
{
// 将领域模型和编解码器类型限定在测试翻译单元，保持断言聚焦文档语义。
using MMM::BeatMap;
using MMM::BeatmapMutationFlags;
using MMM::Network::Collaboration::BeatmapDocumentCodec;
using MMM::Network::Collaboration::BeatmapDocumentError;

/// @brief 构造覆盖全部协作谱面数据类别的领域对象。
/// @details
/// 基准谱面同时包含基础元数据、自定义元数据、普通音符、长按、滑键、Polyline、
/// Timing、自动采样和独立批注。每类对象都写入至少一个非默认字段，使快照或
/// 分类增量漏掉字段时不会因默认值相同而误判成功。
/// author 是分类隔离测试主动改变的元数据字段，其余稳定内容提供共同基线。
/// @param author 写入基础元数据的制谱者名称。
/// @return 已执行 sync 且可直接编码的完整谱面共享对象。
std::shared_ptr<BeatMap> makeCompleteBeatmap(std::string author)
{
    // 共享对象符合编辑器实际持有方式，也便于物化结果进行生命周期比较。
    auto  beatmap = std::make_shared<BeatMap>();
    auto& base    = beatmap->m_baseMapMetadata;
    // 基础字段覆盖文本、数值、轨道布局和时长等不同序列化类型。
    base.name            = "Codec Test";
    base.title           = "Title";
    base.artist          = "Artist";
    base.version         = "Hard";
    base.author          = std::move(author);
    base.preference_bpm  = 180.0;
    base.track_count     = 6;
    base.bgm_track_count = 2;
    base.map_length      = 120000.0;
    // 格式专属 MapMetadata 验证嵌套枚举键到字符串表的往返。
    beatmap->m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        "key";

    // 普通 Note 提供稳定协作 ID、中文批注、采样绑定和自定义属性。
    auto& note             = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp       = 1000.0;
    note.m_track           = 1;
    note.m_collaborationId = "note-root";
    note.m_annotation      = "检查节奏重音";
    note.m_sampleBinding   = MMM::AudioSampleBinding{ "tap.wav", 0.8F };
    // 十六进制颜色作为不透明元数据值，不应被编解码器重新解释。
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["color"] =
        "#112233";

    // 根 Flick 使用负方向轨道偏移，覆盖其派生字段 dtrack。
    auto& flick             = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp       = 1500.0;
    flick.m_track           = 2;
    flick.m_dtrack          = -1;
    flick.m_collaborationId = "flick-root";

    // 子 Hold 同时进入根容器与 Polyline 引用，身份应只物化一次。
    auto& subHold             = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp       = 2000.0;
    subHold.m_duration        = 500.0;
    subHold.m_track           = 3;
    subHold.m_isSubNote       = true;
    subHold.m_collaborationId = "polyline-sub-hold";
    subHold.m_annotation      = "折线起段";
    // 子 Flick 具有不同时间和正向 dtrack，用于区分两类派生音符。
    auto& subFlick             = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp       = 2500.0;
    subFlick.m_track           = 3;
    subFlick.m_dtrack          = 2;
    subFlick.m_isSubNote       = true;
    subFlick.m_collaborationId = "polyline-sub-flick";
    // Polyline 根自身也有稳定 ID 和批注，并按引用保存其子物件。
    auto& polyline             = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp       = subHold.m_timestamp;
    polyline.m_track           = subHold.m_track;
    polyline.m_collaborationId = "polyline-root";
    polyline.m_annotation      = "整条折线说明";
    // 通用 subNotes 保留混合顺序，专用集合保留类型化访问关系。
    polyline.m_subNotes.emplace_back(subHold);
    polyline.m_subNotes.emplace_back(subFlick);
    polyline.m_subHolds.emplace_back(subHold);
    polyline.m_subFlicks.emplace_back(subFlick);

    // Timing 覆盖节拍换算值、效果枚举、效果参数和格式专属属性。
    auto& timing                   = beatmap->m_timings.emplace_back();
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 180.0;
    timing.m_beat_length           = 1000.0 / 3.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 180.0;
    timing.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["beat"] = "0,0,1";

    // 自动采样覆盖负偏移、独立轨道、资源 ID、音量和稳定协作 ID。
    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 750.0;
    sample.m_offsetMs        = -25;
    sample.m_track           = 6;
    sample.m_audioResourceId = "bgm.wav";
    sample.m_volume          = 0.65F;
    sample.m_collaborationId = "sample-root";
    sample.m_metadata.sample_properties[MMM::SampleMetadataType::MMM]["lane"] =
        "0";

    // 三类批注分别锚定时间点、玩家物件和自动采样。
    beatmap->m_annotations = {
        MMM::BeatmapAnnotation{
            // 时间点批注没有 targetId，内容含 Markdown 和换行。
            .m_id         = "annotation-timestamp",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            .m_timestamp  = 1000.0,
            .m_author     = "Creator A",
            .m_content    = "# 节奏\n检查重音",
        },
        MMM::BeatmapAnnotation{
            // 物件批注通过 note-root 关联普通 Note。
            .m_id         = "annotation-note",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
            .m_targetId   = "note-root",
            .m_timestamp  = 1000.0,
            .m_author     = "Reviewer",
            .m_content    = "物件批注",
        },
        MMM::BeatmapAnnotation{
            // 采样批注锚定 sample-root，并使用独立显示时间。
            .m_id         = "annotation-sample",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE,
            .m_targetId   = "sample-root",
            .m_timestamp  = 725.0,
            .m_author     = "Reviewer",
            .m_content    = "采样批注",
        },
    };
    // sync 重建类型索引和引用关系，确保编码器读取的是一致领域状态。
    beatmap->sync();
    return beatmap;
}

/// @brief 校验完整快照往返后仍保留全部数据类型及引用关系。
/// @details
/// 独立 encoder 与 receiver 模拟房主生成初始状态、访客从空文档应用快照。
/// ApplyResult 必须标记为全类别快照，物化后各根容器数量、Polyline 子引用、
/// 采样绑定、嵌套元数据、自动采样身份、批注和浮点 Timing 字段均需保留。
/// @par 快照契约
/// - 空 receiver 可以直接安装完整快照。
/// - ApplyResult::isSnapshot 必须为 true。
/// - ApplyResult::flags 必须覆盖 BeatmapMutationFlags::All。
/// - 根容器与独立批注数量必须与源文档一致。
/// - Polyline 的通用和类型化子引用必须同时重建。
/// - 不透明元数据、资源绑定和协作稳定 ID 不得规范化丢失。
/// @return 完整领域对象逐类恢复成功时返回 true。
bool testCompleteSnapshotRoundTrip()
{
    // receiver 初始没有基线，只有 isSnapshot=true 的载荷可以首次应用。
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec receiver;
    const auto           source = makeCompleteBeatmap("Creator A");
    auto snapshot = encoder.encode(*source, BeatmapMutationFlags::All, true);
    // 完整快照必须生成非空协议载荷。
    if ( !snapshot.has_value() || snapshot->empty() ) {
        XERROR("Complete snapshot could not be encoded");
        return false;
    }
    auto applied = receiver.apply(snapshot.value());
    // 结果标志是授权和刷新路径判断类别的公开契约。
    if ( !applied.has_value() || !applied->isSnapshot ||
         applied->flags != BeatmapMutationFlags::All ) {
        XERROR("Complete snapshot could not be applied");
        return false;
    }
    const auto restored = receiver.materialize();
    // 先检查所有顶层类别计数，防止后续 front 访问空容器。
    if ( !restored || restored->m_noteData.notes.size() != 1U ||
         restored->m_noteData.holds.size() != 1U ||
         restored->m_noteData.flicks.size() != 2U ||
         restored->m_noteData.polylines.size() != 1U ||
         restored->m_timings.size() != 1U ||
         restored->m_audioSamples.size() != 1U ||
         restored->m_annotations.size() != 3U ) {
        XERROR("Complete snapshot restored unexpected category counts");
        return false;
    }
    const auto& polyline = restored->m_noteData.polylines.front();
    // 深层字段选择不同序列化形态，补足仅计数断言覆盖不到的内容。
    const bool matches =
        polyline.m_subNotes.size() == 2U && polyline.m_subHolds.size() == 1U &&
        polyline.m_subFlicks.size() == 1U &&
        restored->m_noteData.notes.front().m_sampleBinding.has_value() &&
        restored->m_noteData.notes.front()
                .m_metadata.note_properties.at(MMM::NoteMetadataType::MMM)
                .at("color") == "#112233" &&
        restored->m_audioSamples.front().m_offsetMs == -25 &&
        restored->m_audioSamples.front().m_collaborationId == "sample-root" &&
        restored->m_annotations == source->m_annotations &&
        // Timing 浮点值允许极小表示误差，不使用未经界定的精确比较。
        std::abs(restored->m_timings.front().m_bpm - 180.0) < 1e-9;
    if ( !matches ) {
        XERROR("Complete snapshot fields did not round-trip");
    }
    return matches;
}

/// @brief 校验 Polyline 实际引用是子物件身份的权威来源。
/// @details
/// 测试故意把被 Polyline 引用的 Hold 和 Flick 的 m_isSubNote 改为 false，模拟
/// 上游状态标志过期。编码器应以实际引用图为准恢复子物件身份，不能把它们既
/// 当作根物件又复制进 Polyline，从而造成往返后对象数量增长。
/// @par 身份来源优先级
/// - Polyline 实际引用关系高于缓存的 m_isSubNote 标志。
/// - 被引用 Hold 仍只在 Hold 根存储中占一个领域对象。
/// - 被引用 Flick 仍只在 Flick 根存储中占一个领域对象。
/// - 物化后的 subNotes 必须重新标记两个引用为子物件。
/// - 编解码往返不得把引用图展开为重复值对象。
/// @return 根容器数量稳定且两个实际子引用均标记为子物件时返回 true。
bool testPolylineReferencesPreventDuplicateRootObjects()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec receiver;
    const auto           source = makeCompleteBeatmap("Creator");
    // 只破坏冗余标志，不改变 Polyline 中真实的 reference_wrapper 关系。
    source->m_noteData.holds.front().m_isSubNote = false;
    source->m_noteData.flicks.back().m_isSubNote = false;

    auto snapshot = encoder.encode(*source, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() ||
         !receiver.apply(snapshot.value()).has_value() ) {
        return false;
    }
    const auto restored = receiver.materialize();
    // 根类别计数不得因错误标志产生第二份复制对象。
    if ( !restored || restored->m_noteData.notes.size() != 1U ||
         restored->m_noteData.holds.size() != 1U ||
         restored->m_noteData.flicks.size() != 2U ||
         restored->m_noteData.polylines.size() != 1U ) {
        return false;
    }
    const auto& polyline = restored->m_noteData.polylines.front();
    // 通用子音符视图是重建引用关系后的权威观察入口。
    return polyline.m_subNotes.size() == 2U &&
           polyline.m_subNotes[0].get().m_isSubNote &&
           polyline.m_subNotes[1].get().m_isSubNote;
}

/// @brief 判断两个谱面的物件类别是否逐字段等价。
/// @details
/// 比较器覆盖普通 Note、Hold、Flick、Polyline 根及其通用子物件顺序。
/// collaborationId 是合并算法的稳定键，元数据和采样绑定属于对象正文；
/// Hold duration 与 Flick dtrack 则按运行时类型补充比较。
/// @param lhs 第一份物件文档。
/// @param rhs 第二份物件文档。
/// @return 所有物件字段、容器规模和 Polyline 子引用均等价时返回 true。
bool sameObjects(const BeatMap& lhs, const BeatMap& rhs)
{
    // optional 是否存在本身就是字段语义，存在时再比较资源 ID 和音量。
    const auto sameBinding = [](const auto& left, const auto& right) {
        if ( left.has_value() != right.has_value() ) return false;
        return !left || (left->m_audioResourceId == right->m_audioResourceId &&
                         std::abs(left->m_volume - right->m_volume) < 1e-6F);
    };
    // 先比较所有 Note 公共字段，再根据类型读取安全的派生字段。
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
        // 类型枚举已相等，因此对 Hold 的静态转换具有同一动态语义。
        if ( left.m_type == MMM::NoteType::HOLD ) {
            return std::abs(static_cast<const MMM::Hold&>(left).m_duration -
                            static_cast<const MMM::Hold&>(right).m_duration) <
                   1e-9;
        }
        // Flick 额外保存横向滑动轨道差值。
        if ( left.m_type == MMM::NoteType::FLICK ) {
            return static_cast<const MMM::Flick&>(left).m_dtrack ==
                   static_cast<const MMM::Flick&>(right).m_dtrack;
        }
        return true;
    };
    // 三个根 deque 使用相同顺序比较，避免仅按 ID 集合漏掉排序变化。
    const auto sameDeque = [&](const auto& left, const auto& right) {
        if ( left.size() != right.size() ) return false;
        for ( std::size_t index = 0; index < left.size(); ++index ) {
            if ( !sameNote(left[index], right[index]) ) return false;
        }
        return true;
    };
    // 顶层类别必须分别相等，Polyline 随后执行更深的子关系检查。
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
             // 专用子集合数量也属于重建后引用结构的一部分。
             left.m_subNotes.size() != right.m_subNotes.size() ||
             left.m_subHolds.size() != right.m_subHolds.size() ||
             left.m_subFlicks.size() != right.m_subFlicks.size() ) {
            return false;
        }
        // 通用 subNotes 保留 Hold 与 Flick 的混合顺序，需逐项比较正文。
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

/// @brief 判断两个谱面的独立多批注记录是否等价。
/// @details 批注类型提供值相等运算，比较涵盖稳定 ID、目标类别、目标 ID、
/// 时间、作者与 Markdown 内容，同时保留 vector 顺序。
/// @param lhs 第一份谱面。
/// @param rhs 第二份谱面。
/// @return 独立批注数组逐项相等时返回 true。
bool sameAnnotations(const BeatMap& lhs, const BeatMap& rhs)
{
    return lhs.m_annotations == rhs.m_annotations;
}

/// @brief 判断两个谱面的时间线类别是否逐字段等价。
/// @details
/// Timing 的时间、BPM、拍长和效果参数允许极小浮点误差；效果枚举及格式专属
/// 元数据必须精确一致。数组按顺序比较，保证时间线排序也没有改变。
/// @param lhs 第一份时间线文档。
/// @param rhs 第二份时间线文档。
/// @return Timing 数量和全部字段等价时返回 true。
bool sameTimelines(const BeatMap& lhs, const BeatMap& rhs)
{
    // 数量不同无需进入索引循环，且防止 rhs 越界。
    if ( lhs.m_timings.size() != rhs.m_timings.size() ) return false;
    for ( std::size_t index = 0; index < lhs.m_timings.size(); ++index ) {
        const auto& left  = lhs.m_timings[index];
        const auto& right = rhs.m_timings[index];
        // 浮点近似与枚举、嵌套属性精确比较共同定义领域等价。
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
/// @details
/// 自动采样独立于音符对象，比较其时间、毫秒偏移、轨道、稳定 ID、资源绑定、
/// 音量以及格式专属元数据，确保 AudioSamples 分类增量不会静默丢字段。
/// @param lhs 第一份采样文档。
/// @param rhs 第二份采样文档。
/// @return 自动采样数组逐字段等价时返回 true。
bool sameAudioSamples(const BeatMap& lhs, const BeatMap& rhs)
{
    if ( lhs.m_audioSamples.size() != rhs.m_audioSamples.size() ) return false;
    for ( std::size_t index = 0; index < lhs.m_audioSamples.size(); ++index ) {
        // 稳定顺序和数量已确认，当前索引代表同一逻辑采样。
        const auto& left  = lhs.m_audioSamples[index];
        const auto& right = rhs.m_audioSamples[index];
        if ( std::abs(left.m_timestamp - right.m_timestamp) >= 1e-9 ||
             left.m_offsetMs != right.m_offsetMs ||
             left.m_track != right.m_track ||
             left.m_collaborationId != right.m_collaborationId ||
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
/// @details
/// 基础元数据没有统一值相等运算，因此显式列举所有协作序列化字段，并额外比较
/// MapMetadata 的格式属性表。该冗长列表是防止新增字段在分类隔离测试中漏检的
/// 可审查契约，而不是仅比较本场景主动修改的 author 和 title。
/// @param lhs 第一份谱面元数据。
/// @param rhs 第二份谱面元数据。
/// @return 全部基础和扩展元数据字段等价时返回 true。
bool sameMetadata(const BeatMap& lhs, const BeatMap& rhs)
{
    const auto& left  = lhs.m_baseMapMetadata;
    const auto& right = rhs.m_baseMapMetadata;
    return left.name == right.name && left.title == right.title &&
           left.title_unicode == right.title_unicode &&
           left.artist == right.artist &&
           left.artist_unicode == right.artist_unicode &&
           left.album == right.album && left.map_path == right.map_path &&
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
/// @details
/// 对 Objects、Timelines、AudioSamples、Metadata 和 Annotations 五个独立标志
/// 分别创建全新编码器与接收器。edited 同时修改所有类别，但每轮 delta 只声明
/// 一个标志；物化结果中该类别必须来自 edited，其余类别必须保持 initial。
/// 这可发现编码器意外携带额外类别，或接收器应用增量时整体替换文档的问题。
/// @par 每轮预期
/// - Objects 只替换对象正文和对象元数据。
/// - Timelines 只替换 Timing 列表及其效果字段。
/// - AudioSamples 只替换自动采样列表。
/// - Metadata 只替换基础元数据与格式属性。
/// - Annotations 只合并独立多批注记录。
/// - 未声明类别必须逐字段保持 initial，而不只是保持数量。
/// @return 五种分类增量均严格隔离且内容完整时返回 true。
bool testStrictCategoryIsolation()
{
    // All 只用于基线快照，不属于需要单独验证的增量类别。
    constexpr std::array FLAGS{
        BeatmapMutationFlags::Objects,      BeatmapMutationFlags::Timelines,
        BeatmapMutationFlags::AudioSamples, BeatmapMutationFlags::Metadata,
        BeatmapMutationFlags::Annotations,
    };
    // 每轮使用独立 Codec，避免上一类别增量改变下一轮编码基线。
    for ( const auto flag : FLAGS ) {
        BeatmapDocumentCodec receiver;
        BeatmapDocumentCodec encoder;
        auto                 initial = makeCompleteBeatmap("Creator A");
        auto                 edited  = makeCompleteBeatmap("Creator B");
        // 物件同时改变公共时间和嵌套元数据，覆盖更新字段集合。
        edited->m_noteData.notes.front().m_timestamp = 12345.0;
        edited->m_noteData.notes.front()
            .m_metadata.note_properties[MMM::NoteMetadataType::MMM]["delta"] =
            "objects";
        // Timeline 改变时间与效果参数，保持 BPM 等其他字段作为对照。
        edited->m_timings.front().m_timestamp             = 875.0;
        edited->m_timings.front().m_timingEffectParameter = 90.0;
        edited->m_audioSamples.front().m_offsetMs         = 321;
        edited->m_audioSamples.front().m_volume           = 0.25F;
        // Metadata 改变文本和数值字段，证明不依赖单一类型。
        edited->m_baseMapMetadata.title         = "Changed Title";
        edited->m_baseMapMetadata.album         = "Collaboration Album";
        edited->m_baseMapMetadata.map_length    = 654321.0;
        edited->m_annotations.front().m_content = "## 新批注";
        // Annotations 同时覆盖已有 ID 更新和新 ID 追加。
        edited->m_annotations.emplace_back(MMM::BeatmapAnnotation{
            .m_id         = "annotation-added",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            .m_timestamp  = 2500.0,
            .m_author     = "Creator B",
            .m_content    = "- 并发追加",
        });
        edited->sync();

        // receiver 先应用完整基线，再应用本轮唯一分类增量。
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
        // 每个比较器根据当前 flag 选择 edited 或 initial 作为期望来源。
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
/// @details
/// 三个 Codec 分别模拟发起访客、权威房主和另一广播接收端。它们从相同快照
/// 起步，随后连续 32 轮修改同一普通 Note，访客编码 Objects 增量，三端按房主
/// 广播语义应用同一载荷。每轮都比较完整物件图和根容器数量，防止基线差分
/// 在重复往返中逐步复制 Polyline 子物件或遗漏先前修改。
/// @par 每轮状态迁移
/// - 访客从已确认文档物化可编辑副本。
/// - 本地副本只修改 note-root 的时间和 round 元数据。
/// - 房主先应用请求形成权威可见文档。
/// - 原访客和远端访客再应用同一广播载荷。
/// - 三端物件图必须完全一致后才能进入下一轮。
/// - 根对象计数在所有轮次保持固定，不能随往返增长。
/// @return 三端在全部轮次保持物件等价和固定类别数量时返回 true。
bool testRepeatedBidirectionalObjectRoundTrips()
{
    // guestDocument 同时承担本地物化和下一轮增量编码基线。
    BeatmapDocumentCodec guestDocument;
    BeatmapDocumentCodec hostDocument;
    BeatmapDocumentCodec broadcaster;
    auto                 current = makeCompleteBeatmap("Creator");
    auto                 snapshot =
        guestDocument.encode(*current, BeatmapMutationFlags::All, true);
    // 三端必须从字节完全相同的初始快照开始。
    if ( !snapshot.has_value() ||
         !guestDocument.apply(snapshot.value()).has_value() ||
         !hostDocument.apply(snapshot.value()).has_value() ||
         !broadcaster.apply(snapshot.value()).has_value() ) {
        return false;
    }

    for ( std::uint32_t round = 0; round < 32U; ++round ) {
        // 每轮基于访客当前已应用的权威状态继续编辑。
        current = guestDocument.materialize();
        if ( !current ) return false;
        current->m_noteData.notes.front().m_timestamp += 1.0;
        // round 属性使每次增量正文都不同并可追踪最新轮次。
        current->m_noteData.notes.front()
            .m_metadata.note_properties[MMM::NoteMetadataType::MMM]["round"] =
            std::to_string(round);
        current->sync();

        auto request = guestDocument.encode(
            *current, BeatmapMutationFlags::Objects, false);
        // 房主先应用请求，代表为本轮操作确定权威顺序。
        if ( !request.has_value() ||
             !hostDocument.apply(request.value()).has_value() ) {
            return false;
        }
        auto authoritative = hostDocument.materialize();
        if ( !authoritative ) return false;
        if ( !guestDocument.apply(request.value()).has_value() ||
             // 发起端也只在权威广播阶段把请求提交到自己的文档。
             !broadcaster.apply(request.value()).has_value() ) {
            return false;
        }
        auto guest      = guestDocument.materialize();
        auto remotePeer = broadcaster.materialize();
        // 深比较之外固定根数量，专门防止子物件在每轮被提升为新根。
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
/// @details
/// 房主和两名访客分别从相同快照维护自己的编码基线，并各自追加一个具有唯一
/// collaborationId 的 Note。authority 按“访客一、房主、访客二”的确定顺序
/// 应用三个增量；集合合并不得采用整类覆盖，否则后到增量会删除先到新增项。
/// @par 合并不变量
/// - 三个差分都以同一个初始对象集合为基线。
/// - 每个差分只携带一个稳定 ID 唯一的新增对象。
/// - authority 的应用顺序代表房主确定的提交总序。
/// - 后到新增不得删除先到新增或原有 note-root。
/// - 结果按稳定身份去重后应恰有四个根 Note。
/// - 容器内部排序不作为跨端协议契约。
/// @return 初始 Note 和三端新增 Note 的时间戳全部存在时返回 true。
bool testConcurrentObjectDeltasMerge()
{
    // 每个发送端需要独立基线，否则其中一端的 encode 会污染另一端差分。
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

    // 房主新增项使用稳定 ID，供 authority 按对象身份执行集合合并。
    auto  hostEdit             = makeCompleteBeatmap("Creator");
    auto& hostNote             = hostEdit->m_noteData.notes.emplace_back();
    hostNote.m_timestamp       = 3000.0;
    hostNote.m_track           = 0;
    hostNote.m_collaborationId = "host-added-note";
    hostEdit->sync();

    // 第一访客从同一旧基线新增位于 4000 ms 的独立对象。
    auto  firstGuestEdit = makeCompleteBeatmap("Creator");
    auto& firstGuestNote = firstGuestEdit->m_noteData.notes.emplace_back();
    firstGuestNote.m_timestamp       = 4000.0;
    firstGuestNote.m_track           = 1;
    firstGuestNote.m_collaborationId = "first-guest-added-note";
    firstGuestEdit->sync();

    // 第二访客新增第三个唯一对象，形成三个并发差分来源。
    auto  secondGuestEdit = makeCompleteBeatmap("Creator");
    auto& secondGuestNote = secondGuestEdit->m_noteData.notes.emplace_back();
    secondGuestNote.m_timestamp       = 5000.0;
    secondGuestNote.m_track           = 2;
    secondGuestNote.m_collaborationId = "second-guest-added-note";
    secondGuestEdit->sync();

    // 三端独立编码的差分都只应包含相对于共同基线的新增操作。
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

    // authority 应保留基线对象加三个新增对象，共四个根 Note。
    const auto merged = authority.materialize();
    if ( !merged || merged->m_noteData.notes.size() != 4U ) return false;
    constexpr std::array EXPECTED_TIMESTAMPS{ 1000.0, 3000.0, 4000.0, 5000.0 };
    // 合并顺序不作为容器排序契约，按时间戳集合检查所有逻辑对象存在。
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

/// @brief 校验两端从同一旧基线修改同一稳定物件时采用房主顺序覆盖而不重复。
/// @details
/// 两个发送端都编辑 collaborationId 为 note-root 的同一 Note。第一端只修改
/// timestamp，第二端只修改 track，但第二份差分仍是相对于共同旧基线生成。
/// authority 顺序应用后应保持单一对象，并以最后到达的完整对象正文覆盖前一版：
/// track 取第二端的五，timestamp 回到第二端基线中的 1000，而不是字段级拼接。
/// @par 覆盖语义
/// - collaborationId 决定两份增量指向同一逻辑对象。
/// - 第一份增量把 timestamp 更新为 1250。
/// - 第二份增量携带基线 timestamp 1000 与新 track 5。
/// - 后到完整对象正文覆盖前到版本，不执行未知的字段级合并。
/// - 结果仍只有一个 note-root，不能生成冲突副本。
/// @return 稳定 ID 未复制且最终正文符合后到增量时返回 true。
bool testConcurrentSameObjectUsesStableIdentity()
{
    // firstEncoder 生成共同快照并保留第一端基线。
    BeatmapDocumentCodec firstEncoder;
    BeatmapDocumentCodec secondEncoder;
    BeatmapDocumentCodec authority;
    auto                 initial = makeCompleteBeatmap("Creator");
    auto                 snapshot =
        firstEncoder.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() ||
         !authority.apply(snapshot.value()).has_value() ) {
        return false;
    }
    secondEncoder.synchronizeEncodingBaseline(*initial);

    // 第一端修改时间，保持轨道为初始值。
    auto firstEdit = makeCompleteBeatmap("Creator");
    firstEdit->m_noteData.notes.front().m_timestamp = 1250.0;
    firstEdit->sync();
    // 第二端修改轨道，保持时间为共同基线值。
    auto secondEdit = makeCompleteBeatmap("Creator");
    secondEdit->m_noteData.notes.front().m_track = 5;
    secondEdit->sync();

    auto firstDelta =
        firstEncoder.encode(*firstEdit, BeatmapMutationFlags::Objects, false);
    // 按房主接收顺序应用，第二端成为同一稳定对象的最终版本。
    auto secondDelta =
        secondEncoder.encode(*secondEdit, BeatmapMutationFlags::Objects, false);
    if ( !firstDelta.has_value() || !secondDelta.has_value() ||
         !authority.apply(firstDelta.value()).has_value() ||
         !authority.apply(secondDelta.value()).has_value() ) {
        return false;
    }

    const auto merged = authority.materialize();
    // 一个 collaborationId 只能对应一个根 Note，禁止追加重复副本。
    return merged && merged->m_noteData.notes.size() == 1U &&
           merged->m_noteData.notes.front().m_collaborationId == "note-root" &&
           std::abs(merged->m_noteData.notes.front().m_timestamp - 1000.0) <
               1e-9 &&
           merged->m_noteData.notes.front().m_track == 5U;
}

/// @brief 校验两个客户端从同一基线新增批注时按稳定 ID 合并。
/// @details
/// 两个编码器从相同三条批注基线出发，各自追加一条具有不同 m_id 的时间点批注。
/// authority 顺序应用两个 Annotations 增量后应得到五条记录；后到增量不能以其
/// 整个数组覆盖先到新增项。测试按稳定 ID 查找，不约束合并后的展示排序。
/// @par 批注合并不变量
/// - m_id 是批注跨端合并的稳定身份。
/// - 相同 timestamp 不意味着两条批注发生冲突。
/// - 不同作者和正文必须随各自 ID 一起保留。
/// - 后到差分不能删除前到差分新增的批注。
/// - 三条基线记录不能在合并过程中重复或丢失。
/// @return 两端新增批注均保留且原批注不丢失时返回 true。
bool testConcurrentAnnotationDeltasMerge()
{
    BeatmapDocumentCodec firstEncoder;
    BeatmapDocumentCodec secondEncoder;
    BeatmapDocumentCodec authority;
    auto                 initial = makeCompleteBeatmap("Creator");
    const auto           snapshot =
        firstEncoder.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot || !authority.apply(*snapshot) ) return false;
    secondEncoder.synchronizeEncodingBaseline(*initial);

    // 第一端新增 ID 唯一的批注，时间与第二端相同以排除按时间误合并。
    auto firstEdit = makeCompleteBeatmap("Creator");
    firstEdit->m_annotations.emplace_back(MMM::BeatmapAnnotation{
        .m_id         = "annotation-from-first",
        .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
        .m_timestamp  = 3000.0,
        .m_author     = "First Creator",
        .m_content    = "第一个客户端",
    });
    // 第二端只改变 ID、作者和正文，目标类型及时间保持一致。
    auto secondEdit = makeCompleteBeatmap("Creator");
    secondEdit->m_annotations.emplace_back(MMM::BeatmapAnnotation{
        .m_id         = "annotation-from-second",
        .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
        .m_timestamp  = 3000.0,
        .m_author     = "Second Creator",
        .m_content    = "第二个客户端",
    });

    const auto firstDelta = firstEncoder.encode(
        *firstEdit, BeatmapMutationFlags::Annotations, false);
    // 两份差分相对于同一基线独立生成，再由 authority 决定应用总序。
    const auto secondDelta = secondEncoder.encode(
        *secondEdit, BeatmapMutationFlags::Annotations, false);
    if ( !firstDelta || !secondDelta || !authority.apply(*firstDelta) ||
         !authority.apply(*secondDelta) ) {
        return false;
    }

    const auto merged = authority.materialize();
    // 三条基线加两条并发新增应恰好形成五条，不允许重复应用。
    if ( !merged || merged->m_annotations.size() != 5U ) return false;
    const auto hasId = [&](std::string_view identity) {
        // 批注顺序不是此场景的关注点，按稳定 ID 搜索合并结果。
        return std::any_of(merged->m_annotations.begin(),
                           merged->m_annotations.end(),
                           [&](const auto& annotation) {
                               return annotation.m_id == identity;
                           });
    };
    return hasId("annotation-from-first") && hasId("annotation-from-second");
}

/// @brief 校验元数据增量小于完整快照且不会覆盖其它谱面类别。
/// @details
/// 单个 Codec 先建立完整基线，再把仅 author 不同的谱面编码为 Metadata 增量。
/// 增量应明显小于全快照，ApplyResult 必须报告非快照和精确分类；物化结果只更新
/// author，物件与自动采样仍保持原数量和代表性字段。
/// @par 分类契约
/// - Metadata delta 的字节数应小于包含所有类别的 snapshot。
/// - ApplyResult::isSnapshot 必须为 false。
/// - ApplyResult::flags 必须恰为 Metadata。
/// - author 更新为 Creator B。
/// - Objects 和 AudioSamples 保持初始内容。
/// @return 元数据增量紧凑、分类正确且其他类别不变时返回 true。
bool testCategoryDelta()
{
    // 编码和应用同一快照，使 codec 同时具有编码、解码两侧基线。
    BeatmapDocumentCodec codec;
    auto                 initial = makeCompleteBeatmap("Creator A");
    auto snapshot = codec.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot.has_value() || !codec.apply(snapshot.value()).has_value() ) {
        return false;
    }

    auto edited = makeCompleteBeatmap("Creator B");
    // 两份完整领域对象仅 author 不同，便于隔离 Metadata 差分大小。
    auto delta = codec.encode(*edited, BeatmapMutationFlags::Metadata, false);
    if ( !delta.has_value() || delta->size() >= snapshot->size() ) return false;
    auto applied = codec.apply(delta.value());
    // 分类标志供上层决定局部刷新范围，不能被错误提升为 All。
    if ( !applied.has_value() || applied->isSnapshot ||
         applied->flags != BeatmapMutationFlags::Metadata ) {
        return false;
    }
    auto restored = codec.materialize();
    // 代表性对象时间和采样数量证明未声明类别没有被清空或替换。
    return restored && restored->m_baseMapMetadata.author == "Creator B" &&
           restored->m_noteData.notes.size() == 1U &&
           restored->m_noteData.notes.front().m_timestamp == 1000.0 &&
           restored->m_audioSamples.size() == 1U;
}

/// @brief 校验授权路径可在不持有文档基线时识别快照与各增量类别。
/// @details
/// inspect 是房主在授权或路由前读取载荷类别的静态只读入口，不应要求接收器
/// 已应用快照，也不能改变任意 Codec 实例。测试依次检查完整快照、Metadata
/// 增量和畸形短帧，并持续确认 untouched 仍没有文档。
/// @par 只读保证
/// - inspect 不依赖某个 BeatmapDocumentCodec 实例的基线。
/// - 快照头应报告 All 和 isSnapshot=true。
/// - 增量头应报告实际分类和 isSnapshot=false。
/// - 检查成功不得创建或物化内部文档。
/// - 检查失败不得接受部分解析的畸形载荷。
/// @return 合法载荷分类准确、畸形载荷拒绝且无状态被修改时返回 true。
bool testPayloadInspectionIsNonMutating()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec untouched;
    auto                 initial = makeCompleteBeatmap("Creator A");
    auto snapshot = encoder.encode(*initial, BeatmapMutationFlags::All, true);
    if ( !snapshot ) return false;
    const auto inspectedSnapshot = BeatmapDocumentCodec::inspect(*snapshot);
    // 静态检查只解析头部与结构，不会把快照安装到 untouched。
    if ( !inspectedSnapshot || !inspectedSnapshot->isSnapshot ||
         inspectedSnapshot->flags != BeatmapMutationFlags::All ||
         untouched.hasDocument() ) {
        return false;
    }

    initial->m_baseMapMetadata.author = "Creator B";
    // encoder 已由快照建立基线，因此可生成单类别增量供检查。
    auto metadataDelta =
        encoder.encode(*initial, BeatmapMutationFlags::Metadata, false);
    if ( !metadataDelta ) return false;
    const auto inspectedDelta = BeatmapDocumentCodec::inspect(*metadataDelta);
    // 增量必须报告 Metadata 且 isSnapshot=false，仍不创建接收文档。
    if ( !inspectedDelta || inspectedDelta->isSnapshot ||
         inspectedDelta->flags != BeatmapMutationFlags::Metadata ||
         untouched.hasDocument() ) {
        return false;
    }

    const MMM::Network::Collaboration::ByteBuffer malformed{ 0xFFU, 0x00U };
    // 不完整且无效的 CBOR/压缩头必须以空 expected 返回。
    return !BeatmapDocumentCodec::inspect(malformed).has_value();
}

/// @brief 校验后台可见文档比较只返回实际增删改的根物件稳定标识。
/// @details
/// initial 与 updated 分别编码为独立完整快照，再由两个只读文档执行比较。
/// updated 修改 note-root，替换根 Flick 的 ID，并删除包含 Hold、子 Flick 的
/// Polyline。结果只应包含受影响的四个根稳定 ID，不应把 Polyline 内部子物件
/// 作为独立可见对象报告给界面。
/// @par 预期变化集合
/// - note-root 因时间字段改变而被报告。
/// - flick-root 因旧稳定 ID 消失而被报告。
/// - added-flick 因新稳定 ID 出现而被报告。
/// - polyline-root 因整个根对象删除而被报告。
/// - Polyline 的内部 Hold 与 Flick ID 不作为独立可见变化返回。
/// @return 变更 ID 排序后与预期根对象集合完全一致时返回 true。
bool testChangedObjectIdentitiesComparedToVisibleDocument()
{
    BeatmapDocumentCodec initialEncoder;
    BeatmapDocumentCodec updatedEncoder;
    BeatmapDocumentCodec initialDocument;
    BeatmapDocumentCodec updatedDocument;
    auto                 initial = makeCompleteBeatmap("Creator");
    auto                 updated = makeCompleteBeatmap("Creator");

    // 修改保留 ID 的普通 Note，应该报告 note-root。
    updated->m_noteData.notes.front().m_timestamp += 25.0;
    // 根 Flick 更换 ID，相当于删除 flick-root 并新增 added-flick。
    updated->m_noteData.flicks.front().m_collaborationId = "added-flick";
    // 删除 Polyline 及其根容器子对象，只向可见层报告 polyline-root。
    updated->m_noteData.polylines.clear();
    updated->m_noteData.holds.clear();
    updated->m_noteData.flicks.pop_back();
    updated->sync();

    const auto initialSnapshot =
        initialEncoder.encode(*initial, BeatmapMutationFlags::All, true);
    const auto updatedSnapshot =
        updatedEncoder.encode(*updated, BeatmapMutationFlags::All, true);
    if ( !initialSnapshot || !updatedSnapshot ||
         !initialDocument.apply(*initialSnapshot) ||
         !updatedDocument.apply(*updatedSnapshot) ) {
        return false;
    }

    auto changed =
        updatedDocument.changedObjectIdentitiesComparedTo(initialDocument);
    if ( !changed ) return false;
    std::sort(changed->begin(), changed->end());
    // 排序消除实现内部哈希容器遍历顺序，不改变集合语义。
    const std::array<std::string, 4> expected{
        "added-flick", "flick-root", "note-root", "polyline-root"
    };
    return changed->size() == expected.size() &&
           std::equal(changed->begin(), changed->end(), expected.begin());
}

/// @brief 校验没有基础快照时拒绝增量和畸形 CBOR。
/// @details
/// synchronizeEncodingBaseline 只建立发送侧差分基线，不会让同一 Codec 获得接收
/// 文档。因此它生成的合法 Objects 增量在 apply 时必须报告 MissingSnapshot。
/// 随后的任意畸形字节则应报告 InvalidPayload，证明状态前置条件错误与格式错误
/// 使用不同诊断枚举，且失败不会隐式创建文档。
/// @par 错误分类
/// - 合法 delta 加空接收基线得到 MissingSnapshot。
/// - 畸形帧无论发送基线如何都得到 InvalidPayload。
/// - 两种失败都不得返回 ApplyResult。
/// - 失败后 Codec 不得把待应用 delta 当作完整文档保存。
/// @return 两种非法应用分别返回预期错误时返回 true。
bool testInvalidPayloads()
{
    // 先建立发送基线并制造一个最小对象更新。
    BeatmapDocumentCodec codec;
    auto                 beatmap = makeCompleteBeatmap("Creator");
    codec.synchronizeEncodingBaseline(*beatmap);
    beatmap->m_noteData.notes.front().m_timestamp += 1.0;
    beatmap->sync();
    auto delta = codec.encode(*beatmap, BeatmapMutationFlags::Objects, false);
    if ( !delta.has_value() ) return false;
    auto missingSnapshot = codec.apply(delta.value());
    // delta 本身合法，但接收侧没有基础文档，错误必须精确归类。
    if ( missingSnapshot.has_value() ||
         missingSnapshot.error() != BeatmapDocumentError::MissingSnapshot ) {
        return false;
    }
    const MMM::Network::Collaboration::ByteBuffer malformed{ 0xFF, 0x00 };
    // 畸形数据与文档水位无关，应在结构解析阶段报告 InvalidPayload。
    auto malformedResult = codec.apply(malformed);
    return !malformedResult.has_value() &&
           malformedResult.error() == BeatmapDocumentError::InvalidPayload;
}

/// @brief 校验大谱面快照经过压缩后仍能在单条协作消息上限内往返。
/// @details
/// 基准谱面扩展为两万个结构相似但稳定 ID 唯一的 Note，用于验证完整快照压缩
/// 后低于 1 MiB 协作操作上限。随后连续修改前 64 个对象，每次编码和应用一个
/// Objects 增量，要求单条差分低于 2 KiB，证明编码基线随每轮更新而推进。
/// 最终检查总数、首对象修改和末对象字段，覆盖头尾未丢失及稳定身份查找。
/// @par 规模边界
/// - 20000 个对象足以覆盖压缩器的大输入路径。
/// - 每个对象稳定 ID 唯一，禁止通过内容相同折叠对象。
/// - 完整快照必须低于协作操作的 1 MiB 上限。
/// - 单对象增量必须低于 2 KiB，避免每轮退化为全类别快照。
/// - 64 次连续编码必须逐次更新发送基线。
/// - 最终对象总数和头尾内容必须同时正确。
/// @return 大快照与 64 次紧凑增量完整往返时返回 true。
bool testLargeSnapshotCompression()
{
    BeatmapDocumentCodec encoder;
    BeatmapDocumentCodec receiver;
    auto                 beatmap = makeCompleteBeatmap("Large Creator");
    /// @brief 压缩回归使用的根 Note 数量。
    constexpr std::size_t LARGE_NOTE_COUNT = 20000;
    // 清除小基准中的普通 Note，仅保留本场景规律生成的大集合。
    beatmap->m_noteData.notes.clear();
    for ( std::size_t index = 0; index < LARGE_NOTE_COUNT; ++index ) {
        // 时间、轨道和稳定 ID 随索引变化，提供可压缩但非完全重复的数据。
        auto& note             = beatmap->m_noteData.notes.emplace_back();
        note.m_timestamp       = static_cast<double>(index) * 25.0;
        note.m_track           = static_cast<int>(index % 6U);
        note.m_collaborationId = "large-note-" + std::to_string(index);
    }
    beatmap->sync();

    auto snapshot = encoder.encode(*beatmap, BeatmapMutationFlags::All, true);
    // 1 MiB 是协作单条操作上限，压缩快照必须留在其内。
    if ( !snapshot.has_value() || snapshot->size() >= 1024U * 1024U ) {
        return false;
    }
    auto applied = receiver.apply(snapshot.value());
    if ( !applied.has_value() || !applied->isSnapshot ) return false;
    /// @brief 用于验证编码基线持续推进的连续编辑轮数。
    constexpr std::size_t INCREMENTAL_EDIT_COUNT = 64;
    for ( std::size_t index = 0; index < INCREMENTAL_EDIT_COUNT; ++index ) {
        // 每轮只修改一个此前未改对象，理想差分应保持常量级规模。
        beatmap->m_noteData.notes[index].m_timestamp += 1.0;
        auto delta =
            encoder.encode(*beatmap, BeatmapMutationFlags::Objects, false);
        if ( !delta.has_value() || delta->size() >= 2048U ||
             !receiver.apply(delta.value()).has_value() ) {
            return false;
        }
    }
    const auto restored = receiver.materialize();
    // 所有增量完成后根对象总数必须仍为两万。
    if ( !restored || restored->m_noteData.notes.size() != LARGE_NOTE_COUNT ) {
        return false;
    }
    const auto last =
        // 通过稳定 ID 定位尾对象，避免依赖物化容器内部排序。
        std::find_if(restored->m_noteData.notes.begin(),
                     restored->m_noteData.notes.end(),
                     [](const MMM::Note& note) {
                         return note.m_collaborationId == "large-note-19999";
                     });
    const auto first =
        // 首对象经历第一轮 +1 ms 更新，用它验证增量确实应用。
        std::find_if(restored->m_noteData.notes.begin(),
                     restored->m_noteData.notes.end(),
                     [](const MMM::Note& note) {
                         return note.m_collaborationId == "large-note-0";
                     });
    return last != restored->m_noteData.notes.end() && last->m_track == 1 &&
           first != restored->m_noteData.notes.end() &&
           first->m_timestamp == 1.0;
}
}  // namespace

/// @brief 运行协作谱面文档编解码完整回归。
/// @details
/// check 在保持短路执行的同时记录首个失败场景名称。场景顺序先覆盖完整快照和
/// 引用重建，再覆盖分类隔离、并发合并、只读检查与非法载荷，最后执行成本较高
/// 的两万 Note 压缩回归。所有场景通过时退出零，否则退出一交由 CTest 报告。
/// @par 场景分组
/// - complete snapshot 验证全部领域类别的初始安装。
/// - polyline references 验证引用图高于冗余子物件标志。
/// - category isolation 与 category delta 验证分类边界。
/// - bidirectional object round trips 验证重复广播稳定性。
/// - concurrent object deltas 验证不同稳定 ID 的集合合并。
/// - same object identity 验证相同稳定 ID 的顺序覆盖。
/// - concurrent annotation deltas 验证批注集合合并。
/// - changed object identities 验证界面可见根对象差异。
/// - payload inspection 验证授权前只读分类。
/// - invalid payloads 验证错误类型边界。
/// - large snapshot compression 验证消息规模上限。
/// @par 诊断约束
/// 每个场景名称保持稳定，失败日志可直接映射到上述契约；入口不捕获异常，
/// 不修改用户文件，也不依赖外部资源或配置目录。
/// @note 各 Codec 与 BeatMap 都在场景内部创建，测试之间不共享编码基线。
/// @note 所有比较均针对物化领域状态，不依赖 JSON/CBOR 对象键的遍历顺序。
/// @note 大谱面场景最后执行，避免早期契约失败时承担无关压缩成本。
/// @note 成功退出前所有 shared_ptr 均按普通作用域生命周期释放。
/// @return 全部编解码场景通过时返回 0。
int main()
{
    // 统一失败日志使用项目日志宏，不向标准输出直接写入。
    const auto check = [](bool result, std::string_view name) {
        if ( !result ) XERROR("Beatmap document codec test failed: {}", name);
        return result;
    };
    // 逻辑与保持首个失败后不再运行后续场景，缩短故障反馈并保留定位日志。
    return check(testCompleteSnapshotRoundTrip(), "complete snapshot") &&
                   check(testPolylineReferencesPreventDuplicateRootObjects(),
                         "polyline references") &&
                   check(testStrictCategoryIsolation(), "category isolation") &&
                   check(testRepeatedBidirectionalObjectRoundTrips(),
                         "bidirectional object round trips") &&
                   check(testConcurrentObjectDeltasMerge(),
                         "concurrent object deltas") &&
                   check(testConcurrentSameObjectUsesStableIdentity(),
                         "same object identity") &&
                   check(testConcurrentAnnotationDeltasMerge(),
                         "concurrent annotation deltas") &&
                   check(testCategoryDelta(), "category delta") &&
                   check(testChangedObjectIdentitiesComparedToVisibleDocument(),
                         "changed object identities") &&
                   check(testPayloadInspectionIsNonMutating(),
                         "payload inspection") &&
                   check(testInvalidPayloads(), "invalid payloads") &&
                   check(testLargeSnapshotCompression(),
                         "large snapshot compression")
               ? 0
               : 1;
}
