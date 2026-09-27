#include "logic/MczAudioOriginAlignment.h"

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <string>
#include <unordered_set>

// 本组直接构造内存谱面，验证导出前的时间域变换，不读取或编码音频文件。
// 所有模型时间均为毫秒，不能把音频播放接口的秒单位带入这些断言。
namespace
{

/// @brief 测试时间比较容差。
/// @note 容差单位也是毫秒，不用于容忍整毫秒级的偏移错误。
constexpr double TIME_EPSILON_MS = 1.0e-6;

/// @brief 比较两个毫秒时间是否一致。
/// @param lhs 实际时间。
/// @param rhs 预期时间。
/// @return 绝对误差不超过模型时间容差时为 true。
bool nearTime(double lhs, double rhs)
{
    // 使用同一容差比较相位和变换后时间，避免两阶段采用不同的边界口径。
    return std::abs(lhs - rhs) <= TIME_EPSILON_MS;
}

/// @brief 创建含主采样、普通采样、后续红线和批注的测试谱面。
/// @param firstBpmTime 首红线的毫秒时间，用于改变整拍前导和拍内相位。
/// @return 时间间隔固定、只改变首红线位置的独立谱面。
/// @note 折线仅设置父级锚点，本夹具不覆盖折线子节点的对齐行为。
MMM::BeatMap makeBeatMap(double firstBpmTime)
{
    // 首 BPM 的 120 对应 500 ms 一拍，方便区分整拍数量与剩余相位。
    MMM::BeatMap beatMap;
    MMM::Timing  firstBpm;
    firstBpm.m_timestamp             = firstBpmTime;
    firstBpm.m_bpm                   = 120.0;
    firstBpm.m_beat_length           = 500.0;
    firstBpm.m_timingEffect          = MMM::TimingEffect::BPM;
    firstBpm.m_timingEffectParameter = 120.0;
    // BPM 数值、拍长和效果参数一致，避免测试夹具自身出现多字段冲突。
    // 保留旧 Malody delay，验证归零后不会让格式元数据再次引入旧偏移。
    firstBpm.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["delay"] = "123.0";
    beatMap.m_timings.push_back(firstBpm);

    // 第二条红线改为 150 BPM，确认相位仍由首 BPM 决定，而非采用后续拍长。
    // 该红线只应整体平移，不应和首红线一起被强制归零。
    MMM::Timing secondBpm             = firstBpm;
    secondBpm.m_timestamp             = 2100.0;
    secondBpm.m_bpm                   = 150.0;
    secondBpm.m_beat_length           = 400.0;
    secondBpm.m_timingEffectParameter = 150.0;
    // 复制得到的后续元数据不会在本例中断言；delay 删除检查仅针对首红线。
    beatMap.m_timings.push_back(secondBpm);

    // 覆盖不同持久化物件容器，普通音符与第二条红线同刻以检查相对关系。
    auto& note       = beatMap.m_noteData.notes.emplace_back();
    note.m_timestamp = 2100.0;
    note.m_track     = 1U;
    // 长条尾部与普通音符同刻；平移应保留时长，而不是缩放或裁短长条。
    auto& hold        = beatMap.m_noteData.holds.emplace_back();
    hold.m_timestamp  = 1800.0;
    hold.m_duration   = 300.0;
    hold.m_track      = 2U;
    auto& flick       = beatMap.m_noteData.flicks.emplace_back();
    flick.m_timestamp = 2300.0;
    flick.m_track     = 3U;
    // 不配置滑键横向跨度，本场景只验证其时间锚点的平移。
    auto& polyline       = beatMap.m_noteData.polylines.emplace_back();
    polyline.m_timestamp = 2500.0;
    polyline.m_track     = 4U;
    // 物件轨道固定，保证所有对齐差异都来自时间变换而非横向布局。

    // 主音频资源 ID 由测试显式指定；锚点与局部偏移都为零，满足对齐前提。
    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 0.0,
        .m_offsetMs        = 0,
        .m_track           = 4U,
        .m_audioResourceId = "main-audio",
    });
    // 资源引用集合稍后只包含 main-audio，效果音不能误被识别成配对主音频。
    // 普通效果音有独立负偏移，变换只应移动锚点，不能再额外修改偏移字段。
    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 300.0,
        .m_offsetMs        = -20,
        .m_track           = 5U,
        .m_audioResourceId = "effect-audio",
    });
    // 批注也属于谱面时间域，需要随物件移动，而不是留在原始音频坐标中。
    beatMap.m_annotations.push_back(MMM::BeatmapAnnotation{
        .m_timestamp = 700.0,
        .m_content   = "alignment",
    });
    // 在数据准备完毕后同步模型，使用与正常谱面操作一致的派生索引状态。
    beatMap.sync();
    return beatMap;
}

