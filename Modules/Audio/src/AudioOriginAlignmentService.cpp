#include "audio/AudioOriginAlignmentService.h"

#include "audio/AudioTimelineExportService.h"
#include "config/Utf8Path.h"

#include <cmath>

namespace MMM::Audio
{

/// @brief 复用离线时间线导出器完成音频首拍原点对齐。
/// @param options 输入输出路径及首拍相对原点的有符号相位。
/// @return 导出成功状态、错误原因和最终帧数。
///
/// 相位转换为单事件的负起始时间：正相位让事件从零点前开始，从而裁掉开头；
/// 负相位让事件延后开始，从而由时间线导出器在开头生成等长静音。
/// 本服务强制单位音量、原速、原调且关闭 EQ，确保操作只改变时间原点。
/// 输出编码格式仍由 outputPath 扩展名决定，本层不改变导出器的格式选择策略。
/// 输入与输出路径允许不同文件系统表示，资源事件使用统一 UTF-8 路径传递。
/// phaseMilliseconds 必须有限，避免秒换算后污染帧位置计算和导出长度。
/// 底层服务的成功状态、错误文本和实际帧数逐项转发，不重新解释部分结果。
/// 服务不原地修改输入文件；是否允许同路径由底层安全导出策略统一判定。
/// @warning 离线文件处理路径；可能解码、编码和等待后台任务。
AudioOriginAlignmentResult AudioOriginAlignmentService::alignToOrigin(
    const AudioOriginAlignmentOptions& options)
{
    // 在触发任何文件访问前验证基本输入，失败结果保持 outputFrames 为零。
    AudioOriginAlignmentResult result;
    if ( options.inputPath.empty() || options.outputPath.empty() ) {
        result.errorMessage = "音频原点对齐的输入或输出路径为空";
        return result;
    }
    if ( !std::isfinite(options.phaseMilliseconds) ) {
        result.errorMessage = "音频原点对齐相位无效";
        return result;
    }

    // 显式覆盖全部资源级参数，避免调用方配置或未来默认值改变源音色。
    AudioTrackConfig unityConfig;
    unityConfig.volume        = 1.0F;
    unityConfig.playbackSpeed = 1.0F;
    unityConfig.playbackPitch = 0.0F;
    unityConfig.muted         = false;
    unityConfig.eqEnabled     = false;

    // 单事件时间线已经能同时表达裁头与前置静音，无需复制两套 PCM 算法。
    AudioTimelineExportOptions timelineOptions;
    timelineOptions.events.push_back(AudioTimelineLoadEvent{
        .eventId     = 1U,
        .resourceKey = "mcz-main-audio-origin-alignment",
        .filePath    = Config::pathToUtf8(options.inputPath),
        // 时间线起点与相位符号相反：首拍偏晚时需要把音频整体向左移动。
        .effectiveStartSeconds = -options.phaseMilliseconds / 1000.0,
        .bgmTrackIndex         = 0U,
        .eventVolume           = 1.0F,
        .resourceConfig        = std::move(unityConfig),
    });
    timelineOptions.outputPath = options.outputPath;

    // 原样转交底层导出的诊断，调用者可以统一展示编码或资源加载错误。
    const auto timelineResult =
        AudioTimelineExportService::exportMixedAudio(timelineOptions);
    // 三个返回字段来自同一次底层调用，保持错误与输出长度的因果对应关系。
    result.success      = timelineResult.success;
    result.errorMessage = timelineResult.errorMessage;
    result.outputFrames = timelineResult.outputFrames;
    return result;
}

}  // namespace MMM::Audio
