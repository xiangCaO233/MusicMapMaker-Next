#include "audio/AudioSpeedExportService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/out/io/FFmpegFileReceiver.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string>
#include <utility>

namespace MMM::Audio
{
namespace
{

// 倍速导出根据音高策略构造两种离线音频图：
//
// - preservePitch=true 使用 TimeStretcher 改变时长并维持原音高；
// - preservePitch=false 用连续源位置和线性插值实现随速度一起变调；
// - 两条路径都由 FFmpegFileReceiver 按目标帧数拉取并编码；
// - minimumDurationSeconds 只增加静音尾部，不截短正常倍速结果；
// - 进度区间为准备 0~0.08、编码 0.08~0.98、完成 1.0；
// - 输入、线程池、内部格式和解码缓存均在构图前验证。
// - 两种图都保持内部采样率与声道数，速度只改变源时间映射；
// - 所有失败返回保留 success=false，完成消息只在编码器成功后发送。
// - 进度回调为空时完全跳过事件构造，不影响核心导出路径；
// - 实际输出可因最小时长超过理论值而包含确定性的静音尾部；
// - 成功结果同时报告帧数和秒数，二者由同一实际写入计数生成。

/// @brief 音频倍速导出的离线处理块大小。
constexpr std::size_t AUDIO_SPEED_EXPORT_CHUNK_FRAMES = 65536;

/// @brief 将 SourceNode 的同块输入结束通知转交给离线拉伸器。
/// @param context 生命周期覆盖导出图的 TimeStretcher。
/// @warning 音频处理热路径：只写入 lock-free final 邮箱。
void requestFinalStretcherInput(void* context) noexcept
{
    // SourceNode 在含最后有效输入的同一 block 通知，拉伸器据此刷新算法尾部。
    if ( !context ) return;
    static_cast<void>(
        static_cast<ice::TimeStretcher*>(context)->request_final_input());
}

/// @brief 发送导出进度。
/// @param options 导出参数。
/// @param progress 进度值。
/// @param message 进度文本。
///
/// message 按值接收并移动到事件对象，调用点可直接传临时本地化文本。
void emitProgress(const AudioSpeedExportOptions& options, float progress,
                  std::string message)
{
    // 所有调用点都通过本入口钳位范围，避免 UI 进度条接收过冲。
    if ( !options.progressCallback ) return;
    options.progressCallback(AudioSpeedExportProgress{
        std::clamp(progress, 0.0f, 1.0f), std::move(message) });
}

/// @brief 不保留音高的倍速采样节点。
///
/// 速度既决定输出时长，也决定源采样步长，因此听感音高随速度一起变化。
/// 连续 double 位置跨 block 保存，避免每块重新取整产生累计漂移。
/// 资源短读的 scratch 尾部清零，插值索引超出实际读取范围时也回退到零样本。
class PitchShiftSpeedNode : public ice::IAudioNode
{
public:
    /// @brief 构造采样节点。
    /// @param track 输入音轨。
    /// @param speed 倍速倍率。
    /// @param format 引擎内部音频格式。
    PitchShiftSpeedNode(std::shared_ptr<ice::AudioTrack> track, double speed,
                        ice::AudioDataFormat format)
        : m_track(std::move(track)), m_speed(speed), m_format(format)
    {
    }

