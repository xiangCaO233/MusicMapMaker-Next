#include "common/BeatmapAudioTimelineCompatibility.h"

#include <string>

/// @brief 覆盖单音频时间线兼容判定的成功路径和全部拒绝原因。
/// @return 所有断言通过时返回零，否则返回对应场景编号。
int main()
{
    using namespace MMM;
    using namespace MMM::Common;

    // 准备一个可被自动采样稳定 ID 解析的主音频资源。
    Project project;
    project.m_audioResources.push_back(AudioResource{
        // ID 与路径相同便于断言，类型保持真实主音频语义。
        .m_id   = "main.ogg",
        .m_path = "main.ogg",
        .m_type = AudioTrackType::Main,
    });

    // 空时间线没有可交给单文件流程的音频来源。
    BeatMap beatmap;
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::MissingSample ) {
        // 场景一验证缺失采样不会被误判为兼容。
        return 1;
    }

    // 唯一采样从谱面零点和资源零偏移开始，构成可兼容基线。
    beatmap.m_audioSamples.push_back(AudioSampleEvent{
        // 时间戳和源偏移同时为零是单文件兼容的核心条件。
        .m_timestamp = 0.0,
        .m_offsetMs  = 0,
        // 轨道编号不影响兼容性，使用非默认值证明该字段被忽略。
        .m_track           = 4U,
        .m_audioResourceId = "main.ogg",
    });
    // 成功结果必须同时提供真值状态和已解析的项目资源。
    auto compatible = resolveSingleZeroPointAudioTimeline(beatmap, project);
    if ( !compatible || compatible.m_resource->m_id != "main.ogg" ) {
        // 场景二验证成功载荷与资源观察指针。
        return 2;
    }

    // 即使谱面时间戳为零，源内负偏移仍无法表示为原始单文件。
    beatmap.m_audioSamples.front().m_offsetMs = -1;
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::NonZeroStart ) {
        // 场景三验证时间轴与源偏移必须同时为零。
        return 3;
    }

    // 恢复零偏移并加入第二条采样，构造复合音频时间线。
    beatmap.m_audioSamples.front().m_offsetMs = 0;
    beatmap.m_audioSamples.push_back(AudioSampleEvent{
        // 第二条采样使用非零时间戳，明确构造独立播放节点。
        .m_timestamp = 1000.0,
        // 未指定 offsetMs 时沿用模型默认值，不影响数量优先判定。
        .m_track           = 5U,
        .m_audioResourceId = "main.ogg",
    });
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::CompositeTimeline ) {
        // 场景四验证多采样不会被静默选择其中一条。
        return 4;
    }

    // 移除额外采样，使最后场景只验证资源解析失败而非数量失败。
    beatmap.m_audioSamples.pop_back();
    beatmap.m_audioSamples.front().m_audioResourceId = "missing.ogg";
    // 项目资源表保持不变，因此 missing.ogg 必然无法按稳定 ID 解析。
    // 该场景也验证失败结果不会残留此前成功解析得到的观察指针。
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::MissingResource ) {
        // 场景五验证悬空资源引用不会返回部分成功结果。
        return 5;
    }

    // 全部兼容性分支符合预期。
    return 0;
}
