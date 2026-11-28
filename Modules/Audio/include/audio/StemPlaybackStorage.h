#pragma once

#include "audio/StemAudition.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace MMM::Audio
{
/// @brief 保存分离任务起点的源文件身份，以免结果关联到中途被替换的文件。
struct StemSourceIdentity {
    /// @brief 解析链接后的绝对 UTF-8 路径。
    std::string path;
    /// @brief 原音频文件大小。
    std::uintmax_t size{};
    /// @brief 以本机文件系统时钟表达的原音频修改时间。
    std::string modified;

    bool operator==(const StemSourceIdentity&) const = default;
};

/// @brief 查询稳定路径及轻量文件版本，文件缺失时返回空。
/// @warning 后台文件操作；不得在渲染或音频回调调用。
[[nodiscard]] std::optional<StemSourceIdentity> readStemSourceIdentity(
    const std::string& filePath);

/// @brief 发布一组已解码验证的 WAV 和初始调音台，返回本代调音台配置路径。
/// @warning 后台全文件复制。当前指针最后提交，失败不会覆盖上一组完整产物。
[[nodiscard]] std::filesystem::path storeStemPlaybackResult(
    const StemSourceIdentity& identity, std::span<const std::string> files,
    const StemPlaybackMix& mix, std::stop_token stop = {});

/// @brief 加载同一源版本已保存的分轨与调音台，无有效产物则返回空。
/// @warning 后台文件与解码任务；decodePool 必须与执行本函数的池分离。
[[nodiscard]] std::optional<StemPlaybackMix> loadStoredStemPlayback(
    const std::string& filePath, ice::ThreadPool& decodePool);

/// @brief 原子更新指定分轨代次的调音台配置。
/// @warning 仅后台文件任务调用；不存在的结果路径不会被重新创建。
[[nodiscard]] bool storeStemPlaybackSettings(const StemPlaybackMix& mix);
}  // namespace MMM::Audio
