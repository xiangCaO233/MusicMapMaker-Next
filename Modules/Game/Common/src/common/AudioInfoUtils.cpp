#include "common/AudioInfoUtils.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <ice/manage/dec/MediaInfo.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>

namespace MMM::Utils
{

std::optional<AudioInfo> AudioInfoUtils::probeAudioInfo(
    const std::filesystem::path& filePath)
{
    if ( !std::filesystem::exists(filePath) ) {
        XERROR("AudioInfoUtils: File not found: {}",
               Config::pathToUtf8(filePath));
        return std::nullopt;
    }

    ice::FFmpegDecoderFactory decoderFactory;
    ice::MediaInfo            mediaInfo;
    const std::string         audioPath = Config::pathToUtf8(filePath);
    if ( !decoderFactory.probe(audioPath, mediaInfo) ) {
        XERROR("AudioInfoUtils: Failed to probe audio metadata for {}",
               audioPath);
        return std::nullopt;
    }

    AudioInfo info;
    info.title  = mediaInfo.title;
    info.artist = mediaInfo.artist;
    if ( mediaInfo.format.samplerate > 0 ) {
        info.duration = static_cast<double>(mediaInfo.frame_count) /
                        static_cast<double>(mediaInfo.format.samplerate);
    }

    if ( info.title.empty() ) {
        info.title = Config::pathToUtf8(filePath.stem());
    }

    XINFO(
        "AudioInfoUtils: Probed info for {}: Title={}, Artist={}, "
        "Duration={:.2f}s",
        Config::pathToUtf8(filePath.filename()),
        info.title,
        info.artist,
        info.duration);

    return info;
}

}  // namespace MMM::Utils
