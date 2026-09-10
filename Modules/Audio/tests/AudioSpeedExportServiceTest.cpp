#include "audio/AudioSpeedExportService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

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

// 本测试同时承担倍速导出行为回归与真实资源解码探针两类职责：
//
// - 用程序生成的 48 kHz 立体声 WAV 验证精确输出帧数和时长；
// - 分别覆盖变调线性插值与保留音高 TimeStretcher 图；
// - 检查 WAV RIFF/data chunk，确认报告帧数与容器负载一致；
// - 遍历常用目标扩展名，验证 Receiver 容器选择和回读能力；
// - 覆盖长 OGG、Unicode 路径与同路径覆盖后的 AudioPool 缓存刷新；
// - minimumDurationSeconds 必须补足静音尾部而不是改变有效内容速度；
// - 可选资源目录按源扩展名抽样导出，并对全部资源执行多窗口解码；
// - 外部 MMM_AUDIO_PROBE_FILE 只增加诊断覆盖，不改变固定回归场景。
//
// 解码验证不只检查 track 创建，还在开头、四分点、中点、四分之三和尾部读取
// 窗口，统计短读、静音、RMS、峰值与非有限样本。压缩容器允许少量编码器延迟，
// 因此最低回读帧数使用 95% 容忍；Receiver 自身报告帧数仍按精确理论值检查。
//
// 文件系统约束如下：
//
// - 固定夹具与输出位于系统临时目录，运行结束统一删除；
// - 资源覆盖输出使用调用方显式传入目录，不写回 tests/data；
// - 输入路径先由 filesystem 表达，再通过项目 UTF-8 helper 交给解码器；
// - 文件遍历使用 error_code 并跳过无权限目录，不依赖异常；
// - 输出回读始终在服务成功后进行，失败路径不会把旧文件当作新结果；
// - 缓存覆盖场景故意复用路径，其余场景使用独立目标防止相互污染。
//
// 通过条件分层为：夹具可用、服务返回成功、结果元数据正确、容器结构可读、
// ICE 解码窗口连续。任一层失败都保留自己的标签，使报告能区分导出算法、
// 编码器、文件系统与解码器问题，而不是只得到一个笼统的测试退出码。

