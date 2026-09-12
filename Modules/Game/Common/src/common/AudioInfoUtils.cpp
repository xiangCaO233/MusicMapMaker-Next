#include "common/AudioInfoUtils.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <ice/manage/dec/MediaInfo.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <system_error>

namespace MMM::Utils
{

/// @brief 使用解码器工厂探测音频文件的展示元数据和时长。
/// @param filePath 待探测音频文件路径。
/// @return 探测成功时返回完整 AudioInfo，否则返回空。
/// @warning
/// 本函数包含文件系统访问和媒体解码探测，不得在音频回调或每帧路径调用。
std::optional<AudioInfo> AudioInfoUtils::probeAudioInfo(
    const std::filesystem::path& filePath)
{
    // 使用 error_code 重载避免文件系统异常越过项目的无异常边界。
    std::error_code filesystemError;
    if ( !std::filesystem::exists(filePath, filesystemError) ||
         filesystemError ) {
        // 不存在和查询失败都无法继续探测，日志保留用户可识别的 UTF-8 路径。
        // 两种失败共享空返回值，调用方无需区分竞态删除和权限错误。
        XERROR("AudioInfoUtils: File not found: {}",
               Config::pathToUtf8(filePath));
        return std::nullopt;
    }

    // 工厂在当前调用内完成轻量探测，结果复制到模块自有 DTO。
    ice::FFmpegDecoderFactory decoderFactory;
    ice::MediaInfo            mediaInfo;
    // 所有跨平台路径都先经过统一 UTF-8 转换，再交给 FFmpeg 后端。
    const std::string audioPath = Config::pathToUtf8(filePath);
    // 解码器拒绝文件时不返回部分元数据，避免调用方误认为探测完整成功。
    if ( !decoderFactory.probe(audioPath, mediaInfo) ) {
        XERROR("AudioInfoUtils: Failed to probe audio metadata for {}",
               audioPath);
        return std::nullopt;
    }

    // 字符串按值复制，返回对象不依赖解码器工厂和 MediaInfo 生命周期。
    AudioInfo info;
    info.title  = mediaInfo.title;
    info.artist = mediaInfo.artist;
    // 只有正采样率才能安全地把 PCM 帧数换算为秒。
    // 无效或缺失采样率保留默认零时长，不执行除法也不猜测容器时长。
    if ( mediaInfo.format.samplerate > 0 ) {
        // frame_count 与 samplerate 使用浮点除法，保留不足一秒的精度。
        info.duration = static_cast<double>(mediaInfo.frame_count) /
                        static_cast<double>(mediaInfo.format.samplerate);
    }

    // 缺少标题标签时使用文件名主干，保证项目列表始终有可见名称。
    if ( info.title.empty() ) {
        // stem 去除扩展名但保留目录外的名称，并再次转换为 UTF-8。
        info.title = Config::pathToUtf8(filePath.stem());
    }

    // 成功日志同时记录最终回退后的标题与计算时长，便于定位元数据差异。
    // 日志只使用文件名，避免在正常成功路径重复输出完整目录结构。
    XINFO(
        "AudioInfoUtils: Probed info for {}: Title={}, Artist={}, "
        "Duration={:.2f}s",
        Config::pathToUtf8(filePath.filename()),
        info.title,
        info.artist,
        info.duration);

    // 返回纯值对象，调用方可安全缓存而无需持有解码器资源。
    return info;
}

}  // namespace MMM::Utils
