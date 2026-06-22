#include "audio/AudioSpeedExportService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// @brief 写入 16 位小端整数。
/// @param file 输出文件流。
/// @param value 要写入的值。
void writeU16(std::ofstream& file, std::uint16_t value)
{
    const char bytes[2] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
    };
    file.write(bytes, 2);
}

/// @brief 写入 32 位小端整数。
/// @param file 输出文件流。
/// @param value 要写入的值。
void writeU32(std::ofstream& file, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu),
    };
    file.write(bytes, 4);
}

/// @brief 从字节数组读取 32 位小端整数。
/// @param bytes 输入字节。
/// @param offset 偏移。
/// @return 读取出的值。
std::uint32_t readU32(const std::vector<unsigned char>& bytes,
                      std::size_t                       offset)
{
    if ( offset + 4 > bytes.size() ) return 0;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

/// @brief WAV chunk 位置。
struct WavChunkInfo {
    /// @brief chunk 数据起点。
    std::size_t offset{ 0 };

    /// @brief chunk 数据字节数。
    std::uint32_t size{ 0 };
};

/// @brief 查找 WAV RIFF chunk。
/// @param bytes 文件字节。
/// @param chunkId 四字节 chunk id。
/// @return 找到时返回 chunk 位置。
std::optional<WavChunkInfo> findWavChunk(
    const std::vector<unsigned char>& bytes, std::string_view chunkId)
{
    if ( bytes.size() < 12 || chunkId.size() != 4 ) {
        return std::nullopt;
    }
    if ( std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) !=
             "RIFF" ||
         std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4) !=
             "WAVE" ) {
        return std::nullopt;
    }

    std::size_t offset = 12;
    while ( offset + 8 <= bytes.size() ) {
        const std::string_view currentId(
            reinterpret_cast<const char*>(bytes.data() + offset), 4);
        const std::uint32_t chunkSize  = readU32(bytes, offset + 4);
        const std::size_t   dataOffset = offset + 8;
        if ( dataOffset + chunkSize > bytes.size() ) {
            return std::nullopt;
        }
        if ( currentId == chunkId ) {
            return WavChunkInfo{ dataOffset, chunkSize };
        }
        offset = dataOffset + chunkSize + (chunkSize & 1u);
    }

    return std::nullopt;
}

/// @brief 读取 WAV 数据 chunk 字节数。
/// @param bytes 文件字节。
/// @return data chunk 字节数。
std::optional<std::uint32_t> readWavDataBytes(
    const std::vector<unsigned char>& bytes)
{
    const auto dataChunk = findWavChunk(bytes, "data");
    if ( !dataChunk ) {
        return std::nullopt;
    }
    return dataChunk->size;
}

/// @brief 输出测试断言。
/// @param condition 断言条件。
/// @param label 断言名称。
/// @return 条件是否成立。
bool check(bool condition, const std::string& label)
{
    if ( condition ) {
        XINFO("[audio-speed-export] PASS: {}", label);
    } else {
        XERROR("[audio-speed-export] FAIL: {}", label);
    }
    return condition;
}

/// @brief 浮点近似比较。
/// @param actual 实际值。
/// @param expected 期望值。
/// @return 足够接近时返回 true。
bool isNearlyEqual(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-6;
}

/// @brief 计算带少量编码器延迟容忍的最低读回帧数。
/// @param expectedFrames 期望输出帧数。
/// @return 最低可接受读回帧数。
std::size_t minimumDecodedFrames(std::size_t expectedFrames)
{
    return expectedFrames > 100 ? expectedFrames * 95 / 100 : expectedFrames;
}

