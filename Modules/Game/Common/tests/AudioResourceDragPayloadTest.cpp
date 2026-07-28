#include "common/AudioResourceDragPayload.h"
#include "mmm/project/AudioResource.h"

#include <string>

int main()
{
    using namespace MMM;
    using namespace MMM::Common;

    const auto payload =
        makeAudioResourceDragPayload("main-track.ogg", AudioTrackType::Main);
    if ( !payload || audioResourceIdView(*payload) != "main-track.ogg" ||
         payload->m_audioTrackType != AudioTrackType::Main ) {
        return 1;
    }

    const std::string oversizedId(AUDIO_RESOURCE_DRAG_ID_CAPACITY + 32U, 'x');
    if ( makeAudioResourceDragPayload(oversizedId, AudioTrackType::Effect) ||
         makeAudioResourceDragPayload("", AudioTrackType::Effect) ) {
        return 2;
    }

    return 0;
}
