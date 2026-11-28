#pragma once

#include "audio/StemAudition.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

/// @brief 单个主音轨的本机缓存状态，包含已发布和旧代次的文件。
struct StemPlaybackCacheInfo {
    /// @brief 稳定模型键。空键代表升级前未标记模型的缓存。
    std::string modelKey;
    /// @brief 分离时记录的模型显示名。
    std::string modelName;
    /// @brief 可在文件管理器中定位的 UTF-8 绝对路径。
    std::string directory;
    /// @brief 缓存内所有常规文件的字节数，不计算链接目标。
    std::uintmax_t bytes{};
    /// @brief 清单匹配当前原曲版本时允许尝试恢复。
    bool reusable{};
};

/// @brief 从模型路径和声部计划生成安全的目录键。
[[nodiscard]] std::string makeStemCacheModelKey(std::string_view modelIdentity);

/// @brief 查询指定主轨各模型的缓存路径和文件总大小。
/// @warning 后台目录遍历；源不可读或统计失败时返回空。
[[nodiscard]] std::optional<std::vector<StemPlaybackCacheInfo>>
readStemPlaybackCache(const std::string& filePath);

/// @brief 清除一个模型的所有代次，返回该模型是否原本被选中。
/// @warning 后台文件操作；旧格式使用空模型键。
[[nodiscard]] std::expected<bool, std::error_code> removeStemPlaybackCache(
    const std::string& filePath, std::string_view modelKey);

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
    const std::string& filePath, ice::ThreadPool& decodePool,
    std::optional<std::string_view> modelKey = std::nullopt);

/// @brief 保存当前所选模型，重开时恢复其缓存及调音台。
/// @warning 后台文件操作；已经删除的代次不可被重新设为当前结果。
[[nodiscard]] bool selectStoredStemPlayback(const std::string&     filePath,
                                            const StemPlaybackMix& mix);

/// @brief 原子更新指定分轨代次的调音台配置。
/// @warning 仅后台文件任务调用；不存在的结果路径不会被重新创建。
[[nodiscard]] bool storeStemPlaybackSettings(const StemPlaybackMix& mix);
}  // namespace MMM::Audio
