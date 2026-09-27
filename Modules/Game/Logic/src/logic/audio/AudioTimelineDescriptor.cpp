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

/// @brief 每个独立哈希流的初始状态。
constexpr std::uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
/// @brief FNV-1a 的逐字节混合乘数。
constexpr std::uint64_t FNV1A_PRIME = 1099511628211ULL;
/// @brief double 位模式的符号位，用于有序映射。
constexpr std::uint64_t DOUBLE_SIGN_BIT = 0x8000000000000000ULL;
/// @brief float 位模式的符号位，用于有序映射。
constexpr std::uint32_t FLOAT_SIGN_BIT = 0x80000000U;

/// @brief 不依赖对象布局的规范编码，统一用于比较与指纹输入。
using CanonicalBytes = std::vector<std::uint8_t>;

/// @brief 尚未赋予稳定事件 ID 的规范音频事件。
/// @note 暂存原始引用仅为诊断使用，实际资源键采用解析后的稳定身份。
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
    // 高字节在前，使字节字典序与无符号数值序一致，并消除主机端序差异。
    for ( int shift = 56; shift >= 0; shift -= 8 ) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 获取跨平台稳定且可按数值顺序比较的 double 位模式。
/// @param value 待规范化数值。
/// @return 统一零值和 NaN 后的可排序无符号位模式。
/// @note 规范化用于比较编码，不修改最终加载事件中的数值本身。
std::uint64_t sortableDoubleBits(double value) noexcept
{
    std::uint64_t bits = 0U;
    if ( std::isnan(value) ) {
        // 不让不同 NaN 负载产生不同指纹；正负零也统一为零位模式。
        bits = 0x7FF8000000000000ULL;
    } else if ( value != 0.0 ) {
        bits = std::bit_cast<std::uint64_t>(value);
    }
    // 负数按补码序反转，非负数翻转符号位，拼成按数值递增的无符号排序键。
    return (bits & DOUBLE_SIGN_BIT) != 0U ? ~bits : bits ^ DOUBLE_SIGN_BIT;
}

/// @brief 获取跨平台稳定且可按数值顺序比较的 float 位模式。
/// @param value 待规范化数值。
/// @return 统一零值和 NaN 后的可排序无符号位模式。
std::uint32_t sortableFloatBits(float value) noexcept
{
    // 使用与 double 相同的零和 NaN 规则，避免不同精度字段的特殊值处理不一致。
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
    // float 排序键仍占统一的八字节整数槽，不能改成原生内存拷贝而改变编码布局。
    appendUnsigned(bytes, sortableFloatBits(value));
}

/// @brief 将有符号整数追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加数值。
void appendSigned(CanonicalBytes& bytes, std::int64_t value)
{
    // 翻转符号位将有符号顺序映射到无符号域，再复用大端编码。
    appendUnsigned(bytes, static_cast<std::uint64_t>(value) ^ DOUBLE_SIGN_BIT);
}

