#ifndef MMM_AUDIO_H
#define MMM_AUDIO_H

#include <ice/manage/AudioPool.hpp>
#include <string_view>

namespace Audio
{
inline ice::ThreadPool         tpool;
inline ice::AudioPool          apool;
std::weak_ptr<ice::AudioTrack> getaudio(std::string_view file);
}  // namespace Audio

#endif  // MMM_AUDIO_H
