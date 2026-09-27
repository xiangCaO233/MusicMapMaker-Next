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
/// @tparam Value 固定宽度无符号整数类型。
/// @param stream 已打开的二进制输出流。
/// @param value 要按最低有效字节优先写出的值。
///
/// WAV 使用小端字段；逐字节写入避免测试夹具依赖宿主机端序。
template<typename Value>
void writeLittleEndian(std::ofstream& stream, Value value)
{
    for ( std::size_t index = 0; index < sizeof(Value); ++index ) {
        stream.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

/// @brief 写入固定采样率、双声道、16 位 PCM WAV 测试音频。
/// @param path 测试输出目录中的目标文件。
/// @param frames 要生成的交错 PCM 帧数。
/// @return 目录创建、文件写入和流收尾均成功时返回 true。
///
/// 每帧写入相同左右样本，且样本值随帧变化，确保文件既可被真实解码器接受，
/// 又无需维护二进制测试资源。数据块大小和 RIFF 总长度按标准头部精确计算。
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

    // 依次写 RIFF 容器、PCM fmt 块与 data 块，字段均为小端。
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
    // 左右声道内容相同，使测试只关注时间长度而不引入声道差异。
    for ( std::size_t frame = 0; frame < frames; ++frame ) {
        const auto sample = static_cast<std::int16_t>(frame % 200U + 1000U);
        writeLittleEndian(stream, static_cast<std::uint16_t>(sample));
        writeLittleEndian(stream, static_cast<std::uint16_t>(sample));
    }
    return stream.good();
}


/// @brief 检查服务调用结果并记录失败。
/// @param condition 当前断言结果。
/// @param message 条件失败时写入日志的上下文。
/// @return 原样返回 condition，便于累计多个独立断言。
bool expect(bool condition, const std::string& message)
{
    if ( !condition ) XERROR("AudioOriginAlignmentServiceTest: {}", message);
    return condition;
}

}  // namespace

/// @brief 运行正相位裁头、负相位补静音和零相位保持长度测试。
/// @return 三种符号路径及夹具创建均成功时返回零。
///
/// 输入为 48000 Hz、4800 帧，即 100 ms。25 ms 精确对应 1200 帧，因此正相位
/// 期望 3600 帧，负相位期望 6000 帧，零相位仍为 4800 帧。
///
/// 三个输出文件使用不同路径，避免后一次导出覆盖前一次结果而掩盖状态错误。
/// 测试只断言服务报告的帧数；实际编码和解码成功已经由 success 覆盖，具体
/// PCM 混合行为属于 AudioTimelineExportService 的独立测试职责。
/// AppThreadPool 在服务调用前初始化，因为真实资源解码沿用应用线程池入口。
/// 生成文件位于 MMM_TEST_OUTPUT_DIR，不向 tests/data 源码资源目录写入结果。
/// 所有长度断言使用整数帧，避免通过浮点秒数再次计算期望而复制实现逻辑。
int main()
{
    XLogger::init("AudioOriginAlignmentServiceTest");
    MMM::Runtime::AppThreadPool::instance().init();

    // 所有生成物写入构建树测试输出目录，测试前后清理以保持重复运行独立。
    const auto root = std::filesystem::path(MMM_TEST_OUTPUT_DIR) /
                      "AudioOriginAlignmentServiceTest";
    std::error_code filesystemError;
    // 清除上次失败遗留文件，保证输出编码器面对的是全新目标路径。
    std::filesystem::remove_all(root, filesystemError);
    // 各目标扩展名均为 WAV，避免本测试把格式选择差异混入相位语义。
    const auto inputPath  = root / "input.wav";
    const auto trimPath   = root / "trim.wav";
    const auto padPath    = root / "pad.wav";
    const auto sourcePath = root / "unchanged.wav";

    // 正相位将内容向左移动 25 ms，开头 1200 帧落在时间线零点之前。
    bool ok = expect(writeFixtureWav(inputPath, 4800U), "无法创建 WAV 输入");
    const auto trimResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = trimPath,
            .phaseMilliseconds = 25.0,
        });
    // 先累计成功状态，再检查长度，使失败日志保留底层导出的具体原因。
    ok &= expect(trimResult.success, trimResult.errorMessage);
    ok &= expect(trimResult.outputFrames == 3600U,
                 "正相位没有裁掉正确的开头帧数");

    // 负相位将内容向右移动 25 ms，导出器应在资源前补 1200 帧静音。
    const auto padResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = padPath,
            .phaseMilliseconds = -25.0,
        });
    // 补静音后的输出比输入多 1200 帧，符号若反转会直接得到裁切长度。
    ok &= expect(padResult.success, padResult.errorMessage);
    ok &= expect(padResult.outputFrames == 6000U,
                 "负相位没有补入正确的开头静音帧数");

    // 零相位是符号边界，验证统一时间线路径不会额外裁切或补帧。
    const auto unchangedResult =
        MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = sourcePath,
            .phaseMilliseconds = 0.0,
        });
    // 零相位作为控制组，排除编码器本身改变报告帧数的可能。
    ok &= expect(unchangedResult.success, unchangedResult.errorMessage);
    // 累计而非短路断言可在一次运行中报告三个符号分支的全部独立结果。
    ok &= expect(unchangedResult.outputFrames == 4800U, "零相位改变了音频帧数");

    // 清理失败不覆盖功能断言结果，避免文件系统收尾掩盖对齐行为。
    std::filesystem::remove_all(root, filesystemError);
    return ok ? 0 : 1;
}
