#include "logic/EditorClipboardProtocol.h"

#include "logic/EditorClipboard.h"
#include "logic/session/context/SessionContext.h"

#include "log/colorful-log.h"
#include <cmath>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace
{
using MMM::Logic::ClipboardItem;
using MMM::Logic::SampleClipboardItem;
using MMM::Logic::TimelineClipboardItem;

/// @brief 使用小容差比较有限浮点数。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 比较两个可选采样绑定的资源与物件音量。
bool sameBinding(const std::optional<MMM::AudioSampleBinding>& lhs,
                 const std::optional<MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) {
        return false;
    }
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    near(lhs->m_volume, rhs->m_volume));
}

/// @brief 以满足协议往返验证的精度比较可选颜色。
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

/// @brief 不抛异常地读取一个音符元数据值。
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

/// @brief 不抛异常地读取一个时间线元数据值。
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

/// @brief 不抛异常地读取一个自动采样元数据值。
std::optional<std::string> sampleMetadataValue(
    const MMM::SampleMetadata& metadata, MMM::SampleMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.sample_properties.find(source);
    if ( sourceIt == metadata.sample_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief 验证音符剪贴板载荷的序列化和解析。
bool testNoteRoundTrip()
{
    ClipboardItem item;
    item.note.m_type       = MMM::NoteType::POLYLINE;
    item.note.m_timestamp  = 12.5;
    item.note.m_duration   = 1.25;
    item.note.m_trackIndex = 2;
    item.note.m_dtrack     = 1;
    item.note.m_annotation = "整条折线\n待复核";
    item.note.m_sampleBinding =
        MMM::AudioSampleBinding{ "main\tbound.wav", 0.35F };
    item.note.m_metadata
        .note_properties[MMM::NoteMetadataType::MMM]["authorNote"] =
        "copy\tline\n%";
    item.note.m_customColors.tap = glm::vec4{ 0.1F, 0.2F, 0.3F, 1.0F };

    MMM::Logic::NoteComponent::SubNote hold;
    hold.type          = MMM::NoteType::HOLD;
    hold.timestamp     = 13.0;
    hold.duration      = 0.75;
    hold.trackIndex    = 3;
    hold.dtrack        = 0;
    hold.annotation    = "起段\t备注";
    hold.sampleBinding = MMM::AudioSampleBinding{ "hold.wav", 0.45F };
    hold.metadata.note_properties[MMM::NoteMetadataType::OSU]["edge"] = "hold";
    hold.customColors.head = glm::vec4{ 0.4F, 0.5F, 0.6F, 1.0F };

    MMM::Logic::NoteComponent::SubNote flick;
    flick.type          = MMM::NoteType::FLICK;
    flick.timestamp     = 14.0;
    flick.duration      = 0.0;
    flick.trackIndex    = 5;
    flick.dtrack        = -1;
    flick.sampleBinding = MMM::AudioSampleBinding{ "flick.wav", 0.55F };
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
    if ( !parsed || parsed->notes.size() != 1 || !parsed->samples.empty() ||
         !parsed->timelines.empty() ) {
        XERROR("Note clipboard protocol did not parse one note item");
        return false;
    }

    const auto& parsedItem = parsed->notes.front();
    const auto& note       = parsedItem.note;
    if ( note.m_type != MMM::NoteType::POLYLINE ||
         !near(note.m_timestamp, 12.5) || !near(note.m_duration, 1.25) ||
         note.m_trackIndex != 2 || note.m_dtrack != 1 ||
         note.m_annotation != "整条折线\n待复核" ||
         !sameBinding(note.m_sampleBinding, item.note.m_sampleBinding) ||
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
         note.m_subNotes[0].annotation != "起段\t备注" ||
         !sameBinding(note.m_subNotes[0].sampleBinding, hold.sampleBinding) ||
         !sameColor(note.m_subNotes[0].customColors.head,
                    hold.customColors.head) ) {
        XERROR("Note clipboard protocol changed first sub note");
        return false;
    }
    if ( note.m_subNotes[1].type != MMM::NoteType::FLICK ||
         note.m_subNotes[1].dtrack != -1 ||
         !sameBinding(note.m_subNotes[1].sampleBinding, flick.sampleBinding) ||
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

/// @brief 验证 V4 混合谱面物件载荷保留自动采样关键字段和相对 BGM 轨道。
bool testMixedChartObjectRoundTrip()
{
    ClipboardItem note;
    note.note.m_timestamp  = 12.5;
    note.note.m_trackIndex = 2;
    note.startBeat         = 25.0;
    note.hasBeatPositions  = true;

    SampleClipboardItem sample;
    sample.sample.m_timestamp       = 10.25;
    sample.sample.m_offsetMs        = -125;
    sample.sample.m_track           = 7;
    sample.sample.m_audioResourceId = "stem\tlayer.ogg";
    sample.sample.m_volume          = 0.65F;
    sample.sample.m_metadata
        .sample_properties[MMM::SampleMetadataType::MALODY]["original_x"] =
        "11";
    sample.bgmLane         = 7;
    sample.startBeat       = 20.5;
    sample.hasBeatPosition = true;

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeChartObjects({ note },
                                                                   { sample });
    if ( !text.starts_with("MMM_CLIPBOARD_V4\tC\n") ) {
        XERROR("Mixed chart-object clipboard did not emit a V4 payload");
        return false;
    }

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text);
    if ( !parsed || parsed->notes.size() != 1 || parsed->samples.size() != 1 ||
         !parsed->timelines.empty() ) {
        XERROR("Mixed chart-object clipboard did not parse both object kinds");
        return false;
    }

    const auto& parsedSample = parsed->samples.front();
    const auto  originalX = sampleMetadataValue(parsedSample.sample.m_metadata,
                                                MMM::SampleMetadataType::MALODY,
                                                "original_x");
    if ( !near(parsedSample.sample.m_timestamp, 10.25) ||
         parsedSample.sample.m_offsetMs != -125 ||
         parsedSample.sample.m_track != 7 || parsedSample.bgmLane != 7 ||
         parsedSample.sample.m_audioResourceId != "stem\tlayer.ogg" ||
         !near(parsedSample.sample.m_volume, sample.sample.m_volume) ||
         !near(parsedSample.startBeat, 20.5) || !parsedSample.hasBeatPosition ||
         !originalX || *originalX != "11" ) {
        XERROR(
            "Mixed chart-object clipboard changed automatic sample fields: "
            "timestamp={}, offset={}, track={}, lane={}, resource='{}', "
            "volume={}, beat={}, hasBeat={}, originalX='{}'",
            parsedSample.sample.m_timestamp,
            parsedSample.sample.m_offsetMs,
            parsedSample.sample.m_track,
            parsedSample.bgmLane,
            parsedSample.sample.m_audioResourceId,
            parsedSample.sample.m_volume,
            parsedSample.startBeat,
            parsedSample.hasBeatPosition,
            originalX.value_or("<missing>"));
        return false;
    }
    return true;
}

/// @brief 验证 V3 音符绑定音量载荷仍可读取。
bool testLegacyV3Payload()
{
    constexpr std::string_view notePayload =
        "MMM_CLIPBOARD_V3\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tlegacy-v3.wav\t0.375\n";

    const auto notes = MMM::Logic::EditorClipboardProtocol::parse(notePayload);
    if ( !notes || notes->notes.size() != 1 || !notes->samples.empty() ||
         !notes->notes.front().note.m_sampleBinding ||
         notes->notes.front().note.m_sampleBinding->m_audioResourceId !=
             "legacy-v3.wav" ||
         !near(notes->notes.front().note.m_sampleBinding->m_volume, 0.375) ) {
        XERROR("Legacy V3 note clipboard payload was rejected");
        return false;
    }

    constexpr std::string_view timelinePayload =
        "MMM_CLIPBOARD_V3\tT\n"
        "T\t4\tb\t150\t0\t0\t1\n";
    const auto timelines =
        MMM::Logic::EditorClipboardProtocol::parse(timelinePayload);
    if ( !timelines || timelines->timelines.size() != 1 ||
         !near(timelines->timelines.front().timeline.m_timestamp, 4.0) ||
         !near(timelines->timelines.front().timeline.m_value, 150.0) ) {
        XERROR("Legacy V3 timeline clipboard payload was rejected");
        return false;
    }
    return true;
}

/// @brief 验证 V2 采样资源行可读取并为缺失的物件音量补 1。
bool testLegacyV2SampleBindingDefaultsVolume()
{
    constexpr std::string_view payload =
        "MMM_CLIPBOARD_V2\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tlegacy%09main.wav\n"
        "S\th\t2\t0.5\t3\t0\n"
        "SS\tlegacy-hold.wav\n";

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(payload);
    if ( !parsed || parsed->notes.size() != 1 ) {
        XERROR("Legacy V2 note clipboard payload was rejected");
        return false;
    }

    const auto& note = parsed->notes.front().note;
    if ( !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId != "legacy\tmain.wav" ||
         !near(note.m_sampleBinding->m_volume, 1.0) ||
         note.m_subNotes.size() != 1 ||
         !note.m_subNotes.front().sampleBinding ||
         note.m_subNotes.front().sampleBinding->m_audioResourceId !=
             "legacy-hold.wav" ||
         !near(note.m_subNotes.front().sampleBinding->m_volume, 1.0) ) {
        XERROR("Legacy V2 sample binding did not default volume to one");
        return false;
    }
    return true;
}

/// @brief 验证超出 float 范围的绑定音量不会产生无穷值。
bool testOutOfRangeSampleBindingVolumeRejected()
{
    constexpr std::string_view payload =
        "MMM_CLIPBOARD_V4\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tmain.wav\t1e100\n"
        "S\th\t2\t0.5\t3\t0\n"
        "SS\thold.wav\t-1e100\n";

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(payload);
    if ( !parsed || parsed->notes.size() != 1 ||
         parsed->notes.front().note.m_sampleBinding ||
         parsed->notes.front().note.m_subNotes.size() != 1 ||
         parsed->notes.front().note.m_subNotes.front().sampleBinding ) {
        XERROR("Clipboard protocol accepted an out-of-range binding volume");
        return false;
    }
    return true;
}

/// @brief 验证时间线剪贴板载荷的序列化和解析。
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
    if ( !parsed || parsed->timelines.size() != 1 || !parsed->notes.empty() ||
         !parsed->samples.empty() ) {
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

/// @brief 验证 MMM 剪贴板解析器会忽略普通文本。
bool testPlainTextIgnored()
{
    if ( MMM::Logic::EditorClipboardProtocol::parse("plain text") ) {
        XERROR("Clipboard protocol parser accepted plain text");
        return false;
    }
    return true;
}

/// @brief 验证协作剪贴板不会导出到系统或跨 Session 泄漏。
bool testCollaborationClipboardIsolation()
{
    MMM::Logic::EditorClipboard clipboard;
    MMM::Logic::SessionContext  localSource;
    MMM::Logic::SessionContext  localTarget;
    ClipboardItem               item;
    item.note.m_timestamp = 12.5;

    clipboard.set({ item }, &localSource, false);
    if ( clipboard.get(&localTarget).size() != 1U ||
         !clipboard.consumePendingSystemText() ) {
        XERROR("Local clipboard no longer crosses ordinary sessions");
        return false;
    }

    MMM::Logic::SessionContext collaborationSource;
    collaborationSource.collaborationClipboardIsolated = true;
    collaborationSource.collaborationClipboardScopeId  = 41U;
    MMM::Logic::SessionContext otherCollaborationSession;
    otherCollaborationSession.collaborationClipboardIsolated = true;
    otherCollaborationSession.collaborationClipboardScopeId  = 41U;
    clipboard.set({ item }, &collaborationSource, true);
    if ( clipboard.consumePendingSystemText() ||
         clipboard.get(&collaborationSource).size() != 1U ||
         !clipboard.get(&localTarget).empty() ||
         !clipboard.get(&otherCollaborationSession).empty() ||
         !clipboard.isCutFrom(&collaborationSource) ||
         clipboard.getCrossSessionCutSource(&localTarget) ) {
        XERROR("Collaboration clipboard escaped its source session");
        return false;
    }

    clipboard.clearForContext(&collaborationSource);
    if ( !clipboard.get(&collaborationSource).empty() ) {
        XERROR("Collaboration clipboard survived session cleanup");
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    if ( !testNoteRoundTrip() ) return 1;
    if ( !testMixedChartObjectRoundTrip() ) return 1;
    if ( !testLegacyV3Payload() ) return 1;
    if ( !testLegacyV2SampleBindingDefaultsVolume() ) return 1;
    if ( !testOutOfRangeSampleBindingVolumeRejected() ) return 1;
    if ( !testTimelineRoundTrip() ) return 1;
    if ( !testPlainTextIgnored() ) return 1;
    if ( !testCollaborationClipboardIsolation() ) return 1;
    return 0;
}