    /// @brief 拉取并写入一块变调倍速音频。
    /// @param buffer 输出缓冲。
    /// @warning 离线导出路径：由 FFmpegFileReceiver
    /// 批量调用；会按块读取音轨缓存并做线性插值，不属于实时播放线程。
    void process(ice::AudioBuffer& buffer) override
    {
        // 提前清零保证资源尾部、短读或异常输入后的未覆盖样本保持静音。
        buffer.clear();
        if ( !m_track || m_speed <= 0.0 || !std::isfinite(m_speed) ) {
            return;
        }

        const std::size_t outputFrames = buffer.num_frames();
        const std::size_t trackFrames  = m_track->num_frames();
        if ( outputFrames == 0 || trackFrames == 0 ||
             m_sourcePosition >= static_cast<double>(trackFrames) ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        // 多读一个相邻源帧，为输出末帧的线性插值提供右端样本。
        const double lastSourcePosition =
            m_sourcePosition + static_cast<double>(outputFrames - 1) * m_speed;
        const std::size_t firstSourceFrame =
            static_cast<std::size_t>(std::floor(m_sourcePosition));
        const std::size_t lastSourceFrame = std::min(
            trackFrames - 1,
            static_cast<std::size_t>(std::floor(lastSourcePosition)) + 1);
        const std::size_t sourceFrameCount =
            lastSourceFrame >= firstSourceFrame
                ? lastSourceFrame - firstSourceFrame + 1
                : std::size_t{ 0 };
        if ( sourceFrameCount == 0 ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        // 这是离线节点，允许按当前速度与块范围调整源 scratch 容量。
        m_sourceBuffer.resize(m_format, sourceFrameCount);
        m_sourceBuffer.clear();
        const std::size_t readFrames =
            m_track->read(m_sourceBuffer, firstSourceFrame, sourceFrameCount);
        // 短读后清空未写区，后续边界插值不会混入上一块遗留数据。
        if ( readFrames < sourceFrameCount ) {
            m_sourceBuffer.clear_from(readFrames);
        }

        const float* const* input  = m_sourceBuffer.raw_ptrs();
        float**             output = buffer.raw_ptrs();
        if ( !input || !output ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        // 每个输出帧映射到连续源坐标，速度大于一跳读，低于一重复插值。
        for ( std::size_t frame = 0; frame < outputFrames; ++frame ) {
            const double sourcePosition =
                m_sourcePosition + static_cast<double>(frame) * m_speed;
            if ( sourcePosition >= static_cast<double>(trackFrames) ) {
                continue;
            }

            const std::size_t sourceIndex =
                static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t nextSourceIndex =
                std::min(sourceIndex + 1, trackFrames - 1);
            // fraction 为当前源坐标在相邻整数帧之间的位置。
            const double fraction =
                sourcePosition - static_cast<double>(sourceIndex);
            const std::size_t localA = sourceIndex - firstSourceFrame;
            const std::size_t localB = nextSourceIndex - firstSourceFrame;

            for ( uint16_t channel = 0; channel < m_format.channels;
                  ++channel ) {
                const float a =
                    localA < readFrames ? input[channel][localA] : 0.0f;
                const float b =
                    localB < readFrames ? input[channel][localB] : 0.0f;
                output[channel][frame] =
                    static_cast<float>(a + (b - a) * fraction);
            }
        }

        // 位置按完整输出块推进，越过源尾部的帧也占用目标静音长度。
        m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
    }

private:
    /// @brief 保持完整解码缓存存活的输入音轨。
    std::shared_ptr<ice::AudioTrack> m_track;
    /// @brief 每个输出帧跨越的源帧数。
    double m_speed{ 1.0 };
    /// @brief 输入输出统一使用的引擎内部格式。
    ice::AudioDataFormat m_format;
    /// @brief 离线插值按块复用的源 PCM 缓冲。
    ice::AudioBuffer m_sourceBuffer;
    /// @brief 下一输出帧对应的连续源坐标。
    double m_sourcePosition{ 0.0 };
};

/// @brief 计算导出目标帧数。
/// @param inputFrames 输入音频帧数。
/// @param sampleRate 输出采样率。
/// @param options 导出参数。
/// @return 输出帧数。
///
/// 理论长度使用 ceil(inputFrames / speed)，至少输出一帧以形成合法容器。
/// minimumDurationSeconds 转成同采样率帧数后只取 max，不会截短正常倍速结果。
std::size_t calculateTargetFrames(std::size_t                    inputFrames,
                                  std::uint32_t                  sampleRate,
                                  const AudioSpeedExportOptions& options)
{
    // ceil 保证最后一个不完整源区间不会因向下取整而被截掉。
    const auto speedFrames = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::ceil(static_cast<double>(inputFrames) / options.speed)));
    if ( options.minimumDurationSeconds <= 0.0 ||
         !std::isfinite(options.minimumDurationSeconds) ) {
        return speedFrames;
    }

    // 最小时长只作为下限，编码器会从已经结束的图继续取得静音。
    const auto minimumFrames = static_cast<std::size_t>(std::ceil(
        options.minimumDurationSeconds * static_cast<double>(sampleRate)));
    return std::max(speedFrames, minimumFrames);
}

/// @brief 创建保留音高的音频图。
/// @param track 输入音轨。
/// @param speed 倍速倍率。
/// @return 图的输出节点。
///
/// SourceNode 的同块 final 通知让 TimeStretcher 及时刷新算法尾部，不依赖额外
/// 空拉取触发结束。prepare 失败时返回空图并由服务生成统一错误。
std::shared_ptr<ice::IAudioNode> createPitchPreservedGraph(
    const std::shared_ptr<ice::AudioTrack>& track, double speed)
{
    // SourceNode 提供顺序 PCM，TimeStretcher 负责状态化变速与尾部刷新。
    auto source = std::make_shared<ice::SourceNode>(track);
    source->setvolume(1.0f);
    source->play();

    auto stretcher = std::make_shared<ice::TimeStretcher>();
    stretcher->set_inputnode(source);
    if ( !stretcher->prepare(ice::ICEConfig::internal_format,
                             AUDIO_SPEED_EXPORT_CHUNK_FRAMES) ) {
        return {};
    }
    stretcher->set_playback_ratio(speed);
    stretcher->set_pitch_semitones(0.0);
    // 同块 final 让 stretcher 在目标帧数内及时提交剩余窗，不依赖额外空拉取。
    source->set_final_input_listener(stretcher.get(),
                                     &requestFinalStretcherInput);
    return stretcher;
}

/// @brief 创建不保留音高的音频图。
/// @param track 输入音轨。
/// @param speed 倍速倍率。
/// @param format 引擎内部音频格式。
/// @return 图的输出节点。
///
/// 此路径不需要附加效果节点，采样节点自己读取缓存并执行线性插值。
std::shared_ptr<ice::IAudioNode> createPitchShiftedGraph(
    const std::shared_ptr<ice::AudioTrack>& track, double speed,
    const ice::AudioDataFormat& format)
{
    return std::make_shared<PitchShiftSpeedNode>(track, speed, format);
}

}  // namespace

/// @brief 按指定速度离线导出音频并可选保持音高。
/// @param options 输入输出路径、速度、音高策略、最小时长和进度回调。
/// @return 实际编码帧数、时长、成功状态与失败原因。
///
/// 结果时长始终由实际 frames_written 与内部采样率计算，不用理论速度反推。
///
/// 阶段执行顺序为：
///
/// - 校验速度、路径与输入文件；
/// - 获取应用共享线程池并完整解码音轨；
/// - 验证内部声道数和采样率；
/// - 计算理论倍速长度与可选最小时长下限；
/// - 根据 preservePitch 构建对应处理图；
/// - 配置 Receiver 的目标帧、块大小和进度映射；
/// - 同步编码并以实际写出帧数生成结果。
///
/// 输出格式由 outputPath 扩展名交给 FFmpegFileReceiver 选择。
/// @warning 低频导出路径；包含文件系统、完整解码、DSP 与编码。
AudioSpeedExportResult AudioSpeedExportService::exportWav(
    const AudioSpeedExportOptions& options)
{
    // 默认失败，所有环境校验在创建输出编码器前完成。
    AudioSpeedExportResult result;
    // 速度必须为有限正数，防止目标帧除零及源位置无法前进。
    if ( options.speed <= 0.0 || !std::isfinite(options.speed) ) {
        result.errorMessage = "Invalid speed multiplier";
        return result;
    }
    if ( options.inputPath.empty() || options.outputPath.empty() ) {
        result.errorMessage = "Input or output audio path is empty";
        return result;
    }

    // error_code 检查避免文件系统异常穿过项目禁用异常的边界。
    std::error_code existsError;
    if ( !std::filesystem::is_regular_file(options.inputPath, existsError) ||
         existsError ) {
        result.errorMessage = "Input audio file does not exist";
        return result;
    }

    emitProgress(options, 0.0f, "正在打开音频...");

    ice::ThreadPool* threadPool = Runtime::AppThreadPool::instance().get();
    if ( !threadPool ) {
        result.errorMessage = "Runtime thread pool is not initialized";
        return result;
    }

    // 使用 CACHY 完整缓存，保证两种处理图都可随机或顺序读取全部源帧。
    auto decoderFactory = std::make_shared<ice::FFmpegDecoderFactory>();
    auto track = ice::AudioTrack::create(Config::pathToUtf8(options.inputPath),
                                         *threadPool,
                                         decoderFactory,
                                         ice::CachingStrategy::CACHY);
    if ( !track ) {
        result.errorMessage = "Failed to decode input audio";
        return result;
    }

    // 解码后仍验证输出图格式，Receiver 和节点都依赖非零声道与采样率。
    const ice::AudioDataFormat format = ice::ICEConfig::internal_format;
    if ( format.channels == 0 || format.samplerate == 0 ) {
        result.errorMessage = "Invalid internal audio format";
        return result;
    }

    emitProgress(options, 0.05f, "正在载入音频缓存...");
    const std::size_t inputFrames = track->num_frames();
    if ( inputFrames == 0 ) {
        result.errorMessage = "Input audio is empty";
        return result;
    }

    // 统一目标帧数使保持音高与变调路径具有相同时长契约。
    const std::size_t targetFrames =
        calculateTargetFrames(inputFrames, format.samplerate, options);
    // 两条图只在 DSP 算法上不同，后续目标长度与编码路径完全一致。
    auto graph = options.preservePitch
                     ? createPitchPreservedGraph(track, options.speed)
                     : createPitchShiftedGraph(track, options.speed, format);
    if ( !graph ) {
        result.errorMessage = "Failed to create audio export graph";
        return result;
    }

    emitProgress(options, 0.08f, "正在通过音频引擎导出...");

    ice::FFmpegFileReceiver receiver(options.outputPath, format);
    receiver.set_source(graph);
    receiver.set_target_frames(targetFrames);
    // 两种处理图均按相同离线块大小拉取，避免结果依赖 Receiver 默认值。
    receiver.set_block_frames(AUDIO_SPEED_EXPORT_CHUNK_FRAMES);
    // Receiver 按已编码帧数映射进度，分母至少为一以覆盖最小空输出。
    receiver.set_progress_callback(
        [&options, targetFrames](std::size_t frames) {
            const float progress =
                0.08f +
                0.90f * static_cast<float>(frames) /
                    static_cast<float>(std::max<std::size_t>(1, targetFrames));
            emitProgress(options, progress, "正在编码倍速音频...");
        });

    // 后端诊断优先于通用错误，使缺少编码器等环境问题可直接定位。
    if ( !receiver.start() ) {
        result.errorMessage = receiver.error_message().empty()
                                  ? "Failed to encode output audio"
                                  : receiver.error_message();
        return result;
    }

    // 完成后以 Receiver 的真实计数生成结果和最终百分之百进度。
    result.outputFrames          = receiver.frames_written();
    result.outputDurationSeconds = static_cast<double>(result.outputFrames) /
                                   static_cast<double>(format.samplerate);
    emitProgress(options, 1.0f, "倍速音频导出完成");
    XINFO("AudioSpeedExportService: wrote {} frames to {}",
          result.outputFrames,
          Config::pathToUtf8(options.outputPath));
    result.success = true;
    return result;
}

}  // namespace MMM::Audio
