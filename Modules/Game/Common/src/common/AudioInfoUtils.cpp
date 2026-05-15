#include "common/AudioInfoUtils.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>

#ifdef _WIN32
#    define pclose _pclose
#    define popen _popen
#endif

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

    std::string command =
        "ffprobe -v quiet -print_format json -show_format -show_streams \"" +
        Config::pathToUtf8(filePath) + "\"";

    std::array<char, 128>                    buffer;
    std::string                              result;
#ifdef _WIN32
    std::wstring wcommand = Config::utf8ToPath(command).wstring();
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_wpopen(wcommand.c_str(), L"r"),
                                                  _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"),
                                                  pclose);
#endif

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

            if ( format.contains("duration") ) {
                info.duration =
                    std::stod(format["duration"].get<std::string>());
            }

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
    } catch ( const std::exception& e ) {
        XERROR("AudioInfoUtils: Failed to parse ffprobe output for {}: {}",
               Config::pathToUtf8(filePath),
               e.what());
        return std::nullopt;
    }
}

}  // namespace MMM::Utils