/// @brief 写入 16 位小端整数。
/// @param file 输出文件流。
/// @param value 要写入的值。
void writeU16(std::ofstream& file, std::uint16_t value)
{
    // 测试显式拆字节，不依赖运行主机端序。
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
    // RIFF 所有多字节数字字段均按最低有效字节优先写入。
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
    // 越界返回零，chunk 解析器随后会把不一致长度判为无效文件。
    if ( offset + 4 > bytes.size() ) return 0;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

/// @brief WAV chunk 位置。
struct WavChunkInfo {
    /// @brief offset 指向 chunk payload，不包含八字节 ID 与长度头。
    /// @brief chunk 数据起点。
    std::size_t offset{ 0 };

    /// @brief chunk 数据字节数。
    std::uint32_t size{ 0 };
};

/// @brief 查找 WAV RIFF chunk。
/// @param bytes 文件字节。
/// @param chunkId 四字节 chunk id。
/// @return 找到时返回 chunk 位置。
///
/// 遍历规则遵循 RIFF：文件头固定十二字节，每个子块由四字节 ID、四字节长度
/// 和 payload 组成，奇数字节 payload 后有一个对齐填充。任何声明长度越过文件
/// 尾部都使整个查找失败，不能继续在损坏字节中寻找伪 chunk。
/// 未找到目标 chunk 返回 nullopt，和找到零长度 chunk 的 WavChunkInfo 明确区分。
std::optional<WavChunkInfo> findWavChunk(
    const std::vector<unsigned char>& bytes, std::string_view chunkId)
{
    // 先验证 RIFF/WAVE 容器签名，避免在任意文件字节中误匹配 chunk ID。
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
    // 每轮至少需要完整 chunk 头；奇数字节 payload 后跳过 RIFF 对齐填充。
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
    // 只暴露 data 长度，具体 chunk 遍历与边界校验集中在 findWavChunk。
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
    // 测试选择累计断言，因此每个成功与失败项都带稳定标签输出。
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
    // 秒数均由整数帧除固定采样率得到，极小绝对误差足以覆盖浮点换算。
    return std::abs(actual - expected) < 1e-6;
}

/// @brief 计算带少量编码器延迟容忍的最低读回帧数。
/// @param expectedFrames 期望输出帧数。
/// @return 最低可接受读回帧数。
std::size_t minimumDecodedFrames(std::size_t expectedFrames)
{
    // 短文件不放宽，较长压缩容器只容许最多 5% 的编解码延迟差异。
    return expectedFrames > 100 ? expectedFrames * 95 / 100 : expectedFrames;
}

/// @brief 创建测试用立体声 WAV。
/// @param path 输出路径。
/// @param frames 帧数。
/// @param sampleRate 采样率。
/// @return 是否创建成功。
///
/// 夹具采用 PCM16 双声道，blockAlign 固定为四字节，RIFF 长度等于 36 加数据
/// 字节数。正弦波幅度保留充足余量，不产生削波；左右相同简化解码质量统计。
/// 文件只用于本次临时测试，成功条件包含完整 flush 后的 stream.good。
///
/// 头部字段关系如下：
///
/// - byteRate 等于 sampleRate 乘 blockAlign；
/// - dataBytes 等于 frames 乘 blockAlign；
/// - RIFF size 不含开头八字节，因此为 36 加 dataBytes；
/// - fmt size 为 PCM 固定的十六字节；
/// - format tag 为一，表示未压缩整数 PCM；
/// - 每个 frame 依次写左、右两个相同 int16 样本。
bool writeFixtureWav(const std::filesystem::path& path, std::uint32_t frames,
                     std::uint32_t sampleRate)
{
    // 使用 error_code 创建目录，夹具失败通过返回值表达而不抛异常。
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

    // 固定 PCM fmt 块之后紧接 data 块，便于独立验证输出 chunk 解析。
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

    // 440 Hz 非静音正弦波写入左右声道，可检测解码尾部被错误清零。
    for ( std::uint32_t frame = 0; frame < frames; ++frame ) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
                             static_cast<double>(frame) /
                             static_cast<double>(sampleRate);
        const auto sample =
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
    // 先在文件尾取得大小，再一次性读取，便于后续按偏移解析 RIFF。
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
    /// @brief 一份窗口同时保存读取契约与样本质量统计，便于日志定位具体区间。
    ///
    /// requestedFrames 与 readFrames 用于识别短读；finiteSamples 与无效标志用于
    /// 区分空输出和数值损坏；RMS、peak、silentFrames 用于识别错误静音区间。
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
    /// @brief 聚合多窗口统计，不把单个尾部问题折叠成只有 true/false 的结果。
    ///
    /// trackCreated 与 trackFrames 分开保存，能够区分解码器拒绝容器和接受容器
    /// 但报告空音轨。窗口集合只在帧数非零时生成。
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
    // 按 unsigned char 调用 tolower，避免负 char 触发未定义行为。
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
    // 白名单只包含当前 FFmpeg/ICE 测试关心的常见音频资源扩展名。
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
    // 仅 MP3 编码器被视为环境可选，其他目标失败仍是回归。
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
    // 权限不足目录被跳过，其他迭代错误通过 error_code 停止并返回已发现集合。
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
    // 排序保证资源覆盖日志与按扩展名首例导出在不同文件系统上稳定。
    std::sort(files.begin(), files.end());
    return files;
}

/// @brief 统计一个解码窗口中的样本连续性。
/// @param buffer 已读取的音频缓冲。
/// @param startFrame 起始帧。
/// @param requestedFrames 请求帧数。
/// @param readFrames 实际读取帧数。
/// @return 解码窗口统计。
///
/// 有限样本参与 RMS 与峰值，NaN/Inf 只设置错误标志而不进入平方和。静音按帧
/// 判断：只有该帧全部声道峰值小于阈值才累计 silentFrames。这样单侧声道有声
/// 不会被另一静音声道误判为整帧静音。
///
/// stats 保留 start/request/read 三个坐标，让尾窗短读可以直接定位到容器声明的
/// 帧范围。finiteSamples 理论上等于 readFrames 乘声道数，出现差异即意味着
/// 至少一个非有限样本。RMS 只用于诊断，不参与通过阈值。
DecodeWindowStats analyzeDecodeWindow(ice::AudioBuffer& buffer,
                                      std::size_t       startFrame,
                                      std::size_t       requestedFrames,
                                      std::size_t       readFrames)
{
    // 统计只遍历实际 readFrames，短读本身由调用方单独记录。
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

    // framePeak 用于按帧统计静音，sumSquares 与 peak 则按全部有限样本统计。
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

    // 只有至少一个有限样本时才计算 RMS，避免零分母。
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
///
/// 尾部窗口在音轨长于窗口时从 trackFrames-windowFrames 开始，确保请求不会
/// 越界；短音轨则与首窗重合。排序去重后调用方获得严格递增的确定读取顺序。
/// 四分点使用整数除法，任何舍入都向下且仍处于合法音轨范围。
std::vector<std::size_t> makeDecodeWindowStarts(std::size_t trackFrames,
                                                std::size_t windowFrames)
{
    // 五个候选点覆盖首尾与内部区间，短音轨可能重合，最终去重。
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
///
/// AudioTrack 使用 CACHY 策略完整解码，随后每个窗口通过正式 read 接口读取到
/// 内部格式 AudioBuffer。探针不把空音轨视为 track 创建失败，而分别保留
/// trackCreated=true 与 trackFrames=0，便于诊断容器识别和内容为空两类问题。
///
/// 每个窗口缓冲按实际请求长度 resize 并预先清零；若 read 短读，残余静音不会
/// 被统计为已读样本。结果累计 shortReadWindows、silentWindows 与
/// nonSilentWindows，调用方可按夹具或真实资源选择不同静音严格度。
DecodeProbeResult probeAudioDecode(const std::filesystem::path& path)
{
    // 每个探针使用独立单线程解码池，避免跨文件缓存状态影响结果。
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

    // 4096 帧足够计算稳定 RMS，同时不会让多资源探针占用过多内存。
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

        // 短读、静音和非法样本分别统计，日志可区分容器长度与 PCM 质量问题。
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
///
/// 基本契约要求音轨存在、帧数达到下限、窗口非空、无短读且样本全部有限。
/// strictNonSilent 用于已知持续有声的夹具；真实音乐可能合法包含静音开头或尾部，
/// 因而只要求至少一个窗口非静音。所有窗口统计在断言前完整输出。
///
/// duration 日志由探针帧数除内部采样率得到，只作诊断，不参与精确容器时长断言。
/// 各窗口日志同时输出 RMS 与 peak，可区分真实静音、极低音量及解码失败清零。
/// check 使用非短路累计，单个资源可一次报告多个互相关联的失败条件。
bool checkEngineDecode(const std::filesystem::path& path,
                       std::size_t                  expectedMinimumFrames,
                       const std::string& label, bool strictNonSilent)
{
    // 先输出所有窗口诊断，再累计契约断言，失败时保留完整现场。
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
    // 生成夹具可要求每窗非静音；真实资源只要求至少一个窗口有内容。
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
    // 导出容器可能包含合法静音尾部，因此使用非严格静音策略。
    return checkEngineDecode(path, expectedMinimumFrames, label, false);
}

/// @brief 运行真实音频资源解码覆盖测试。
/// @param resourceRoot 测试资源根目录。
/// @param outputRoot 输出目录。
/// @return 通过时返回 true。
///
/// 资源覆盖首先验证发现列表非空与输出目录可创建。每个文件都执行解码探针，
/// 每种源扩展名再选首个文件以 1.25 倍、不保音高导出为 WAV 并回读。输出统一
/// 写到构建测试目录参数，不修改或覆盖源资源。
///
/// exportedExtensions 保存已经验证导出的源扩展名，避免大量同格式资源显著拉长
/// 测试时间；这不影响所有文件的解码窗口覆盖。导出目标按遍历索引命名，确保
/// 不同扩展名不会互相覆盖。最终日志 passed/files 只表示源解码覆盖进度。
bool runResourceAudioCoverage(const std::filesystem::path& resourceRoot,
                              const std::filesystem::path& outputRoot)
{
    // 全部资源都做解码探针，每种源扩展名只选择首个文件做一次倍速导出。
    const auto files = collectAudioFiles(resourceRoot);
    bool       ok    = true;
    ok &= check(!files.empty(), "resource audio files discovered");

    std::error_code createError;
    std::filesystem::create_directories(outputRoot, createError);
    ok &= check(!createError, "resource audio output directory created");

    std::vector<std::string> exportedExtensions;
    std::size_t              passed = 0;
    // passed 只统计源资源解码通过数，整体 ok 还包含导出与回读结果。
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

        // 同扩展名后续文件跳过重复导出，但仍已完成上面的解码窗口覆盖。
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
        // 成功输出必须再次通过 ICE 解码，不能只接受编码器返回值。
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

/// @brief 运行倍速导出、容器编码、缓存刷新与资源解码覆盖测试。
/// @param argc 可选包含资源根目录与资源导出目录。
/// @param argv argv[1]/argv[2] 成对启用资源覆盖；环境变量可启用单文件探针。
/// @return 所有固定场景及可选探针通过时返回 EXIT_SUCCESS。
///
/// 固定场景按以下顺序执行：
///
/// - 初始化应用线程池并清理临时根目录；
/// - 创建 0.1 秒、2 秒和 20 秒三份 48 kHz WAV；
/// - 对大夹具先做多窗口解码，确认测试输入本身可靠；
/// - 将短夹具以 2 倍、不保音高导出 WAV，期望精确 2400 帧；
/// - 解析输出 RIFF/data chunk，确认 PCM16 双声道数据字节数；
/// - 逐个尝试 MP3、FLAC、OGG、M4A、Opus 与 AAC 容器并回读尾部；
/// - 以 1.2 倍导出 20 秒 OGG，覆盖多次大块拉取和长文件尾部；
/// - 导出到中文目录与文件名，覆盖跨平台 UTF-8 路径转换；
/// - 覆盖同一 OGG 路径后从同一 AudioPool 再加载，拒绝陈旧缓存；
/// - 使用 TimeStretcher 导出保音高结果并校验容器大小与回读；
/// - 设置 0.075 秒最小时长，验证 2400 帧有效结果补到 3600 帧；
/// - 按命令行参数执行资源目录覆盖；
/// - 按环境变量执行单外部音频解码探针；
/// - 清理临时目录并按累计结果关闭线程池和日志。
///
/// 容器循环只对当前环境明确缺少 MP3 编码器的两类后端错误执行跳过；其他
/// 编码失败仍会使测试失败。每个成功容器不仅检查 Receiver 帧数，还重新进入
/// IonCachyEngine 解码路径，避免生成无法被应用读取的文件。
///
/// 固定帧数期望由夹具直接推导：
///
/// - 4800 帧以 2 倍导出得到 2400 帧与 0.05 秒；
/// - 96000 帧以 2 倍导出得到 48000 帧；
/// - 20 秒输入以 1.2 倍导出跨越多个处理块，帧数由结果报告后回读；
/// - 保音高结果允许算法窗造成 1600 到 3200 帧范围，但时长字段必须自洽；
/// - 2400 帧结果设置 0.075 秒下限后精确补到 3600 帧。
///
/// 所有固定输出路径互不相同，只有 cacheReloadOutput 有意被覆盖两次，用于验证
/// AudioPool 能识别磁盘文件变化。Unicode 输出使用项目 UTF-8 转换 helper 构造，
/// 不把源代码执行环境的窄字符串编码假设带入文件系统。
///
/// 进度与结果字段按以下方式判定：
///
/// - 普通与保音高路径都必须最终报告至少 1.0；
/// - 回调只记录历史最大值，不要求中间事件严格按固定次数出现；
/// - outputFrames 对无压缩和容器 Receiver 都表示提交的 PCM 帧数；
/// - outputDurationSeconds 必须等于 outputFrames 除内部采样率；
/// - success=false 时先记录 errorMessage，再由 check 累计失败；
/// - 输出回读只在 success=true 后执行，避免用不存在文件产生次生噪声。
///
/// 主流程使用 ok &= 而非短路 &&，让同一场景的独立元数据与容器断言都执行；
/// 只有依赖对象存在的读取使用条件分支保护。最终清理不覆盖 ok，临时目录清理
/// 失败不会被误报为倍速算法失败，但生成物路径会保留在前面的日志中。
///
/// 缓存刷新场景先保持 firstCachedTrack 强引用，再覆盖磁盘文件并再次请求同路径。
/// 因而实现不能仅依赖 weak_ptr 是否过期来判断缓存有效性，还必须识别文件身份
/// 已改变；第二音轨帧数显著大于第一音轨是最直接的可观察结果。
int main(int argc, char* argv[])
{
    XLogger::init("AudioSpeedExportServiceTest");
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();

    // 固定临时目录在开始前完整清理，避免旧输出让存在性与缓存测试假通过。
    const auto root =
        std::filesystem::temp_directory_path() / "mmm_audio_speed_export_test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);

    // 路径集中声明，便于确认除缓存测试外没有两个场景意外写向同一目标。
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

    // 三种长度分别覆盖精确短输出、压缩容器回读和跨多个导出块的长输入。
    bool ok = true;
    ok &= check(writeFixtureWav(inputPath, 4800, 48000), "fixture wav created");
    ok &= check(writeFixtureWav(largeInputPath, 96000, 48000),
                "large fixture wav created");
    // 20 秒夹具足以跨越多次 65536 帧离线处理块。
    ok &= check(writeFixtureWav(longInputPath, 48000 * 20, 48000),
                "long fixture wav created");
    ok &= checkEngineCanReadTail(largeInputPath, 96000, "large fixture");

    // 基准 WAV 场景使用不保音高图，2 倍速度理论长度精确减半。
    MMM::Audio::AudioSpeedExportOptions options;
    options.inputPath     = inputPath;
    options.outputPath    = outputPath;
    options.speed         = 2.0;
    options.preservePitch = false;
    float lastProgress    = 0.0f;
    // 只记录最大进度，验证回调最终到达完成状态而不绑定中间调用次数。
    options.progressCallback =
        [&lastProgress](const MMM::Audio::AudioSpeedExportProgress& progress) {
            lastProgress = std::max(lastProgress, progress.progress);
        };

    // 基准结果同时验证成功标志、精确帧数、实际时长与完成进度四个返回契约。
    const auto result = MMM::Audio::AudioSpeedExportService::exportWav(options);
    if ( !result.success ) {
        XERROR("[audio-speed-export] error: {}", result.errorMessage);
    }
    ok &= check(result.success, "speed export succeeds");
    ok &= check(result.outputFrames == 2400, "2x pitch-shifted frame count");
    ok &= check(isNearlyEqual(result.outputDurationSeconds, 0.05),
                "2x pitch-shifted duration");
    ok &= check(lastProgress >= 1.0f, "progress reached done");

    // 直接解析无压缩 WAV，可精确验证数据字节等于帧数乘声道和样本宽度。
    std::vector<unsigned char> bytes;
    const bool                 outputReadable = readFile(outputPath, bytes);
    ok &= check(outputReadable, "output wav readable");
    ok &= check(bytes.size() >= 44, "output wav has header");
    // 只有完整最小 WAV 头存在时才继续解引用签名与 chunk 数据。
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

    // 常用压缩和无损容器共享同一导出入口，逐个验证编码选择与应用回读。
    const std::vector<std::string> containerExtensions{
        ".mp3", ".flac", ".ogg", ".m4a", ".opus", ".aac"
    };
    // 每种容器复用相同 2 倍输入，便于比较 Receiver 报告的目标帧数。
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
            // 仅已知可选 MP3 编码器缺失可以跳过，其他错误继续落入失败断言。
            if ( isOptionalContainerEncoderUnavailable(
                     extension, containerResult.errorMessage) ) {
                XINFO("[audio-speed-export] SKIP: {} encoder unavailable",
                      label);
                continue;
            }
        }
        ok &= check(containerResult.success, label + " speed export succeeds");
        // Receiver 目标帧数与容器编码延迟无关，仍必须精确报告 48000。
        ok &= check(containerResult.outputFrames == 48000,
                    label + " export receiver frame count");
        if ( containerResult.success ) {
            ok &= checkEngineCanReadTail(
                containerOutput,
                minimumDecodedFrames(containerResult.outputFrames),
                label + " output");
        }
    }

    // 长 OGG 跨越多个 65536 帧处理块，覆盖连续源位置不会在块边界漂移。
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
    // 长输出成功后专门读取尾窗，捕获块边界位置累计造成的末尾短读。
    if ( longOggResult.success ) {
        ok &= checkEngineCanReadTail(
            longOggOutput,
            minimumDecodedFrames(longOggResult.outputFrames),
            "long ogg output");
    }

    // 中文路径在服务入口和 FFmpeg 接收器之间必须保持完整平台路径语义。
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
    // 除回读外先用 filesystem 确认 Unicode 目标确实落到预期路径。
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

    // 首次把短源写到固定路径并通过 AudioPool 加载，记录缓存帧数基线。
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
    // 同一 AudioPool 会记住路径，后续覆盖测试才能暴露缓存失效逻辑错误。
    auto firstCachedTrack =
        cachePool.get_or_load(cacheThreadPool, cachePathUtf8).lock();
    ok &= check(firstCachedTrack != nullptr, "cache first track loaded");
    const std::size_t firstCachedFrames =
        firstCachedTrack ? firstCachedTrack->num_frames() : 0;
    ok &= check(firstCachedFrames >=
                    minimumDecodedFrames(cacheFirstResult.outputFrames),
                "cache first track frame count");

    // 同一路径改写为长源输出，第二次加载必须检测文件变化而非复用旧音轨。
    MMM::Audio::AudioSpeedExportOptions cacheSecondOptions;
    cacheSecondOptions.inputPath     = longInputPath;
    cacheSecondOptions.outputPath    = cacheReloadOutput;
    cacheSecondOptions.speed         = 1.2;
    cacheSecondOptions.preservePitch = false;
    const auto cacheSecondResult =
        MMM::Audio::AudioSpeedExportService::exportWav(cacheSecondOptions);
    ok &=
        check(cacheSecondResult.success, "cache reload second export succeeds");
    // 第二次 get_or_load 必须基于新文件元数据返回更长音轨，而不是旧 weak 缓存。
    auto secondCachedTrack =
        cachePool.get_or_load(cacheThreadPool, cachePathUtf8).lock();
    ok &= check(secondCachedTrack != nullptr, "cache second track loaded");
    const std::size_t secondCachedFrames =
        secondCachedTrack ? secondCachedTrack->num_frames() : 0;
    ok &= check(secondCachedFrames >=
                    minimumDecodedFrames(cacheSecondResult.outputFrames),
                "cache second track reloads changed file");
    // 新输出来自 20 秒输入，回读长度必须明确超过首次 0.1 秒输入结果。
    ok &= check(secondCachedFrames > firstCachedFrames,
                "cache second track is not stale");

    // 保音高算法允许窗口尾部造成一定帧数差异，但实际时长字段必须精确对应。
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
    // Rubber Band 窗口允许尾部差异，但结果必须落在 2 倍时长的宽松有效范围。
    ok &= check(keepPitchResult.outputFrames >= 1600 &&
                    keepPitchResult.outputFrames <= 3200,
                "keep-pitch frame count near 2x duration");
    ok &=
        check(isNearlyEqual(
                  keepPitchResult.outputDurationSeconds,
                  static_cast<double>(keepPitchResult.outputFrames) / 48000.0),
              "keep-pitch output duration returned");
    ok &= check(keepPitchProgress >= 1.0f, "keep-pitch progress reached done");

    // 保音高 WAV 仍需满足 Receiver 报告帧数与 data chunk 字节数一致。
    std::vector<unsigned char> keepPitchBytes;
    const bool keepPitchReadable = readFile(keepPitchOutput, keepPitchBytes);
    ok &= check(keepPitchReadable, "keep-pitch output wav readable");
    ok &= check(keepPitchBytes.size() >= 44, "keep-pitch output wav header");
    // data 字节数仍必须严格等于实际算法输出帧数乘四字节 blockAlign。
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
    ok &= checkEngineCanReadTail(
        keepPitchOutput,
        minimumDecodedFrames(keepPitchResult.outputFrames),
        "keep-pitch output");

    // 理论 2 倍结果为 0.05 秒，最小时长把目标扩展到 0.075 秒即 3600 帧。
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
    // 补足场景直接验证 WAV data 长度，确保静音尾部真正写入容器。
    if ( paddedBytes.size() >= 44 ) {
        const auto dataBytes = readWavDataBytes(paddedBytes);
        ok &= check(dataBytes.has_value(),
                    "minimum-duration output wav data chunk exists");
        if ( dataBytes ) {
            ok &= check(*dataBytes == 3600u * 2u * 2u,
                        "minimum-duration output wav data size");
        }
    }
    // 回读至少 3600 帧，确认静音补足不是只修改结果元数据。
    ok &= checkEngineCanReadTail(paddedOutput, 3600, "minimum-duration output");

    // 资源根与输出根必须成对给出，避免默认向源码资源目录写测试产物。
    // 未传资源参数时固定回归仍完整执行，不把本地资产布局设为测试前置条件。
    if ( argc >= 3 ) {
        ok &= runResourceAudioCoverage(argv[1], argv[2]);
    }

    // 外部探针是诊断入口，只读取用户指定文件且不生成旁路输出。
    if ( const char* externalProbePath = std::getenv("MMM_AUDIO_PROBE_FILE");
         externalProbePath && externalProbePath[0] != '\0' ) {
        const std::filesystem::path probePath(externalProbePath);
        ok &= checkEngineDecode(probePath, 1, "external probe", false);
    }

    // 固定场景生成物在断言完成后清理，可选资源输出由调用方目录保留。
    std::filesystem::remove_all(root, cleanupError);

    // 成败两条退出路径都显式关闭应用线程池和日志，避免进程析构顺序干扰。
    if ( !ok ) {
        appThreadPool.shutdown();
        XLogger::shutdown();
        return EXIT_FAILURE;
    }

    XINFO("AudioSpeedExportServiceTest passed.");
    appThreadPool.shutdown();
    XLogger::shutdown();
    return EXIT_SUCCESS;
}