/// @brief 验证正时间多拍前导只裁切一拍内相位。
/// @return 相位、首红线归零及各类事件平移均符合预期时为 true。
/// @note 不验证真实音频裁切结果；这里只检查与裁切相位配套的模型改写。
bool checkPositiveMultiBeatAlignment()
{
    // 1600 = 3×500 + 100，不能把全部 1600 ms 作为音频裁头量。
    auto beatMap = makeBeatMap(1600.0);
    // 每个场景独立构造模型，避免重复应用相位污染后续用例。
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    // 先核对计算结果再调用变换，避免用错误相位得到另一组自洽但不正确的结果。
    const bool applied =
        timing.success && nearTime(timing.phaseMilliseconds, 100.0) &&
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    // 首红线特例归零，其余时间减去 100 ms；主音频仍固定在原点。
    // 成功还应清空错误文本并删除首红线 delay，避免导出时重复应用偏移。
    const bool valid =
        applied && error.empty() &&
        // 成功应用不增删事件，后续断言沿用夹具中的固定容器索引。
        nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
        !beatMap.m_timings[0]
             .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY]
             .contains("delay") &&
        nearTime(beatMap.m_timings[1].m_timestamp, 2000.0) &&
        nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2000.0) &&
        nearTime(beatMap.m_noteData.holds[0].m_timestamp, 1700.0) &&
        // 时间平移不改变相对时长，长条尾部应随起点一起移动。
        nearTime(beatMap.m_noteData.holds[0].m_duration, 300.0) &&
        nearTime(beatMap.m_noteData.flicks[0].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_noteData.polylines[0].m_timestamp, 2400.0) &&
        nearTime(beatMap.m_audioSamples[0].effectiveTimestamp(), 0.0) &&
        nearTime(beatMap.m_audioSamples[1].m_timestamp, 200.0) &&
        // 同时检查锚点和偏移，比只比较有效播放时间更能发现两字段相互抵消的错误。
        // 普通采样的 -20 ms 保留，不能把全局相位同时写入局部偏移。
        beatMap.m_audioSamples[1].m_offsetMs == -20 &&
        nearTime(beatMap.m_annotations[0].m_timestamp, 600.0);
    if ( !valid )
        XERROR("Positive multi-beat MCZ origin alignment failed: {}", error);
    return valid;
}

/// @brief 验证负时间多拍前导折回后通过补静音相位统一平移。
/// @return 保留负相位符号且事件正确后移时为 true。
/// @note 这里只验证补静音对应的时间方向，不执行音频处理。
bool checkNegativeMultiBeatAlignment()
{
    // -600 对 500 取有符号余量得到 -100，而不是正向折回的 400。
    auto beatMap = makeBeatMap(-600.0);
    // 后续红线仍为 2100 ms，不随首红线参数一同移动；预期值因此可独立计算。
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    const bool  applied =
        timing.success && nearTime(timing.phaseMilliseconds, -100.0) &&
        // 将实际计算出的负相位传入应用阶段，验证两接口的符号约定一致。
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    // 减去负相位意味着统一增加 100 ms，首红线与主音频另行保持零点。
    const bool valid =
        applied && nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
        nearTime(beatMap.m_timings[1].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_audioSamples[0].effectiveTimestamp(), 0.0) &&
        nearTime(beatMap.m_audioSamples[1].m_timestamp, 400.0) &&
        nearTime(beatMap.m_annotations[0].m_timestamp, 800.0);
    if ( !valid )
        XERROR("Negative multi-beat MCZ origin alignment failed: {}", error);
    return valid;
}

