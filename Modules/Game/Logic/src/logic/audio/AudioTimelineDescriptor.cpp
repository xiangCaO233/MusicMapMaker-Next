#include "logic/audio/AudioTimelineDescriptor.h"

#include "config/Utf8Path.h"
#include "logic/ProjectResourceService.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{

constexpr std::uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr std::uint64_t FNV1A_PRIME        = 1099511628211ULL;
constexpr std::uint64_t DOUBLE_SIGN_BIT    = 0x8000000000000000ULL;
constexpr std::uint32_t FLOAT_SIGN_BIT     = 0x80000000U;

using CanonicalBytes = std::vector<std::uint8_t>;

/// @brief 尚未赋予稳定事件 ID 的规范音频事件。
struct PendingTimelineEvent {
    /// @brief 交给 AudioManager 的加载事件。
    MMM::Audio::AudioTimelineLoadEvent m_event;

    /// @brief 用于排序、事件 ID 和指纹的完整听觉语义字节。
    CanonicalBytes m_canonicalBytes;

    /// @brief 谱面中保存的原始资源引用。
    std::string m_originalReference;

    /// @brief 原始引用是否未能解析为项目资源。
    bool m_unresolved{ false };
};

/// @brief 将无符号整数按大端序追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加的整数。
void appendUnsigned(CanonicalBytes& bytes, std::uint64_t value)
{
    for ( int shift = 56; shift >= 0; shift -= 8 ) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 获取跨平台稳定且可按数值顺序比较的 double 位模式。
/// @param value 待规范化数值。
/// @return 统一零值和 NaN 后的可排序无符号位模式。
std::uint64_t sortableDoubleBits(double value) noexcept
{
    std::uint64_t bits = 0U;
    if ( std::isnan(value) ) {
        bits = 0x7FF8000000000000ULL;
    } else if ( value != 0.0 ) {
        bits = std::bit_cast<std::uint64_t>(value);
    }
    return (bits & DOUBLE_SIGN_BIT) != 0U ? ~bits : bits ^ DOUBLE_SIGN_BIT;
}

/// @brief 获取跨平台稳定且可按数值顺序比较的 float 位模式。
/// @param value 待规范化数值。
/// @return 统一零值和 NaN 后的可排序无符号位模式。
std::uint32_t sortableFloatBits(float value) noexcept
{
    std::uint32_t bits = 0U;
    if ( std::isnan(value) ) {
        bits = 0x7FC00000U;
    } else if ( value != 0.0F ) {
        bits = std::bit_cast<std::uint32_t>(value);
    }
    return (bits & FLOAT_SIGN_BIT) != 0U ? ~bits : bits ^ FLOAT_SIGN_BIT;
}

/// @brief 将 double 追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加数值。
void appendDouble(CanonicalBytes& bytes, double value)
{
    appendUnsigned(bytes, sortableDoubleBits(value));
}

/// @brief 将 float 追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加数值。
void appendFloat(CanonicalBytes& bytes, float value)
{
    appendUnsigned(bytes, sortableFloatBits(value));
}

/// @brief 将有符号整数追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加数值。
void appendSigned(CanonicalBytes& bytes, std::int64_t value)
{
    appendUnsigned(bytes, static_cast<std::uint64_t>(value) ^ DOUBLE_SIGN_BIT);
}

/// @brief 将长度前缀 UTF-8 字符串追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加字符串。
void appendString(CanonicalBytes& bytes, std::string_view value)
{
    appendUnsigned(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

/// @brief 将完整音轨配置追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param config 待追加的持久化音轨配置。
void appendTrackConfig(CanonicalBytes& bytes, const AudioTrackConfig& config)
{
    appendFloat(bytes, config.volume);
    appendFloat(bytes, config.playbackSpeed);
    appendFloat(bytes, config.playbackPitch);
    appendUnsigned(bytes, config.muted ? 1U : 0U);
    appendUnsigned(bytes, config.eqEnabled ? 1U : 0U);
    appendSigned(bytes, static_cast<std::int64_t>(config.eqPreset));

    appendUnsigned(bytes,
                   static_cast<std::uint64_t>(config.eqBandGains.size()));
    for ( const float gain : config.eqBandGains ) {
        appendFloat(bytes, gain);
    }

    appendUnsigned(bytes, static_cast<std::uint64_t>(config.eqBandQs.size()));
    for ( const float q : config.eqBandQs ) {
        appendFloat(bytes, q);
    }
}

/// @brief 构造单个加载事件的完整听觉语义字节。
/// @param event 待规范化加载事件。
/// @return 不含画布轨道等纯视觉字段的规范字节。
CanonicalBytes makeCanonicalEventBytes(
    const MMM::Audio::AudioTimelineLoadEvent& event)
{
    CanonicalBytes bytes;
    bytes.reserve(128U + event.resourceKey.size() + event.filePath.size() +
                  (event.resourceConfig.eqBandGains.size() +
                   event.resourceConfig.eqBandQs.size()) *
                      sizeof(std::uint64_t));
    appendDouble(bytes, event.effectiveStartSeconds);
    appendString(bytes, event.resourceKey);
    appendString(bytes, event.filePath);
    appendFloat(bytes, event.eventVolume);
    appendTrackConfig(bytes, event.resourceConfig);
    return bytes;
}

/// @brief 将项目资源路径解析为规范化绝对路径。
/// @param project 音频资源所属项目。
/// @param beatmapPath 当前谱面路径，用作项目根缺失时的回退基准。
/// @param storedPath 项目资源保存的 UTF-8 路径。
/// @return 规范化绝对路径；空资源路径返回空。
/// @warning 低频描述符路径：可能访问文件系统以消解现有路径前缀。
std::filesystem::path resolveAbsoluteResourcePath(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::string& storedPath)
{
    if ( storedPath.empty() ) return {};

    auto resourcePath = Config::utf8ToPath(storedPath);
    if ( resourcePath.is_relative() ) {
        auto basePath = project.m_projectRoot;
        if ( basePath.empty() && !beatmapPath.empty() ) {
            auto            absoluteBeatmapPath = beatmapPath;
            std::error_code filesystemError;
            if ( absoluteBeatmapPath.is_relative() ) {
                const auto converted = std::filesystem::absolute(
                    absoluteBeatmapPath, filesystemError);
                if ( !filesystemError ) {
                    absoluteBeatmapPath = converted;
                }
            }
            basePath = absoluteBeatmapPath.parent_path();
        }

        if ( !basePath.empty() && basePath.is_relative() ) {
            std::error_code filesystemError;
            const auto      converted =
                std::filesystem::absolute(basePath, filesystemError);
            if ( !filesystemError ) basePath = converted;
        }
        if ( !basePath.empty() ) resourcePath = basePath / resourcePath;
    }

    if ( resourcePath.is_relative() ) {
        std::error_code filesystemError;
        const auto      converted =
            std::filesystem::absolute(resourcePath, filesystemError);
        if ( !filesystemError ) resourcePath = converted;
    }

    std::error_code filesystemError;
    const auto      canonicalPath =
        std::filesystem::weakly_canonical(resourcePath, filesystemError);
    if ( !filesystemError ) resourcePath = canonicalPath;
    return resourcePath.lexically_normal();
}

/// @brief 向 FNV-1a 状态追加单字节。
/// @param state 当前哈希状态。
/// @param byte 待追加字节。
void updateFnv1a(std::uint64_t& state, std::uint8_t byte) noexcept
{
    state ^= byte;
    state *= FNV1A_PRIME;
}

/// @brief 向 FNV-1a 状态追加一段字节。
/// @param state 当前哈希状态。
/// @param bytes 待追加字节。
void updateFnv1a(std::uint64_t& state, const CanonicalBytes& bytes) noexcept
{
    for ( const std::uint8_t byte : bytes ) {
        updateFnv1a(state, byte);
    }
}

/// @brief 向 FNV-1a 状态追加 ASCII 域标识。
/// @param state 当前哈希状态。
/// @param domain 待追加域标识。
void updateFnv1a(std::uint64_t& state, std::string_view domain) noexcept
{
    for ( const char character : domain ) {
        updateFnv1a(state, static_cast<std::uint8_t>(character));
    }
    updateFnv1a(state, static_cast<std::uint8_t>(0U));
}

/// @brief 向 FNV-1a 状态追加大端序无符号整数。
/// @param state 当前哈希状态。
/// @param value 待追加整数。
void updateFnv1a(std::uint64_t& state, std::uint64_t value) noexcept
{
    for ( int shift = 56; shift >= 0; shift -= 8 ) {
        updateFnv1a(state, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 为规范音频元组和重复序号生成稳定事件 ID。
/// @param canonicalBytes 事件的完整听觉语义字节。
/// @param duplicateOrdinal 相同音频元组中的零基重复序号。
/// @param collisionSalt 极小概率哈希碰撞时使用的确定性盐值。
/// @return 稳定 FNV-1a 事件标识。
std::uint64_t makeStableEventId(const CanonicalBytes& canonicalBytes,
                                std::uint64_t         duplicateOrdinal,
                                std::uint64_t         collisionSalt) noexcept
{
    std::uint64_t state = FNV1A_OFFSET_BASIS;
    updateFnv1a(state, "MMM.AudioTimelineEvent.v1");
    updateFnv1a(state, canonicalBytes);
    updateFnv1a(state, duplicateOrdinal);
    updateFnv1a(state, collisionSalt);
    return state;
}

/// @brief 将一个 64 位哈希编码为固定长度小写十六进制文本。
/// @param value 待编码哈希。
/// @param output 接收文本的 32 字节双哈希字符串。
/// @param offset 写入起始位置。
void writeHex(std::uint64_t value, std::string& output, std::size_t offset)
{
    constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
    for ( std::size_t index = 0U; index < 16U; ++index ) {
        const auto shift = static_cast<unsigned>((15U - index) * 4U);
        output[offset + index] =
            HEX_DIGITS[static_cast<std::size_t>((value >> shift) & 0xFU)];
    }
}

/// @brief 构造覆盖谱面结束时间和全部规范事件的双 FNV-1a 指纹。
/// @param events 已按规范音频元组排序的待处理事件。
/// @param chartEndSeconds 非自动采样内容决定的谱面结束时间。
/// @return 固定 32 位小写十六进制指纹。
std::string buildFingerprint(const std::vector<PendingTimelineEvent>& events,
                             double chartEndSeconds)
{
    CanonicalBytes payload;
    appendDouble(payload, chartEndSeconds);
    appendUnsigned(payload, static_cast<std::uint64_t>(events.size()));
    for ( const auto& event : events ) {
        appendUnsigned(
            payload, static_cast<std::uint64_t>(event.m_canonicalBytes.size()));
        payload.insert(payload.end(),
                       event.m_canonicalBytes.begin(),
                       event.m_canonicalBytes.end());
    }

    std::uint64_t firstHash  = FNV1A_OFFSET_BASIS;
    std::uint64_t secondHash = FNV1A_OFFSET_BASIS;
    updateFnv1a(firstHash, "MMM.AudioTimelineDescriptor.v1.first");
    updateFnv1a(secondHash, "MMM.AudioTimelineDescriptor.v1.second");
    updateFnv1a(firstHash, payload);
    updateFnv1a(secondHash, payload);

    std::string fingerprint(32U, '0');
    writeHex(firstHash, fingerprint, 0U);
    writeHex(secondHash, fingerprint, 16U);
    return fingerprint;
}

}  // namespace

bool audioTimelineDescriptorReferencesResource(
    const AudioTimelineDescriptor& descriptor, std::string_view resourceId)
{
    if ( resourceId.empty() ) return false;
    return std::any_of(
        descriptor.m_events.begin(),
        descriptor.m_events.end(),
        [resourceId](const MMM::Audio::AudioTimelineLoadEvent& event) {
            return event.resourceKey == resourceId;
        });
}

/// @brief 从 BeatMap 自动采样和项目资源构建音频加载描述符。
/// @param beatMap 待读取的谱面；仅遍历 m_audioSamples。
/// @param project 用于解析音频资源 ID、旧路径和完整音轨配置的项目。
/// @param beatmapPath 谱面所在的项目相对或绝对路径。
/// @param chartContentEndSeconds 玩家物件等非采样内容决定的结束时间。
/// @return 规范排序的加载事件、解析诊断、稳定指纹和谱面结束时间。
/// @warning 低频描述符重建路径：会解析文件系统路径并排序完整采样列表。
AudioTimelineDescriptor buildAudioTimelineDescriptor(
    const BeatMap& beatMap, const Project& project,
    const std::filesystem::path& beatmapPath, double chartContentEndSeconds)
{
    std::vector<PendingTimelineEvent> pendingEvents;
    pendingEvents.reserve(beatMap.m_audioSamples.size());

    /// @brief 与自动采样顺序一致的资源引用视图。
    std::vector<std::string_view> audioReferences;
    audioReferences.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        audioReferences.emplace_back(sample.m_audioResourceId);
    }
    /// @brief 一次建表后批量解析的项目资源结果。
    const auto resolvedResources =
        ProjectResourceService::resolveAudioResourceReferences(
            project, beatmapPath, audioReferences);

    /// @brief 每个资源只解析一次规范绝对文件路径。
    std::unordered_map<const AudioResource*, std::string>
        absolutePathsByResource;
    absolutePathsByResource.reserve(project.m_audioResources.size());

    std::size_t sampleIndex = 0U;
    for ( const auto& sample : beatMap.m_audioSamples ) {
        const auto* resource = resolvedResources[sampleIndex++];

        MMM::Audio::AudioTimelineLoadEvent event;
        event.resourceKey =
            resource ? resource->m_id : sample.m_audioResourceId;
        event.effectiveStartSeconds =
            (sample.m_timestamp + static_cast<double>(sample.m_offsetMs)) /
            1000.0;
        event.eventVolume = sample.m_volume;
        if ( resource ) {
            auto [pathIterator, inserted] =
                absolutePathsByResource.try_emplace(resource);
            if ( inserted ) {
                pathIterator->second =
                    Config::pathToUtf8(resolveAbsoluteResourcePath(
                        project, beatmapPath, resource->m_path));
            }
            event.filePath       = pathIterator->second;
            event.resourceConfig = resource->m_config;
        }

        pendingEvents.push_back(PendingTimelineEvent{
            .m_event             = event,
            .m_canonicalBytes    = makeCanonicalEventBytes(event),
            .m_originalReference = sample.m_audioResourceId,
            .m_unresolved        = resource == nullptr,
        });
    }

    std::sort(
        pendingEvents.begin(),
        pendingEvents.end(),
        [](const PendingTimelineEvent& lhs, const PendingTimelineEvent& rhs) {
            return lhs.m_canonicalBytes < rhs.m_canonicalBytes;
        });

    AudioTimelineDescriptor descriptor;
    descriptor.m_chartEndSeconds = chartContentEndSeconds;
    descriptor.m_fingerprint =
        buildFingerprint(pendingEvents, chartContentEndSeconds);
    descriptor.m_events.reserve(pendingEvents.size());
    descriptor.m_diagnostics.reserve(pendingEvents.size());

    std::set<std::uint64_t> usedEventIds;
    std::uint64_t           duplicateOrdinal = 0U;
    for ( std::size_t index = 0U; index < pendingEvents.size(); ++index ) {
        auto& pending = pendingEvents[index];
        if ( index > 0U && pending.m_canonicalBytes ==
                               pendingEvents[index - 1U].m_canonicalBytes ) {
            ++duplicateOrdinal;
        } else {
            duplicateOrdinal = 0U;
        }

        std::uint64_t collisionSalt = 0U;
        std::uint64_t eventId       = 0U;
        do {
            eventId = makeStableEventId(
                pending.m_canonicalBytes, duplicateOrdinal, collisionSalt);
            ++collisionSalt;
        } while ( eventId == 0U || usedEventIds.contains(eventId) );
        usedEventIds.insert(eventId);
        pending.m_event.eventId = eventId;

        if ( pending.m_unresolved ) {
            descriptor.m_diagnostics.push_back(
                AudioTimelineDescriptorDiagnostic{
                    .m_code = AudioTimelineDescriptorDiagnosticCode::
                        UnresolvedAudioResource,
                    .m_eventId        = eventId,
                    .m_audioReference = pending.m_originalReference,
                    .m_message        = "无法解析自动采样音频资源引用 '" +
                                        pending.m_originalReference +
                                        "'，该事件将以缺失资源载入",
                });
        }
        descriptor.m_events.push_back(std::move(pending.m_event));
    }
    return descriptor;
}

}  // namespace MMM::Logic
