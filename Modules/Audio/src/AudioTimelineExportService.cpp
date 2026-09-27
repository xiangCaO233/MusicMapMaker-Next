#include "audio/AudioTimelineExportService.h"

#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/out/io/FFmpegFileReceiver.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Audio
{
namespace
{

// 离线时间线导出复用实时 MixerNode，但在控制线程一次性完成全部资源准备：
//
// - 输入事件先按文件路径去重解码，避免同一资源建立多份 AudioTrack；
// - 资源级速度、音高和 EQ 再按处理配置键去重，避免重复执行离线 DSP；
// - 事件起点转换为统一采样率的有符号帧，允许负起点表达裁头；
// - 资源音量与事件音量在构造片段前相乘，静音资源直接使用零增益；
// - MixerNode 负责重叠、负起点和复合结束位置，导出器不复制混音算法；
// - FFmpegFileReceiver 按固定大块拉取节点并编码到目标扩展名；
// - 任一步失败立即返回具体原因，不把部分输出报告为成功。
//
// 事件遍历保持输入顺序，MixerNode 会在构造期按起点与 eventId 建立确定调度。
// 本服务不修改源文件，也不把生成物写入测试或项目资源目录之外的指定路径。
// 资源解码与 DSP 失败不会继续编码，避免留下看似成功但内容不完整的文件。
// 进度文本只表示已进入阶段，不把开始阶段误报为该阶段已经完成。
// 成功帧数来自编码器实际写入统计，可用于上层计算最终文件时长。

/// @brief 离线复合时间线单次处理的最大帧数。
constexpr std::size_t AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES = 65536U;

/// @brief 将秒数安全转换为统一时间线帧。
/// @param seconds 秒数，允许为负。
/// @return 限制在 AudioTimelineFrame 可表达范围内的最近帧。
///
/// 使用 long double 完成乘法，推迟大秒数在转换前丢失精度的时点。llround 采用
/// 最近帧而非截断，使正负小数秒在时间线原点两侧保持对称舍入。
AudioTimelineFrame secondsToTimelineFrame(double seconds) noexcept
{
    // 非有限时间没有可用排序语义，安全回退到时间线原点。
    if ( !std::isfinite(seconds) ) return 0;

    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    // 在转换回整数前饱和，避免 llround 的输入越出返回类型范围。
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 规范化项目音频资源音量。
/// @param volume 持久化线性音量。
/// @return 有限的零到一音量。
///
/// 资源音量来自项目配置，公开语义为百分比；异常值选择最近合法边界或零。
float sanitizedResourceVolume(float volume) noexcept
{
    // 项目资源音量是百分比语义，不允许事件文件把它放大到一以上。
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 规范化单个事件音量。
/// @param volume 事件线性音量。
/// @return 有限的非负音量。
///
/// 事件格式可能允许大于一的强调倍率，因此这里只限制下界，不截去合法放大。
float sanitizedEventVolume(float volume) noexcept
{
    // 事件层允许正向放大，但负值和非有限值都按静音处理。
    return std::isfinite(volume) ? std::max(volume, 0.0F) : 0.0F;
}

}  // namespace

/// @brief 把全部自动采样事件离线混合并编码为一个音频文件。
/// @param options 事件、谱面结束时间、输出路径与阶段进度回调。
/// @return 编码成功状态、诊断文本和实际写出帧数。
///
/// 函数按“校验环境、加载资源、准备 DSP、构造时间线、编码”五阶段执行。
/// 所有资源使用 CACHY 完整缓存，保证后续离线处理和 MixerNode 拉取不会缺页。
///
/// 缓存层级保持以下身份关系：
///
/// - tracksByPath 的键是事件使用的 UTF-8 文件路径；
/// - preparedByConfig 的键包含路径以及速度、音高、EQ 等资源级参数；
/// - clips 仍按事件逐项创建，保留 eventId、起点、BGM 轨和事件音量；
/// - 多事件可共享 PreparedTimelineAudio，但不会因此合并时序身份；
/// - MixerNode 的结束帧取 chartEndSeconds 与全部实际片段尾部的最大值。
///
/// 输出父目录按需创建，源资源存在性在解码前逐一确认。
/// 空事件列表仍可按 chartEndSeconds 导出静音；若结束时间也为零则生成一帧
/// 最小输出，保证容器编码器拥有可提交的数据。targetFrames 在转换到 size_t 前
/// 再次钳位，避免极端有符号时间线长度在平台位宽间溢出。
/// @warning 用户触发的低频导出路径；包含文件系统、解码、DSP 与编码操作。
AudioTimelineExportResult AudioTimelineExportService::exportMixedAudio(
    const AudioTimelineExportOptions& options)
{
    // 默认失败且帧数为零，只有编码器确实写出数据后才提交 success。
    AudioTimelineExportResult result;
    if ( options.outputPath.empty() ) {
        result.errorMessage = "音频输出路径为空";
        return result;
    }

    // 整条图统一使用内部格式，避免事件之间发生不同采样率或声道布局混合。
    const ice::AudioDataFormat format = ice::ICEConfig::internal_format;
    if ( format.channels == 0U || format.samplerate == 0U ) {
        result.errorMessage = "音频引擎内部格式无效";
        return result;
    }

    ice::ThreadPool* threadPool = Runtime::AppThreadPool::instance().get();
    if ( !threadPool ) {
        result.errorMessage = "音频后台线程池尚未初始化";
        return result;
    }

    // 使用 error_code 路径表达失败，符合项目禁用异常的约束。
    std::error_code filesystemError;
    if ( !options.outputPath.parent_path().empty() ) {
        std::filesystem::create_directories(options.outputPath.parent_path(),
                                            filesystemError);
        if ( filesystemError ) {
            result.errorMessage = "无法创建音频输出目录";
            return result;
        }
    }

    // 进度回调只在阶段边界调用，不从音频拉取或编码内部热循环调用。
    if ( options.progress ) options.progress("正在解码音频资源…");
    auto decoderFactory = std::make_shared<ice::FFmpegDecoderFactory>();
    std::unordered_map<std::string, std::shared_ptr<ice::AudioTrack>>
        tracksByPath;
    // 最坏每个事件对应不同文件，按事件数预留可避免 rehash。
    tracksByPath.reserve(options.events.size());
    // 第一遍只处理文件身份；事件位置、音量和 DSP 配置留到第二遍。
    for ( const auto& event : options.events ) {
        // 空路径保留 resourceKey 进入诊断，便于定位未解析的谱面资源引用。
        if ( event.filePath.empty() ) {
            result.errorMessage = "存在未解析的音频资源：" + event.resourceKey;
            return result;
        }
        // 同一路径的多个事件共享完整缓存，但仍在下一阶段生成独立片段。
        if ( tracksByPath.contains(event.filePath) ) continue;

        const auto path = Config::utf8ToPath(event.filePath);
        filesystemError.clear();
        // 目录或特殊文件都不能交给解码器，统一按资源缺失处理。
        if ( !std::filesystem::is_regular_file(path, filesystemError) ||
             filesystemError ) {
            result.errorMessage = "找不到音频资源：" + event.filePath;
            return result;
        }
        // 完整缓存是资源级离线 DSP 和确定性导出的前置条件。
        auto track = ice::AudioTrack::create(event.filePath,
                                             *threadPool,
                                             decoderFactory,
                                             ice::CachingStrategy::CACHY);
        if ( !track || track->num_frames() == 0U ) {
            result.errorMessage = "无法解码音频资源：" + event.filePath;
            return result;
        }
        tracksByPath.emplace(event.filePath, std::move(track));
    }

    if ( options.progress ) options.progress("正在处理音频时间线…");
    std::unordered_map<std::string,
                       std::shared_ptr<const PreparedTimelineAudio>>
        preparedByConfig;
    // 路径相同但处理配置不同必须生成不同缓存，因此使用处理键而非仅用路径。
    // 第二遍按处理配置共享不可变 PCM，再为每个事件保留独立时间线身份。
    preparedByConfig.reserve(options.events.size());
    std::vector<PreparedTimelineClip> clips;
    clips.reserve(options.events.size());
    for ( const auto& event : options.events ) {
        // 事件起点异常会破坏排序与结束帧计算，不能静默回退到零。
        if ( !std::isfinite(event.effectiveStartSeconds) ) {
            result.errorMessage = "音频事件起播时间无效：" + event.resourceKey;
            return result;
        }

        const auto trackIterator = tracksByPath.find(event.filePath);
        if ( trackIterator == tracksByPath.end() || !trackIterator->second ) {
            result.errorMessage = "音频资源未完成载入：" + event.filePath;
            return result;
        }

        // 缓存键覆盖会改变 PCM 的资源配置，事件位置和事件音量不参与。
        const auto processingKey = makeAudioResourceProcessingCacheKey(
            event.filePath, event.resourceConfig);
        auto preparedIterator = preparedByConfig.find(processingKey);
        if ( preparedIterator == preparedByConfig.end() ) {
            // 只有首个相同处理键执行离线 DSP，后续事件共享其结果。
            auto prepared = prepareAudioTimelineResource(trackIterator->second,
                                                         event.resourceConfig);
            if ( !prepared ) {
                result.errorMessage = "无法处理音频资源：" + event.filePath;
                return result;
            }
            preparedIterator =
                preparedByConfig.emplace(processingKey, std::move(prepared))
                    .first;
        }

        // 资源静音在混音音量层实现，不妨碍共享同一份已处理 PCM。
        const float resourceVolume =
            event.resourceConfig.muted
                ? 0.0F
                : sanitizedResourceVolume(event.resourceConfig.volume);
        // 起点允许为负；MixerNode 会在零点输出时裁去相应源前缀。
        clips.push_back(PreparedTimelineClip{
            .eventId    = event.eventId,
            .sourceKey  = event.resourceKey,
            .startFrame = secondsToTimelineFrame(event.effectiveStartSeconds),
            .bgmTrackIndex = event.bgmTrackIndex,
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .audio  = preparedIterator->second,
        });
    }

    // 无效或负谱面结束时间按零处理，实际片段尾部仍可扩展复合结束帧。
    const double chartEndSeconds = std::isfinite(options.chartEndSeconds)
                                       ? std::max(options.chartEndSeconds, 0.0)
                                       : 0.0;
    // 节点构造会过滤空资源并把结束位置扩展到最晚片段尾部。
    auto timeline = std::make_shared<AudioTimelineMixerNode>(
        std::move(clips),
        secondsToTimelineFrame(chartEndSeconds),
        AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES);
    timeline->play();

    // 编码器至少拉取一帧，空时间线也能生成格式合法的最小输出。
    const auto timelineEndFrame = timeline->timelineEndFrame();
    const auto positiveEndFrame =
        timelineEndFrame > 0 ? static_cast<std::uint64_t>(timelineEndFrame)
                             : std::uint64_t{ 1U };
    const auto targetFrames = static_cast<std::size_t>(std::min(
        positiveEndFrame,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));

    // Receiver 按目标帧数停止拉取，不依赖节点结束后的额外空 block。
    ice::FFmpegFileReceiver receiver(options.outputPath, format);
    receiver.set_source(timeline);
    receiver.set_target_frames(targetFrames);
    // 与 MixerNode scratch 使用同一块大小，避免 Receiver 请求超过预备容量。
    receiver.set_block_frames(AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES);
    if ( options.progress ) options.progress("正在混音并转码音频…");
    // start 同步完成离线拉取和编码；失败信息优先使用后端具体诊断。
    if ( !receiver.start() ) {
        result.errorMessage = receiver.error_message().empty()
                                  ? "无法编码拼装后的音频"
                                  : receiver.error_message();
        return result;
    }

    // 以实际写出帧数作为最终事实，不能只凭 start 返回值宣告成功。
    result.outputFrames = receiver.frames_written();
    result.success      = result.outputFrames > 0U;
    // start 成功但零帧仍视为失败，防止产生只有头部的无内容输出。
    if ( !result.success ) {
        result.errorMessage = "音频编码器未写出任何数据";
        return result;
    }

    // 成功日志使用 UTF-8 路径，保持跨平台输出可读。
    XINFO("AudioTimelineExportService: wrote {} frames to {}",
          result.outputFrames,
          Config::pathToUtf8(options.outputPath));
    return result;
}

}  // namespace MMM::Audio