/// @brief 将长度前缀 UTF-8 字符串追加到规范字节流。
/// @param bytes 接收字节的容器。
/// @param value 待追加字符串。
void appendString(CanonicalBytes& bytes, std::string_view value)
{
    // 长度前缀隔开相邻变长字段，避免不同字符串分割方式形成相同字节串。
    appendUnsigned(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

/// @brief 将完整音轨配置追加到规范字节流。
/// @note 新增影响播放的配置字段时，应同步扩展这里的编码，避免指纹漏报变更。
/// @param bytes 接收字节的容器。
/// @param config 待追加的持久化音轨配置。
void appendTrackConfig(CanonicalBytes& bytes, const AudioTrackConfig& config)
{
    // 指纹覆盖完整持久化配置；即使均衡器暂时关闭，修改其配置也改变描述符。
    appendFloat(bytes, config.volume);
    appendFloat(bytes, config.playbackSpeed);
    appendFloat(bytes, config.playbackPitch);
    appendUnsigned(bytes, config.muted ? 1U : 0U);
    appendUnsigned(bytes, config.eqEnabled ? 1U : 0U);
    appendSigned(bytes, static_cast<std::int64_t>(config.eqPreset));

    // 数组长度属于编码内容，防止不同频段数与后续字段拼接产生歧义。
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
/// @details BGM 相对轨参与调度语义，正式玩家键数及画布位置不直接编码。
/// 资源路径也参与指纹，移动资源后即使 ID 不变仍能触发重新加载。
CanonicalBytes makeCanonicalEventBytes(
    const MMM::Audio::AudioTimelineLoadEvent& event)
{
    // 不编码对象句柄、协作 ID 或容器序号；相同听觉输入应得到相同规范元组。
    CanonicalBytes bytes;
    // 预留是减少扩容的估计，不作为编码长度；实际字段仍由追加函数完整写入。
    bytes.reserve(128U + event.resourceKey.size() + event.filePath.size() +
                  (event.resourceConfig.eqBandGains.size() +
                   event.resourceConfig.eqBandQs.size()) *
                      sizeof(std::uint64_t));
    // 有效时间排在首位，规范排序先按起播时刻，再由其他字段稳定打破平局。
    appendDouble(bytes, event.effectiveStartSeconds);
    appendUnsigned(bytes, event.bgmTrackIndex);
    appendString(bytes, event.resourceKey);
    appendString(bytes, event.filePath);
    appendFloat(bytes, event.eventVolume);
    appendTrackConfig(bytes, event.resourceConfig);
    return bytes;
}

/// @brief 构造 Main 资源画布同步所需的规范事件字节。
/// @param event 已解析到 Main 项目资源的加载事件。
/// @return 仅包含资源身份和有效起播位置的规范字节。
/// @note 不能拿这个精简编码替代完整调度指纹，音量等加载参数变化仍需重建调度。
CanonicalBytes makeMainAudioSyncEventBytes(
    const MMM::Audio::AudioTimelineLoadEvent& event)
{
    // 画布同步只关心 Main 身份和起播位置，不因音量、均衡器或 BGM
    // 列差异拆分同步组。
    CanonicalBytes bytes;
    bytes.reserve(24U + event.resourceKey.size() + event.filePath.size());
    appendDouble(bytes, event.effectiveStartSeconds);
    appendString(bytes, event.resourceKey);
    appendString(bytes, event.filePath);
    return bytes;
}

/// @brief 将项目资源路径解析为规范化绝对路径。
/// @param project 音频资源所属项目。
/// @param beatmapPath 当前谱面路径，用作项目根缺失时的回退基准。
/// @param storedPath 项目资源保存的 UTF-8 路径。
/// @return 规范化绝对路径；空资源路径返回空。
/// @warning 低频描述符路径：可能访问文件系统以消解现有路径前缀。
/// @note 存在性和解码错误交给加载阶段，缺失资源仍可得到稳定的候选路径。
std::filesystem::path resolveAbsoluteResourcePath(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::string& storedPath)
{
    if ( storedPath.empty() ) return {};

    auto resourcePath = Config::utf8ToPath(storedPath);
    if ( resourcePath.is_relative() ) {
        // 优先以项目根为基准；无项目根时才借用谱面目录，不能反过来解释项目相对资源。
        auto basePath = project.m_projectRoot;
        if ( basePath.empty() && !beatmapPath.empty() ) {
            // 先把谱面转为绝对位置再取父目录，避免相对目录随后被再次拼接。
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
        // 根和谱面基准都不足时，最后尝试按当前工作目录绝对化。
        std::error_code filesystemError;
        const auto      converted =
            std::filesystem::absolute(resourcePath, filesystemError);
        if ( !filesystemError ) resourcePath = converted;
    }

    std::error_code filesystemError;
    const auto      canonicalPath =
        std::filesystem::weakly_canonical(resourcePath, filesystemError);
    if ( !filesystemError ) resourcePath = canonicalPath;
    // 规范化失败仍保留可诊断的词法路径；描述符构建不负责证明音频能被解码。
    return resourcePath.lexically_normal();
}

/// @brief 向 FNV-1a 状态追加单字节。
/// @param state 当前哈希状态。
/// @param byte 待追加字节。
void updateFnv1a(std::uint64_t& state, std::uint8_t byte) noexcept
{
    // 无符号乘法回绕是哈希定义的一部分，不作为数值溢出错误处理。
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
    // 零字节终止域标签，避免标签尾部与后续数据合并成另一种输入。
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
/// @details 重复序号区分相同事件的多次出现，碰撞盐只在 ID 冲突时改变。
/// @note 这是内容标识而非安全摘要；是否为零及与其他事件碰撞由调用方处理。
std::uint64_t makeStableEventId(const CanonicalBytes& canonicalBytes,
                                std::uint64_t         duplicateOrdinal,
                                std::uint64_t         collisionSalt) noexcept
{
    std::uint64_t state = FNV1A_OFFSET_BASIS;
    // 版本域区分用途；规范编码变更时需一并考虑旧标识兼容性。
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
/// @pre output 已容纳 offset 起的十六个字符，函数不调整字符串长度。
void writeHex(std::uint64_t value, std::string& output, std::size_t offset)
{
    // 固定写满十六位，保留前导零，确保双哈希拼接边界不依赖数值大小。
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
/// @details
/// 长度与事件数量都参与编码，不把事件边界依赖于可变字符串中的特殊字符。
/// @note 输入顺序必须已经规范化，指纹计算不再排序事件。
std::string buildFingerprint(const std::vector<PendingTimelineEvent>& events,
                             double chartEndSeconds)
{
    CanonicalBytes payload;
    // 结束时间独立编码，即使音频采样不变，正式谱面长度变化仍能触发描述符更新。
    appendDouble(payload, chartEndSeconds);
    appendUnsigned(payload, static_cast<std::uint64_t>(events.size()));
    for ( const auto& event : events ) {
        // 每个规范元组独立带长度，后续字段变化不能跨事件边界重新解释。
        appendUnsigned(
            payload, static_cast<std::uint64_t>(event.m_canonicalBytes.size()));
        payload.insert(payload.end(),
                       event.m_canonicalBytes.begin(),
                       event.m_canonicalBytes.end());
    }

    // 相同载荷使用不同域生成两个哈希值，不将其宣称为密码学完整性校验。
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

/// @brief 构造只覆盖 Main 资源序列和有效起播位置的同步指纹。
/// @param events Main 资源对应的规范同步事件。
/// @return 无 Main 资源时返回空，否则返回稳定双 FNV-1a 指纹。
/// @note 按值接收同步元组以便就地排序，不修改完整加载事件的顺序。
std::string buildMainAudioSyncFingerprint(std::vector<CanonicalBytes> events)
{
    // 空指纹明确表示没有主音轨同步依据，不与一个非空但无事件的固定摘要混用。
    if ( events.empty() ) return {};

    // 规范化事件顺序但不去重，相同起播位置出现多次仍是不同的调度内容。
    std::sort(events.begin(), events.end());
    CanonicalBytes payload;
    appendUnsigned(payload, static_cast<std::uint64_t>(events.size()));
    for ( const auto& event : events ) {
        appendUnsigned(payload, static_cast<std::uint64_t>(event.size()));
        payload.insert(payload.end(), event.begin(), event.end());
    }

    std::uint64_t firstHash  = FNV1A_OFFSET_BASIS;
    std::uint64_t secondHash = FNV1A_OFFSET_BASIS;
    updateFnv1a(firstHash, "MMM.MainAudioSyncTimeline.v1.first");
    updateFnv1a(secondHash, "MMM.MainAudioSyncTimeline.v1.second");
    updateFnv1a(firstHash, payload);
    updateFnv1a(secondHash, payload);

    std::string fingerprint(32U, '0');
    writeHex(firstHash, fingerprint, 0U);
    writeHex(secondHash, fingerprint, 16U);
    return fingerprint;
}

}  // namespace

/// @brief 判断描述符中是否有指定资源键的加载事件。
/// @param descriptor 已构建的加载描述符。
/// @param resourceId 要检查的资源 ID。
/// @return 非空 ID 精确命中任一事件资源键时返回真。
/// @note 不解析路径别名，也不访问项目资源表。
/// @details 缺失资源事件也参与检查，便于资源重新出现后定位受影响描述符。
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
/// @details 空资源引用被忽略，非空但无法解析的引用保留为缺失资源事件并附诊断。
/// @pre project 与 beatMap 在构建期间保持稳定，批量解析结果观察其资源列表。
/// @note 不把自动采样末尾当作谱面结束时刻，音频实际时长由后续加载层掌握。
AudioTimelineDescriptor buildAudioTimelineDescriptor(
    const BeatMap& beatMap, const Project& project,
    const std::filesystem::path& beatmapPath, double chartContentEndSeconds)
{
    std::vector<PendingTimelineEvent> pendingEvents;
    pendingEvents.reserve(beatMap.m_audioSamples.size());
    /// @brief 只收集 Main 资源身份和起播位置的同步事件。
    std::vector<CanonicalBytes> mainAudioSyncEvents;
    mainAudioSyncEvents.reserve(beatMap.m_audioSamples.size());

    /// @brief 与自动采样顺序一致的资源引用视图。
    std::vector<std::string_view> audioReferences;
    audioReferences.reserve(beatMap.m_audioSamples.size());
    // string_view 只在本次构建中借用采样字符串，不存入返回描述符。
    for ( const auto& sample : beatMap.m_audioSamples ) {
        // 空引用是静音草稿，不参与加载描述；后面的构建循环必须采用相同过滤条件。
        if ( sample.m_audioResourceId.empty() ) continue;
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
    // 指针键只在本次资源列表稳定期间有效，不作为跨项目的持久化身份。

    /// @brief 非空资源引用在批量解析结果中的位置，不是原始采样容器下标。
    std::size_t sampleIndex = 0U;
    for ( const auto& sample : beatMap.m_audioSamples ) {
        if ( sample.m_audioResourceId.empty() ) continue;
        const auto* resource = resolvedResources[sampleIndex++];

        MMM::Audio::AudioTimelineLoadEvent event;
        event.resourceKey =
            resource ? resource->m_id : sample.m_audioResourceId;
        // 领域时间和偏移均为毫秒，合并后一次性换算到音频时间线的秒单位。
        event.effectiveStartSeconds =
            (sample.m_timestamp + static_cast<double>(sample.m_offsetMs)) /
            1000.0;
        const auto playerTrackCount = static_cast<std::uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        // 音频调度只关心 BGM 相对轨，不因玩家键数不同而改变等价歌曲的轨道身份。
        // 非法落入玩家区的采样回退到首条 BGM 轨，避免无符号减法回绕。
        event.bgmTrackIndex = sample.m_track >= playerTrackCount
                                  ? sample.m_track - playerTrackCount
                                  : 0U;
        // 单次采样音量和资源级配置分开保存，让加载层组合两层播放参数。
        event.eventVolume = sample.m_volume;
        if ( resource ) {
            auto [pathIterator, inserted] =
                absolutePathsByResource.try_emplace(resource);
            if ( inserted ) {
                // 同资源多次采样复用绝对路径，避免每个事件重复触发文件系统规范化。
                pathIterator->second =
                    Config::pathToUtf8(resolveAbsoluteResourcePath(
                        project, beatmapPath, resource->m_path));
            }
            event.filePath       = pathIterator->second;
            event.resourceConfig = resource->m_config;
        }
        if ( resource && resource->m_type == AudioTrackType::Main ) {
            // 无法解析的引用不推测 Main
            // 身份，避免把缺失资源错误加入画布同步依据。
            mainAudioSyncEvents.push_back(makeMainAudioSyncEventBytes(event));
        }

        // 未解析事件仍保留原资源键和规范元组，诊断在稳定 ID 生成后再关联。
        pendingEvents.push_back(PendingTimelineEvent{
            .m_event             = event,
            .m_canonicalBytes    = makeCanonicalEventBytes(event),
            .m_originalReference = sample.m_audioResourceId,
            .m_unresolved        = resource == nullptr,
        });
    }

    // 不依赖谱面采样容器顺序，相同语义的重排不应重新生成整套事件身份。
    std::sort(
        pendingEvents.begin(),
        pendingEvents.end(),
        [](const PendingTimelineEvent& lhs, const PendingTimelineEvent& rhs) {
            return lhs.m_canonicalBytes < rhs.m_canonicalBytes;
        });

    AudioTimelineDescriptor descriptor;
    descriptor.m_chartEndSeconds = chartContentEndSeconds;
    // 完整指纹用于加载内容变化判断，同步指纹只用于主音轨时间关系分组。
    descriptor.m_fingerprint =
        buildFingerprint(pendingEvents, chartContentEndSeconds);
    descriptor.m_mainAudioSyncFingerprint =
        buildMainAudioSyncFingerprint(std::move(mainAudioSyncEvents));
    descriptor.m_events.reserve(pendingEvents.size());
    descriptor.m_diagnostics.reserve(pendingEvents.size());

    /// @brief 已分配的非零事件 ID，用于确定性处理哈希碰撞。
    std::set<std::uint64_t> usedEventIds;
    std::uint64_t           duplicateOrdinal = 0U;
    // ID 在排序后分配；若在收集阶段生成，输入次序会干扰重复项与碰撞处理。
    for ( std::size_t index = 0U; index < pendingEvents.size(); ++index ) {
        auto& pending = pendingEvents[index];
        if ( index > 0U && pending.m_canonicalBytes ==
                               pendingEvents[index - 1U].m_canonicalBytes ) {
            // 完全相同的采样也不能合并，重复序号为每次播放提供不同 ID。
            ++duplicateOrdinal;
        } else {
            // 进入新规范元组后重复序号重新从零开始，其他事件数量不影响其首个
            // ID。
            duplicateOrdinal = 0U;
        }

        // 规范排序固定了碰撞处理顺序，逐次加盐不会受输入容器重排影响。
        std::uint64_t collisionSalt = 0U;
        std::uint64_t eventId       = 0U;
        do {
            eventId = makeStableEventId(
                pending.m_canonicalBytes, duplicateOrdinal, collisionSalt);
            ++collisionSalt;
        } while ( eventId == 0U || usedEventIds.contains(eventId) );
        // 零保留为无事件标识，所有交付的 ID 在本描述符内唯一。
        usedEventIds.insert(eventId);
        pending.m_event.eventId = eventId;

        // 诊断使用最终事件 ID，调用方可将缺失资源提示准确关联到加载事件。
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
        // 即使存在诊断也交付事件，缺失资源处理由统一加载流程决定而非在此静默丢弃。
        descriptor.m_events.push_back(std::move(pending.m_event));
    }
    return descriptor;
}

}  // namespace MMM::Logic