/// @brief 验证整数拍首红线归零时不重编码相位或移动普通内容。
/// @return 零相位仍可成功应用且普通事件不移动时为 true。
/// @note 不检查编码器是否被调用，只证明模型给出零相位而非失败结果。
bool checkWholeBeatAlignment()
{
    // 1500 恰好为三拍，移除整数拍前导不需要改变音频的拍内起点。
    auto beatMap = makeBeatMap(1500.0);
    // 采用精确整拍而非接近边界的浮点数，本例不覆盖容差吸附行为。
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    const bool  applied =
        timing.success && nearTime(timing.phaseMilliseconds, 0.0) &&
        // 仍显式调用应用接口，防止调用方把零相位误解为无需归零首红线。
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    // 零相位不代表不做任何操作：首红线仍应归零，其他时间保持原值。
    return applied && nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
           nearTime(beatMap.m_timings[1].m_timestamp, 2100.0) &&
           nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2100.0) &&
           nearTime(beatMap.m_audioSamples[1].m_timestamp, 300.0);
}

/// @brief 验证缺少 BPM 或 Main 自动采样时安全失败。
/// @return 缺失、偏离原点和重复主采样均被拒绝并提供应用错误时为 true。
/// @note 本例检查失败结果和诊断，不通过全模型对比证明失败时的无修改保证。
bool checkInvalidInputsFail()
{
    // 空谱面没有拍长依据，计算阶段应明确失败，不能使用默认 BPM 掩盖缺失。
    MMM::BeatMap emptyBeatMap;
    const auto   missingTiming =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(emptyBeatMap);
    // 这里只检查计算结果的 success，未把其错误消息格式当作接口契约。
    // 另用完整谱面测试资源 ID 不匹配，分开覆盖计算阶段与应用阶段的拒绝入口。
    auto        beatMap = makeBeatMap(100.0);
    std::string error;
    if ( missingTiming.success ||
         // 显式使用不存在的资源 ID，而不是空集合，覆盖目标查找失败分支。
         MMM::Logic::applyMczAudioOriginAlignment(
             beatMap,
             std::unordered_set<std::string>{ "missing-main" },
             100.0,
             error) ||
         error.empty() ) {
        return false;
    }

    // ID 正确但主采样晚于原点，不能套用原点裁切方案。
    // 局部偏移保持零，确保有效时间也确实为 10 ms。
    auto nonOriginMain                          = makeBeatMap(100.0);
    nonOriginMain.m_audioSamples[0].m_timestamp = 10.0;
    // 拒绝条件依据有效触发时间，不是文件名或轨道编号。
    // 各失败用例独立观察错误输出，不能让前一用例的诊断满足非空断言。
    error.clear();
    if ( MMM::Logic::applyMczAudioOriginAlignment(
             nonOriginMain,
             std::unordered_set<std::string>{ "main-audio" },
             100.0,
             error) ||
         error.empty() ) {
        return false;
    }

    // 即使已有一个合法零点采样，同一目标音频再次触发也必须拒绝单一对齐方案。
    // 第二次触发放在 500 ms，避免仅验证两个完全相同事件的特殊重复情况。
    auto duplicateMain = makeBeatMap(100.0);
    duplicateMain.m_audioSamples.push_back(MMM::AudioSampleEvent{
        // 与原有主采样共享资源引用，但保留不同触发时间。
        .m_timestamp       = 500.0,
        .m_offsetMs        = 0,
        .m_track           = 4U,
        .m_audioResourceId = "main-audio",
    });
    error.clear();
    // 重新清空诊断，确保非空错误确实由重复主采样检查产生。
    // 同时要求 false 和错误文本，不能把未说明原因的失败当作完整诊断。
    return !MMM::Logic::applyMczAudioOriginAlignment(
               duplicateMain,
               std::unordered_set<std::string>{ "main-audio" },
               100.0,
               error) &&
           !error.empty();
}

}  // namespace

/// @brief 运行 MCZ 导出原点对齐的模型回归场景。
/// @return 全部通过时为零，否则为一。
/// @note 无外部资源参数，不覆盖谱包打包、文件路径替换或实际听感。
/// @note 不启动播放线程；模型变换结果由返回值和字段断言直接验证。
int main()
{
    // 初始化项目日志以输出相位或平移断言失败的上下文。
    XLogger::init("MczAudioOriginAlignmentTest");
    // 错误日志不代替失败状态，调用者仍以进程返回码判断结果。
    // 正相位、负相位和零相位分别覆盖三个时间方向，再检查拒绝条件。
    // 短路保留首个失败，单次返回码不表示后续用例一定执行过。
    return checkPositiveMultiBeatAlignment() &&
                   checkNegativeMultiBeatAlignment() &&
                   checkWholeBeatAlignment() && checkInvalidInputsFail()
               ? 0
               : 1;
}
