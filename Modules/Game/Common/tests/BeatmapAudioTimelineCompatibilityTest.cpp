#include "common/BeatmapAudioTimelineCompatibility.h"

#include <string>

int main()
{
    using namespace MMM;
    using namespace MMM::Common;

    Project project;
    project.m_audioResources.push_back(AudioResource{
        .m_id   = "main.ogg",
        .m_path = "main.ogg",
        .m_type = AudioTrackType::Main,
    });

    BeatMap beatmap;
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::MissingSample ) {
        return 1;
    }

    beatmap.m_audioSamples.push_back(AudioSampleEvent{
        .m_timestamp       = 0.0,
        .m_offsetMs        = 0,
        .m_track           = 4U,
        .m_audioResourceId = "main.ogg",
    });
    auto compatible = resolveSingleZeroPointAudioTimeline(beatmap, project);
    if ( !compatible || compatible.m_resource->m_id != "main.ogg" ) {
        return 2;
    }

    beatmap.m_audioSamples.front().m_offsetMs = -1;
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::NonZeroStart ) {
        return 3;
    }

    beatmap.m_audioSamples.front().m_offsetMs = 0;
    beatmap.m_audioSamples.push_back(AudioSampleEvent{
        .m_timestamp       = 1000.0,
        .m_track           = 5U,
        .m_audioResourceId = "main.ogg",
    });
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::CompositeTimeline ) {
        return 4;
    }

    beatmap.m_audioSamples.pop_back();
    beatmap.m_audioSamples.front().m_audioResourceId = "missing.ogg";
    if ( resolveSingleZeroPointAudioTimeline(beatmap, project).m_issue !=
         SingleAudioTimelineIssue::MissingResource ) {
        return 5;
    }

    return 0;
}