/// @brief 创建测试用立体声 WAV。
/// @param path 输出路径。
/// @param frames 帧数。
/// @param sampleRate 采样率。
/// @return 是否创建成功。
bool writeFixtureWav(const std::filesystem::path& path, std::uint32_t frames,
                     std::uint32_t sampleRate)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;

    constexpr std::uint16_t channels      = 2;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint16_t blockAlign    = channels * bitsPerSample / 8u;
    const std::uint32_t     byteRate      = sampleRate * blockAlign;
    const std::uint32_t     dataBytes     = frames * blockAlign;

    file.write("RIFF", 4);
    writeU32(file, 36u + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16u);
    writeU16(file, 1u);
    writeU16(file, channels);
    writeU32(file, sampleRate);
    writeU32(file, byteRate);
    writeU16(file, blockAlign);
    writeU16(file, bitsPerSample);
    file.write("data", 4);
    writeU32(file, dataBytes);

    for ( std::uint32_t frame = 0; frame < frames; ++frame ) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
                             static_cast<double>(frame) /
                             static_cast<double>(sampleRate);
        const auto   sample =
            static_cast<std::int16_t>(std::sin(phase) * 12000.0);
        writeU16(file, static_cast<std::uint16_t>(sample));
        writeU16(file, static_cast<std::uint16_t>(sample));
    }
    return file.good();
}

