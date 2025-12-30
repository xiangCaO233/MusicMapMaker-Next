#include "TestAudioBackend.h"

namespace Audio
{
std::weak_ptr<ice::AudioTrack> getaudio(std::string_view file)
{
    return apool.get_or_load(tpool, file);
};
}  // namespace Audio
