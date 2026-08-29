#include "audio/AudioOriginAlignmentService.h"

#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

/// @brief 以小端格式写入无符号整数。
template<typename Value>
void writeLittleEndian(std::ofstream& stream, Value value)
{
    for ( std::size_t index = 0; index < sizeof(Value); ++index ) {
        stream.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

/// @brief 写入固定采样率、双声道、16 位 PCM WAV 测试音频。
bool writeFixtureWav(const std::filesystem::path& path, std::size_t frames)
{
    constexpr std::uint32_t SAMPLE_RATE = 48000U;
    constexpr std::uint16_t CHANNELS    = 2U;
    constexpr std::uint16_t BITS        = 16U;
    const auto              dataBytes =
        static_cast<std::uint32_t>(frames * CHANNELS * (BITS / 8U));

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if ( !stream ) return false;

    stream.write("RIFF", 4);
    writeLittleEndian(stream, 36U + dataBytes);
    stream.write("WAVEfmt ", 8);
    writeLittleEndian(stream, 16U);
    writeLittleEndian(stream, static_cast<std::uint16_t>(1U));
    writeLittleEndian(stream, CHANNELS);
    writeLittleEndian(stream, SAMPLE_RATE);
    writeLittleEndian(stream, SAMPLE_RATE * CHANNELS * (BITS / 8U));
    writeLittleEndian(stream,
                      static_cast<std::uint16_t>(CHANNELS * (BITS / 8U)));
    writeLittleEndian(stream, BITS);
    stream.write("data", 4);
    writeLittleEndian(stream, dataBytes);
    for ( std::size_t frame = 0; frame < frames; ++frame ) {
        const auto sample = static_cast<std::int16_t>(frame % 200U + 1000U);
        writeLittleEndian(stream, static_cast<std::uint16_t>(sample));
        writeLittleEndian(stream, static_cast<std::uint16_t>(sample));
    }
    return stream.good();
}


/// @brief 检查服务调用结果并记录失败。
bool expect(bool condition, const std::string& message)
{
    if ( !condition ) XERROR("AudioOriginAlignmentServiceTest: {}", message);
    return condition;
}

}  // namespace

int main()
{
    XLogger::init("AudioOriginAlignmentServiceTest");
    MMM::Runtime::AppThreadPool::instance().init();

    const auto      root = std::filesystem::path(MMM_TEST_OUTPUT_DIR) /
                           "AudioOriginAlignmentServiceTest";
    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);
    const auto inputPath  = root / "input.wav";
    const auto trimPath   = root / "trim.wav";
    const auto padPath    = root / "pad.wav";
    const auto sourcePath = root / "unchanged.wav";

    bool ok = expect(writeFixtureWav(inputPath, 4800U), "无法创建 WAV 输入");
    const auto trimResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = trimPath,
            .phaseMilliseconds = 25.0,
        });
    ok &= expect(trimResult.success, trimResult.errorMessage);
    ok &= expect(trimResult.outputFrames == 3600U,
                 "正相位没有裁掉正确的开头帧数");

    const auto padResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = padPath,
            .phaseMilliseconds = -25.0,
        });
    ok &= expect(padResult.success, padResult.errorMessage);
    ok &= expect(padResult.outputFrames == 6000U,
                 "负相位没有补入正确的开头静音帧数");

    const auto unchangedResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = sourcePath,
            .phaseMilliseconds = 0.0,
        });
    ok &= expect(unchangedResult.success, unchangedResult.errorMessage);
    ok &= expect(unchangedResult.outputFrames == 4800U, "零相位改变了音频帧数");

    std::filesystem::remove_all(root, filesystemError);
    return ok ? 0 : 1;
}