/// @brief 读取完整二进制文件。
/// @param path 输入路径。
/// @param bytes 输出字节。
/// @return 是否读取成功。
bool readFile(const std::filesystem::path& path,
              std::vector<unsigned char>&  bytes)
{
    bytes.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;
    const auto size = file.tellg();
    if ( size <= 0 ) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

/// @brief 单个解码读取窗口的统计信息。
struct DecodeWindowStats {
    /// @brief 读取起始帧。
    std::size_t startFrame{ 0 };

    /// @brief 请求读取的帧数。
    std::size_t requestedFrames{ 0 };

    /// @brief 实际读取的帧数。
    std::size_t readFrames{ 0 };

    /// @brief 窗口 RMS。
    double rms{ 0.0 };

    /// @brief 窗口峰值。
    double peak{ 0.0 };

    /// @brief 有限浮点样本数量。
    std::size_t finiteSamples{ 0 };

    /// @brief 近似静音帧数量。
    std::size_t silentFrames{ 0 };

    /// @brief 是否包含 NaN 或 Inf。
    bool hasInvalidSamples{ false };
};

/// @brief 音频解码探针结果。
struct DecodeProbeResult {
    /// @brief 音轨是否创建成功。
    bool trackCreated{ false };

    /// @brief 解码器报告的总帧数。
    std::size_t trackFrames{ 0 };

    /// @brief 短读窗口数量。
    std::size_t shortReadWindows{ 0 };

    /// @brief 全静音窗口数量。
    std::size_t silentWindows{ 0 };

    /// @brief 非静音窗口数量。
    std::size_t nonSilentWindows{ 0 };

    /// @brief 是否发现非法浮点样本。
    bool hasInvalidSamples{ false };

    /// @brief 所有采样窗口。
    std::vector<DecodeWindowStats> windows;
};

/// @brief 返回小写扩展名。
/// @param path 文件路径。
/// @return 小写扩展名。
std::string lowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// @brief 判断文件是否为音频资源。
/// @param path 文件路径。
/// @return 支持时返回 true。
bool isAudioResourceFile(const std::filesystem::path& path)
{
    const std::string extension = lowerExtension(path);
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3" ||
           extension == ".flac" || extension == ".m4a" ||
           extension == ".opus" || extension == ".aac";
}

/// @brief 判断容器导出失败是否来自当前环境缺少可用编码器。
/// @param extension 输出文件扩展名。
/// @param errorMessage 导出错误信息。
/// @return 可以跳过该环境相关用例时返回 true。
bool isOptionalContainerEncoderUnavailable(const std::string& extension,
                                           const std::string& errorMessage)
{
    if ( extension != ".mp3" ) {
        return false;
    }
    return errorMessage.find("FFmpeg encoder is not available") !=
               std::string::npos ||
           errorMessage.find("Failed to open audio encoder") !=
               std::string::npos;
}

/// @brief 递归收集音频测试资源。
/// @param root 资源根目录。
/// @return 音频文件列表。
std::vector<std::filesystem::path> collectAudioFiles(
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code                    error;
    if ( !std::filesystem::is_directory(root, error) || error ) {
        return files;
    }

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while ( !error && it != end ) {
        const auto& path = it->path();
        if ( it->is_regular_file(error) && !error &&
             isAudioResourceFile(path) ) {
            files.push_back(path);
        }
        it.increment(error);
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// @brief 统计一个解码窗口中的样本连续性。
/// @param buffer 已读取的音频缓冲。
/// @param startFrame 起始帧。
/// @param requestedFrames 请求帧数。
/// @param readFrames 实际读取帧数。
/// @return 解码窗口统计。
DecodeWindowStats analyzeDecodeWindow(ice::AudioBuffer& buffer,
                                      std::size_t       startFrame,
                                      std::size_t       requestedFrames,
                                      std::size_t       readFrames)
{
    DecodeWindowStats stats;
    stats.startFrame      = startFrame;
    stats.requestedFrames = requestedFrames;
    stats.readFrames      = readFrames;

    const auto format   = ice::ICEConfig::internal_format;
    const auto channels = static_cast<std::size_t>(format.channels);
    const auto samples  = buffer.raw_ptrs();
    if ( !samples || channels == 0 ) {
        stats.hasInvalidSamples = true;
        return stats;
    }

    double sumSquares = 0.0;
    for ( std::size_t frame = 0; frame < readFrames; ++frame ) {
        double framePeak = 0.0;
        for ( std::size_t channel = 0; channel < channels; ++channel ) {
            const float sample = samples[channel][frame];
            if ( !std::isfinite(sample) ) {
                stats.hasInvalidSamples = true;
                continue;
            }
            const double value    = static_cast<double>(sample);
            const double absValue = std::abs(value);
            framePeak             = std::max(framePeak, absValue);
            stats.peak            = std::max(stats.peak, absValue);
            sumSquares += value * value;
            ++stats.finiteSamples;
        }
        if ( framePeak < 1e-5 ) {
            ++stats.silentFrames;
        }
    }

    if ( stats.finiteSamples > 0 ) {
        stats.rms =
            std::sqrt(sumSquares / static_cast<double>(stats.finiteSamples));
    }
    return stats;
}

/// @brief 生成多个解码采样窗口的起始帧。
/// @param trackFrames 音轨总帧数。
/// @param windowFrames 单个窗口帧数。
/// @return 起始帧列表。
std::vector<std::size_t> makeDecodeWindowStarts(std::size_t trackFrames,
                                                std::size_t windowFrames)
{
    std::vector<std::size_t> starts;
    if ( trackFrames == 0 ) return starts;

    starts.push_back(0);
    starts.push_back(trackFrames / 4);
    starts.push_back(trackFrames / 2);
    starts.push_back((trackFrames * 3) / 4);
    starts.push_back(trackFrames > windowFrames ? trackFrames - windowFrames
                                                : std::size_t{ 0 });

    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

/// @brief 通过 IonCachyEngine 多窗口读取音频并收集诊断信息。
/// @param path 音频路径。
/// @return 解码探针结果。
DecodeProbeResult probeAudioDecode(const std::filesystem::path& path)
{
    DecodeProbeResult result;
    ice::ThreadPool   threadPool(1);
    auto decoderFactory = std::make_shared<ice::FFmpegDecoderFactory>();
    auto track          = ice::AudioTrack::create(MMM::Config::pathToUtf8(path),
                                                  threadPool,
                                                  decoderFactory,
                                                  ice::CachingStrategy::CACHY);
    if ( !track ) {
        return result;
    }

    result.trackCreated = true;
    result.trackFrames  = track->num_frames();
    if ( result.trackFrames == 0 ) {
        return result;
    }

    constexpr std::size_t windowFrames = 4096;
    for ( std::size_t startFrame :
          makeDecodeWindowStarts(result.trackFrames, windowFrames) ) {
        const std::size_t requestedFrames =
            std::min(windowFrames, result.trackFrames - startFrame);
        ice::AudioBuffer buffer;
        buffer.resize(ice::ICEConfig::internal_format, requestedFrames);
        buffer.clear();
        const std::size_t readFrames =
            track->read(buffer, startFrame, requestedFrames);

        DecodeWindowStats stats = analyzeDecodeWindow(
            buffer, startFrame, requestedFrames, readFrames);
        if ( readFrames < requestedFrames ) {
            ++result.shortReadWindows;
        }
        if ( stats.peak < 1e-5 ) {
            ++result.silentWindows;
        } else {
            ++result.nonSilentWindows;
        }
        result.hasInvalidSamples =
            result.hasInvalidSamples || stats.hasInvalidSamples;
        result.windows.push_back(stats);
    }
    return result;
}

/// @brief 输出并校验音频解码探针结果。
/// @param path 音频路径。
/// @param expectedMinimumFrames 预期最少帧数。
/// @param label 测试标签。
/// @param strictNonSilent 是否要求每个采样窗口都不是静音。
/// @return 验证是否通过。
bool checkEngineDecode(const std::filesystem::path& path,
                       std::size_t                  expectedMinimumFrames,
                       const std::string& label, bool strictNonSilent)
{
    const DecodeProbeResult probe = probeAudioDecode(path);
    XINFO("[audio-speed-export] {} engine frames={} duration={:.3f}s",
          label,
          probe.trackFrames,
          static_cast<double>(probe.trackFrames) /
              static_cast<double>(ice::ICEConfig::internal_format.samplerate));
    for ( const auto& window : probe.windows ) {
        XINFO(
            "[audio-speed-export] {} window start={} requested={} read={} "
            "rms={:.8f} peak={:.8f} silent_frames={} invalid={}",
            label,
            window.startFrame,
            window.requestedFrames,
            window.readFrames,
            window.rms,
            window.peak,
            window.silentFrames,
            window.hasInvalidSamples);
    }

    bool ok = true;
    ok &= check(probe.trackCreated, label + " engine track created");
    ok &= check(probe.trackFrames >= expectedMinimumFrames,
                label + " engine frame count covers output");
    ok &= check(!probe.windows.empty(), label + " decode windows sampled");
    ok &= check(probe.shortReadWindows == 0,
                label + " decode windows have no short reads");
    ok &= check(!probe.hasInvalidSamples, label + " decoded samples finite");
    if ( strictNonSilent ) {
        ok &= check(probe.silentWindows == 0,
                    label + " decode windows are non-silent");
    } else {
        ok &= check(probe.nonSilentWindows > 0,
                    label + " has at least one non-silent decode window");
    }
    return ok;
}

/// @brief 使用 IonCachyEngine 读取音频尾部和多个窗口，验证解码连续性。
/// @param path 音频路径。
/// @param expectedMinimumFrames 预期最少帧数。
/// @param label 测试标签。
/// @return 验证是否通过。
bool checkEngineCanReadTail(const std::filesystem::path& path,
                            std::size_t                  expectedMinimumFrames,
                            const std::string&           label)
{
    return checkEngineDecode(path, expectedMinimumFrames, label, false);
}

/// @brief 运行真实音频资源解码覆盖测试。
/// @param resourceRoot 测试资源根目录。
/// @param outputRoot 输出目录。
/// @return 通过时返回 true。
bool runResourceAudioCoverage(const std::filesystem::path& resourceRoot,
                              const std::filesystem::path& outputRoot)
{
    const auto files = collectAudioFiles(resourceRoot);
    bool       ok    = true;
    ok &= check(!files.empty(), "resource audio files discovered");

    std::error_code createError;
    std::filesystem::create_directories(outputRoot, createError);
    ok &= check(!createError, "resource audio output directory created");

    std::vector<std::string> exportedExtensions;
    std::size_t              passed = 0;
    for ( std::size_t i = 0; i < files.size(); ++i ) {
        const auto extension = lowerExtension(files[i]);
        XINFO("[audio-speed-export] Resource audio case {} / {}: {}",
              i + 1,
              files.size(),
              MMM::Config::pathToUtf8(files[i]));
        if ( checkEngineDecode(files[i],
                               1,
                               "resource " + files[i].filename().string(),
                               false) ) {
            ++passed;
        } else {
            ok = false;
        }

        if ( std::find(exportedExtensions.begin(),
                       exportedExtensions.end(),
                       extension) != exportedExtensions.end() ) {
            continue;
        }
        exportedExtensions.push_back(extension);

        const auto exportPath =
            outputRoot / ("resource_export_" + std::to_string(i) + ".wav");
        MMM::Audio::AudioSpeedExportOptions options;
        options.inputPath     = files[i];
        options.outputPath    = exportPath;
        options.speed         = 1.25;
        options.preservePitch = false;
        const auto result =
            MMM::Audio::AudioSpeedExportService::exportWav(options);
        if ( !result.success ) {
            XERROR("[audio-speed-export] resource export error: {}",
                   result.errorMessage);
        }
        ok &=
            check(result.success, "resource export succeeds for " + extension);
        ok &= check(result.outputFrames > 0,
                    "resource export writes frames for " + extension);
        if ( result.success ) {
            ok &= checkEngineDecode(exportPath,
                                    minimumDecodedFrames(result.outputFrames),
                                    "resource exported " + extension,
                                    false);
        }
    }

    XINFO("[audio-speed-export] Resource audio decode coverage passed {}/{}",
          passed,
          files.size());
    return ok;
}

}  // namespace

int main(int argc, char* argv[])
{
    XLogger::init("AudioSpeedExportServiceTest");

    const auto root =
        std::filesystem::temp_directory_path() / "mmm_audio_speed_export_test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);

    const auto inputPath         = root / "input.wav";
    const auto largeInputPath    = root / "large_input.wav";
    const auto longInputPath     = root / "long_input.wav";
    const auto outputPath        = root / "output_2x.wav";
    const auto keepPitchOutput   = root / "output_2x_keep_pitch.wav";
    const auto paddedOutput      = root / "output_2x_padded.wav";
    const auto longOggOutput     = root / "long_output_1_2x.ogg";
    const auto cacheReloadOutput = root / "cache_reload.ogg";
    const auto unicodeOutputPath = root / MMM::Config::utf8ToPath("中文目录") /
                                   MMM::Config::utf8ToPath("输出_倍速_2x.ogg");

    bool ok = true;
    ok &= check(writeFixtureWav(inputPath, 4800, 48000), "fixture wav created");
    ok &= check(writeFixtureWav(largeInputPath, 96000, 48000),
                "large fixture wav created");
    ok &= check(writeFixtureWav(longInputPath, 48000 * 20, 48000),
                "long fixture wav created");
    ok &= checkEngineCanReadTail(largeInputPath, 96000, "large fixture");

    MMM::Audio::AudioSpeedExportOptions options;
    options.inputPath     = inputPath;
    options.outputPath    = outputPath;
    options.speed         = 2.0;
    options.preservePitch = false;
    float lastProgress    = 0.0f;
    options.progressCallback =
        [&lastProgress](const MMM::Audio::AudioSpeedExportProgress& progress) {
            lastProgress = std::max(lastProgress, progress.progress);
        };

    const auto result = MMM::Audio::AudioSpeedExportService::exportWav(options);
    if ( !result.success ) {
        XERROR("[audio-speed-export] error: {}", result.errorMessage);
    }
    ok &= check(result.success, "speed export succeeds");
    ok &= check(result.outputFrames == 2400, "2x pitch-shifted frame count");
    ok &= check(isNearlyEqual(result.outputDurationSeconds, 0.05),
                "2x pitch-shifted duration");
    ok &= check(lastProgress >= 1.0f, "progress reached done");

    std::vector<unsigned char> bytes;
    const bool                 outputReadable = readFile(outputPath, bytes);
    ok &= check(outputReadable, "output wav readable");
    ok &= check(bytes.size() >= 44, "output wav has header");
    if ( bytes.size() >= 44 ) {
        ok &= check(std::string(reinterpret_cast<const char*>(bytes.data()),
                                4) == "RIFF",
                    "output wav riff header");
        const auto dataBytes = readWavDataBytes(bytes);
        ok &= check(dataBytes.has_value(), "output wav data chunk exists");
        if ( dataBytes ) {
            ok &= check(*dataBytes == 2400u * 2u * 2u, "output wav data size");
        }
    }

    const std::vector<std::string> containerExtensions{
        ".mp3", ".flac", ".ogg", ".m4a", ".opus", ".aac"
    };
    for ( const auto& extension : containerExtensions ) {
        const std::string label = extension.substr(1);
        const auto        containerOutput =
            root / (std::string("output_2x") + extension);

        MMM::Audio::AudioSpeedExportOptions containerOptions;
        containerOptions.inputPath     = largeInputPath;
        containerOptions.outputPath    = containerOutput;
        containerOptions.speed         = 2.0;
        containerOptions.preservePitch = false;
        const auto containerResult =
            MMM::Audio::AudioSpeedExportService::exportWav(containerOptions);
        if ( !containerResult.success ) {
            XERROR("[audio-speed-export] {} error: {}",
                   label,
                   containerResult.errorMessage);
            if ( isOptionalContainerEncoderUnavailable(
                     extension, containerResult.errorMessage) ) {
                XINFO("[audio-speed-export] SKIP: {} encoder unavailable",
                      label);
                continue;
            }
        }
        ok &= check(containerResult.success, label + " speed export succeeds");
        ok &= check(containerResult.outputFrames == 48000,
                    label + " export receiver frame count");
        if ( containerResult.success ) {
            ok &= checkEngineCanReadTail(
                containerOutput,
                minimumDecodedFrames(containerResult.outputFrames),
                label + " output");
        }
    }

    MMM::Audio::AudioSpeedExportOptions longOggOptions;
    longOggOptions.inputPath     = longInputPath;
    longOggOptions.outputPath    = longOggOutput;
    longOggOptions.speed         = 1.2;
    longOggOptions.preservePitch = false;
    const auto longOggResult =
        MMM::Audio::AudioSpeedExportService::exportWav(longOggOptions);
    if ( !longOggResult.success ) {
        XERROR("[audio-speed-export] long ogg error: {}",
               longOggResult.errorMessage);
    }
    ok &= check(longOggResult.success, "long ogg speed export succeeds");
    ok &=
        check(longOggResult.outputFrames > 0, "long ogg export writes frames");
    if ( longOggResult.success ) {
        ok &= checkEngineCanReadTail(
            longOggOutput,
            minimumDecodedFrames(longOggResult.outputFrames),
            "long ogg output");
    }

    std::error_code unicodeDirectoryError;
    std::filesystem::create_directories(unicodeOutputPath.parent_path(),
                                        unicodeDirectoryError);
    ok &= check(!unicodeDirectoryError, "unicode output directory created");

    MMM::Audio::AudioSpeedExportOptions unicodeOptions;
    unicodeOptions.inputPath     = inputPath;
    unicodeOptions.outputPath    = unicodeOutputPath;
    unicodeOptions.speed         = 2.0;
    unicodeOptions.preservePitch = false;
    const auto unicodeResult =
        MMM::Audio::AudioSpeedExportService::exportWav(unicodeOptions);
    if ( !unicodeResult.success ) {
        XERROR("[audio-speed-export] unicode path error: {}",
               unicodeResult.errorMessage);
    }
    ok &= check(unicodeResult.success, "unicode path speed export succeeds");
    ok &= check(unicodeResult.outputFrames == 2400,
                "unicode path export receiver frame count");
    if ( unicodeResult.success ) {
        std::error_code outputExistsError;
        ok &= check(std::filesystem::is_regular_file(unicodeOutputPath,
                                                     outputExistsError) &&
                        !outputExistsError,
                    "unicode path output file exists");
        ok &= checkEngineCanReadTail(
            unicodeOutputPath,
            minimumDecodedFrames(unicodeResult.outputFrames),
            "unicode path output");
    }

    MMM::Audio::AudioSpeedExportOptions cacheFirstOptions;
    cacheFirstOptions.inputPath     = inputPath;
    cacheFirstOptions.outputPath    = cacheReloadOutput;
    cacheFirstOptions.speed         = 2.0;
    cacheFirstOptions.preservePitch = false;
    const auto cacheFirstResult =
        MMM::Audio::AudioSpeedExportService::exportWav(cacheFirstOptions);
    ok &= check(cacheFirstResult.success, "cache reload first export succeeds");

    ice::ThreadPool cacheThreadPool(1);
    ice::AudioPool  cachePool;
    const auto      cachePathUtf8 = MMM::Config::pathToUtf8(cacheReloadOutput);
    auto            firstCachedTrack =
        cachePool.get_or_load(cacheThreadPool, cachePathUtf8).lock();
    ok &= check(firstCachedTrack != nullptr, "cache first track loaded");
    const std::size_t firstCachedFrames =
        firstCachedTrack ? firstCachedTrack->num_frames() : 0;
    ok &= check(firstCachedFrames >=
                    minimumDecodedFrames(cacheFirstResult.outputFrames),
                "cache first track frame count");

    MMM::Audio::AudioSpeedExportOptions cacheSecondOptions;
    cacheSecondOptions.inputPath     = longInputPath;
    cacheSecondOptions.outputPath    = cacheReloadOutput;
    cacheSecondOptions.speed         = 1.2;
    cacheSecondOptions.preservePitch = false;
    const auto cacheSecondResult =
        MMM::Audio::AudioSpeedExportService::exportWav(cacheSecondOptions);
    ok &=
        check(cacheSecondResult.success, "cache reload second export succeeds");
    auto secondCachedTrack =
        cachePool.get_or_load(cacheThreadPool, cachePathUtf8).lock();
    ok &= check(secondCachedTrack != nullptr, "cache second track loaded");
    const std::size_t secondCachedFrames =
        secondCachedTrack ? secondCachedTrack->num_frames() : 0;
    ok &= check(secondCachedFrames >=
                    minimumDecodedFrames(cacheSecondResult.outputFrames),
                "cache second track reloads changed file");
    ok &= check(secondCachedFrames > firstCachedFrames,
                "cache second track is not stale");

    MMM::Audio::AudioSpeedExportOptions keepPitchOptions;
    keepPitchOptions.inputPath     = inputPath;
    keepPitchOptions.outputPath    = keepPitchOutput;
    keepPitchOptions.speed         = 2.0;
    keepPitchOptions.preservePitch = true;
    float keepPitchProgress        = 0.0f;
    keepPitchOptions.progressCallback =
        [&keepPitchProgress](
            const MMM::Audio::AudioSpeedExportProgress& progress) {
            keepPitchProgress = std::max(keepPitchProgress, progress.progress);
        };

    const auto keepPitchResult =
        MMM::Audio::AudioSpeedExportService::exportWav(keepPitchOptions);
    if ( !keepPitchResult.success ) {
        XERROR("[audio-speed-export] keep pitch error: {}",
               keepPitchResult.errorMessage);
    }
    ok &= check(keepPitchResult.success, "keep-pitch speed export succeeds");
    ok &= check(keepPitchResult.outputFrames > 0,
                "keep-pitch export writes frames");
    ok &= check(keepPitchResult.outputFrames >= 1600 &&
                    keepPitchResult.outputFrames <= 3200,
                "keep-pitch frame count near 2x duration");
    ok &=
        check(isNearlyEqual(
                  keepPitchResult.outputDurationSeconds,
                  static_cast<double>(keepPitchResult.outputFrames) / 48000.0),
              "keep-pitch output duration returned");
    ok &= check(keepPitchProgress >= 1.0f, "keep-pitch progress reached done");

    std::vector<unsigned char> keepPitchBytes;
    const bool keepPitchReadable = readFile(keepPitchOutput, keepPitchBytes);
    ok &= check(keepPitchReadable, "keep-pitch output wav readable");
    ok &= check(keepPitchBytes.size() >= 44, "keep-pitch output wav header");
    if ( keepPitchBytes.size() >= 44 ) {
        ok &= check(
            std::string(reinterpret_cast<const char*>(keepPitchBytes.data()),
                        4) == "RIFF",
            "keep-pitch output wav riff header");
        const auto dataBytes = readWavDataBytes(keepPitchBytes);
        ok &= check(dataBytes.has_value(),
                    "keep-pitch output wav data chunk exists");
        if ( dataBytes ) {
            ok &= check(*dataBytes == keepPitchResult.outputFrames * 2u * 2u,
                        "keep-pitch output wav data size");
        }
    }

    MMM::Audio::AudioSpeedExportOptions paddedOptions;
    paddedOptions.inputPath              = inputPath;
    paddedOptions.outputPath             = paddedOutput;
    paddedOptions.speed                  = 2.0;
    paddedOptions.preservePitch          = false;
    paddedOptions.minimumDurationSeconds = 0.075;
    const auto paddedResult =
        MMM::Audio::AudioSpeedExportService::exportWav(paddedOptions);
    if ( !paddedResult.success ) {
        XERROR("[audio-speed-export] padded error: {}",
               paddedResult.errorMessage);
    }
    ok &= check(paddedResult.success, "minimum-duration export succeeds");
    ok &= check(paddedResult.outputFrames == 3600,
                "minimum-duration export pads silence");
    ok &= check(isNearlyEqual(paddedResult.outputDurationSeconds, 0.075),
                "minimum-duration output duration returned");

    std::vector<unsigned char> paddedBytes;
    const bool paddedReadable = readFile(paddedOutput, paddedBytes);
    ok &= check(paddedReadable, "minimum-duration output wav readable");
    if ( paddedBytes.size() >= 44 ) {
        const auto dataBytes = readWavDataBytes(paddedBytes);
        ok &= check(dataBytes.has_value(),
                    "minimum-duration output wav data chunk exists");
        if ( dataBytes ) {
            ok &= check(*dataBytes == 3600u * 2u * 2u,
                        "minimum-duration output wav data size");
        }
    }
    ok &= checkEngineCanReadTail(paddedOutput, 3600, "minimum-duration output");

    if ( argc >= 3 ) {
        ok &= runResourceAudioCoverage(argv[1], argv[2]);
    }

    if ( const char* externalProbePath = std::getenv("MMM_AUDIO_PROBE_FILE");
         externalProbePath && externalProbePath[0] != '\0' ) {
        const std::filesystem::path probePath(externalProbePath);
        ok &= checkEngineDecode(probePath, 1, "external probe", false);
    }

    std::filesystem::remove_all(root, cleanupError);

    if ( !ok ) {
        XLogger::shutdown();
        return EXIT_FAILURE;
    }

    XINFO("AudioSpeedExportServiceTest passed.");
    XLogger::shutdown();
    return EXIT_SUCCESS;
}
