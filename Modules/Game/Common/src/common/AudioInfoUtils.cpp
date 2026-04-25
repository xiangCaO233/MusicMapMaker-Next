#include "common/AudioInfoUtils.h"
#include "log/colorful-log.h"
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace MMM::Utils
{

std::optional<AudioInfo> AudioInfoUtils::probeAudioInfo(
    const std::filesystem::path& filePath)
{
    if ( !std::filesystem::exists(filePath) ) {
        XERROR("AudioInfoUtils: File not found: {}", filePath.string());
        return std::nullopt;
    }

    std::string command =
        "ffprobe -v quiet -print_format json -show_format -show_streams \"" +
        filePath.string() + "\"";

    std::array<char, 128>                    buffer;
    std::string                              result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"),
                                                  pclose);

    if ( !pipe ) {
        XERROR("AudioInfoUtils: popen() failed for command: {}", command);
        return std::nullopt;
    }

    while ( fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr ) {
        result += buffer.data();
    }

    try {
        auto      j = nlohmann::json::parse(result);
        AudioInfo info;

        if ( j.contains("format") ) {
            auto& format = j["format"];

            // 获取时长
            if ( format.contains("duration") ) {
                info.duration =
                    std::stod(format["duration"].get<std::string>());
            }

            // 获取元数据
            if ( format.contains("tags") ) {
                auto& tags = format["tags"];
                if ( tags.contains("title") ) {
                    info.title = tags["title"].get<std::string>();
                }
                if ( tags.contains("artist") ) {
                    info.artist = tags["artist"].get<std::string>();
                }
            }
        }

        // 如果没有 Title，使用文件名
        if ( info.title.empty() ) {
            info.title = filePath.stem().string();
        }

        XINFO(
            "AudioInfoUtils: Probed info for {}: Title={}, Artist={}, "
            "Duration={:.2f}s",
            filePath.filename().string(),
            info.title,
            info.artist,
            info.duration);

        return info;
    } catch ( const std::exception& e ) {
        XERROR("AudioInfoUtils: Failed to parse ffprobe output for {}: {}",
               filePath.string(),
               e.what());
        return std::nullopt;
    }
}

}  // namespace MMM::Utils
