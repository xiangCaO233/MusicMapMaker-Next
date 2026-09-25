#include "network/collaboration/BeatmapDocumentCodec.h"

#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "mmm/Metadata.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Note.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
using Json = nlohmann::json;

/// @brief 协作谱面文档的线格式与状态模型说明。
///
/// @details
/// 线上负载由固定 12 字节外层头和 CBOR 正文组成。外层头布局为：
///
/// - 字节 0..3：ASCII 魔数 MMBD；
/// - 字节 4：外层封装版本；
/// - 字节 5：压缩标志位；
/// - 字节 6..7：必须为零的保留位；
/// - 字节 8..11：小端未压缩 CBOR 长度；
/// - 后续字节：原始 CBOR 或 DEFLATE 正文。
///
/// CBOR 根对象包含内层 version 与 snapshot。完整快照包含 objects、timelines、
/// audio_samples、metadata 和 annotations 五类；增量以对应 *_delta 携带数组
/// added/removed，metadata 因字段耦合较强而整体替换。
///
/// @par Objects 表示
/// 根 NOTE、HOLD、FLICK 各编码一次。POLYLINE 作为根对象编码，其 sub_notes 内嵌
/// 有序子对象；BeatMap 类型容器中的同一子对象不会再次出现在根数组。每个根对象
/// 的 collaboration_id 是并发合并身份，字段更新表现为移除旧编码并添加新编码。
///
/// @par 状态分工
/// Codec 同时维护接收 document 和发送 encodingBaseline。apply 推进接收文档；
/// encode 只在负载成功生成后推进发送基线。两者分离允许同一房间既接收远端状态
/// 又提交本地变更，同时保证失败重试不会跳过尚未成功编码的差异。
///
/// @par 兼容回退
/// 玩家物件和批注优先按稳定 ID 比较与应用。遇到缺失或重复 ID 时，不猜测对象
/// 对应关系，而是回退到完整 JSON 值的多重集增删。旧数据仍可同步，只是失去
/// 精确替换与未变对象编码缓存优化。
///
/// @par 安全边界
/// 外层在分配前限制声明解压尺寸；内层读取严格检查 JSON 类型、枚举范围、ID
/// 唯一性及批注长度。所有解析使用无异常模式，失败通过 BeatmapDocumentError
/// 或空返回值表达，永不把部分 BeatMap 暴露给调用者。
///
/// @par 性能边界
/// 未变化玩家物件通过领域指纹避免重复 JSON 编码；其他分类只在对应 mutation
/// flag 出现时编码。压缩采用速度优先级，且小文档直接发送原始 CBOR，避免协作
/// 编辑的高频局部提交承担不必要固定开销。
/// 编码缓存只保存成功基线，空间开销随当前根对象数量线性增长且可随 reset 释放。
///
/// @brief CBOR 内层协作文档结构版本。
/// @note 字段集合或增量语义不兼容变化时必须递增。
constexpr std::uint32_t DOCUMENT_FORMAT_VERSION = 5;
/// @brief 协作谱面负载固定魔数，对应 ASCII `MMBD`。
constexpr std::array<std::uint8_t, 4> DOCUMENT_PAYLOAD_MAGIC{
    'M', 'M', 'B', 'D'
};
/// @brief 谱面负载外层封装版本。
/// @note 只在魔数之后占一字节，与 CBOR 内层文档版本独立演进。
constexpr std::uint8_t DOCUMENT_PAYLOAD_VERSION = 1;
/// @brief 外层负载头长度。
/// @note 包含魔数、版本、标志、保留位和四字节未压缩长度。
constexpr std::size_t DOCUMENT_PAYLOAD_HEADER_BYTES = 12;
/// @brief 防止畸形压缩包声明过大的解压内存。
/// @note 编码与解码同时执行该上限，合法本地文档不会生成远端拒绝的尺寸。
constexpr std::size_t MAX_UNCOMPRESSED_DOCUMENT_BYTES = 64U * 1024U * 1024U;
/// @brief 小负载不压缩，避免固定压缩开销反而增大消息。
/// @note 达到阈值也只尝试压缩，结果不更小时仍发送原始 CBOR。
constexpr std::size_t DOCUMENT_COMPRESSION_THRESHOLD_BYTES = 1024U;
/// @brief 外层负载压缩标志。
/// @note 当前仅定义最低位，解码器拒绝任何未知标志位。
constexpr std::uint8_t DOCUMENT_PAYLOAD_COMPRESSED = 1U;

/// @brief 向负载头写入小端 32 位整数。
/// @param output 接收四个字节的连续缓冲。
/// @param value 要编码的无符号值。
///
/// @details 显式逐字节写入使负载格式不依赖主机字节序或结构体对齐。
void appendUint32(ByteBuffer& output, std::uint32_t value)
{
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 从负载头读取小端 32 位整数。
/// @param input 已通过头长度检查的负载。
/// @param offset 四字节字段起始偏移。
/// @return 解码后的主机序 uint32 值。
/// @warning 调用方必须保证 offset 后至少还有四个字节。
std::uint32_t readUint32(std::span<const std::uint8_t> input,
                         std::size_t                   offset)
{
    std::uint32_t value = 0;
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        value |= static_cast<std::uint32_t>(input[offset++]) << shift;
    }
    return value;
}

/// @brief 将 CBOR 文档封装为可选 DEFLATE 压缩的有界二进制负载。
/// @param document 已完成字段构造的 JSON 文档。
/// @return 带固定头的二进制负载，或明确的文档错误。
///
/// @details
/// 文档先编码为 CBOR，再在超过阈值时尝试 MZ_BEST_SPEED 压缩。只有压缩结果
/// 真正更小时才设置 compressed 标志，否则保留原始 CBOR。外层头包含魔数、
/// 版本、标志、两个保留字节和未压缩长度，接收端可在分配前执行边界检查。
///
/// @note 压缩失败不是文档失败，编码器会安全回退到未压缩正文。
std::expected<ByteBuffer, BeatmapDocumentError> encodeDocumentPayload(
    const Json& document)
{
    // 未压缩尺寸既限制内存，也必须能放入线格式的 32 位长度字段。
    const ByteBuffer raw = Json::to_cbor(document);
    if ( raw.empty() || raw.size() > MAX_UNCOMPRESSED_DOCUMENT_BYTES ||
         raw.size() > std::numeric_limits<std::uint32_t>::max() ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }

    ByteBuffer body       = raw;
    bool       compressed = false;
    // 小负载跳过压缩，避免 zlib 头和 CPU 开销大于节省空间。
    if ( raw.size() >= DOCUMENT_COMPRESSION_THRESHOLD_BYTES ) {
        mz_ulong compressedSize =
            mz_compressBound(static_cast<mz_ulong>(raw.size()));
        ByteBuffer candidate(compressedSize);
        // 只有成功且严格变小才采用候选，保证压缩永不扩大线上负载。
        if ( mz_compress2(candidate.data(),
                          &compressedSize,
                          raw.data(),
                          static_cast<mz_ulong>(raw.size()),
                          MZ_BEST_SPEED) == MZ_OK &&
             compressedSize < raw.size() ) {
            candidate.resize(static_cast<std::size_t>(compressedSize));
            body       = std::move(candidate);
            compressed = true;
        }
    }

    // 预留精确上界，避免写入固定头和正文时多次扩容。
    ByteBuffer output;
    output.reserve(DOCUMENT_PAYLOAD_HEADER_BYTES + body.size());
    output.insert(output.end(),
                  DOCUMENT_PAYLOAD_MAGIC.begin(),
                  DOCUMENT_PAYLOAD_MAGIC.end());
    output.push_back(DOCUMENT_PAYLOAD_VERSION);
    output.push_back(compressed ? DOCUMENT_PAYLOAD_COMPRESSED : 0U);
    output.push_back(0U);
    output.push_back(0U);
    appendUint32(output, static_cast<std::uint32_t>(raw.size()));
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

/// @brief 校验外层头并解压 CBOR 文档。
/// @param payload 来自协作传输的完整文档负载。
/// @return 解析成功的 JSON，或 InvalidPayload。
///
/// @details
/// 解码顺序刻意从廉价边界到昂贵操作：先检查长度、魔数、版本、保留位和未知
/// 标志，再读取受限未压缩长度。压缩正文必须恰好解出声明字节数，未压缩正文
/// 则必须与声明长度完全相等。最后以无异常模式解析 CBOR。
///
/// @warning 声明尺寸在任何分配前限制为 64 MiB，防止畸形负载触发内存膨胀。
std::expected<Json, BeatmapDocumentError> decodeDocumentPayload(
    std::span<const std::uint8_t> payload)
{
    if ( payload.size() <= DOCUMENT_PAYLOAD_HEADER_BYTES ||
         !std::equal(DOCUMENT_PAYLOAD_MAGIC.begin(),
                     DOCUMENT_PAYLOAD_MAGIC.end(),
                     payload.begin()) ||
         payload[4] != DOCUMENT_PAYLOAD_VERSION || payload[6] != 0U ||
         payload[7] != 0U ||
         (payload[5] & ~DOCUMENT_PAYLOAD_COMPRESSED) != 0U ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    // 固定头已验证长度，偏移 8 的四字节字段可以安全读取。
    const std::size_t rawSize = readUint32(payload, 8);
    if ( rawSize == 0 || rawSize > MAX_UNCOMPRESSED_DOCUMENT_BYTES ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }

    const auto body = payload.subspan(DOCUMENT_PAYLOAD_HEADER_BYTES);
    ByteBuffer raw;
    if ( (payload[5] & DOCUMENT_PAYLOAD_COMPRESSED) != 0U ) {
        // 仅按已校验的 rawSize 分配，并要求解压器返回完全一致的长度。
        raw.resize(rawSize);
        mz_ulong decodedSize = static_cast<mz_ulong>(raw.size());
        if ( mz_uncompress(raw.data(),
                           &decodedSize,
                           body.data(),
                           static_cast<mz_ulong>(body.size())) != MZ_OK ||
             decodedSize != rawSize ) {
            return std::unexpected(BeatmapDocumentError::InvalidPayload);
        }
    } else {
        if ( body.size() != rawSize ) {
            return std::unexpected(BeatmapDocumentError::InvalidPayload);
        }
        raw.assign(body.begin(), body.end());
    }

    // strict=true 拒绝尾随 CBOR 数据，allow_exceptions=false 符合禁用异常约束。
    Json document = Json::from_cbor(raw, true, false);
    if ( document.is_discarded() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    return document;
}

template<typename Enum>
/// @brief 把按来源枚举分组的字符串属性表编码为稳定结构。
/// @tparam Enum 元数据来源枚举类型。
/// @param properties 来源到键值表的映射。
/// @return 每个来源一项的 JSON 数组。
///
/// @details JSON 对象键不能直接表达枚举，因此外层使用 source 数值与 values
/// 对象配对。容器顺序不作为语义，接收端按 source 重建无序表。
Json encodePropertyMap(
    const std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    Json result = Json::array();
    for ( const auto& [source, values] : properties ) {
        Json encodedValues = Json::object();
        for ( const auto& [key, value] : values ) {
            encodedValues[key] = value;
        }
        result.push_back(Json{
            { "source", static_cast<std::uint32_t>(source) },
            { "values", std::move(encodedValues) },
        });
    }
    return result;
}

template<typename Enum>
/// @brief 严格解码来源分组属性表。
/// @tparam Enum 目标来源枚举类型。
/// @param source encodePropertyMap 生成的数组结构。
/// @param properties 接收解码结果的目标映射。
/// @return 全部条目结构和字符串值合法时返回 true。
///
/// @details 目标在解析前清空；失败可能留下部分结果，但调用方只在整体成功后
/// 使用临时 BeatMap，因此不会把半解码元数据提交为可见文档。
///
/// @par 结构约束
/// 外层每项必须是对象并同时具有 unsigned source 与 object values。values 中的
/// 每个值必须为字符串；键由 JSON 对象机制保证为文本。同一 source 重复出现时
/// 合并到同一目标映射，重复键保持首次 emplace 的值以兼容既有文档行为。
bool decodePropertyMap(
    const Json& source,
    std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    if ( !source.is_array() ) return false;
    properties.clear();
    for ( const auto& entry : source ) {
        if ( !entry.is_object() ) return false;
        const auto sourceIt = entry.find("source");
        const auto valuesIt = entry.find("values");
        if ( sourceIt == entry.end() || !sourceIt->is_number_unsigned() ||
             valuesIt == entry.end() || !valuesIt->is_object() ) {
            return false;
        }
        auto& output =
            properties[static_cast<Enum>(sourceIt->get<std::uint32_t>())];
        for ( auto valueIt = valuesIt->begin(); valueIt != valuesIt->end();
              ++valueIt ) {
            if ( !valueIt.value().is_string() ) return false;
            output.emplace(valueIt.key(), valueIt.value().get<std::string>());
        }
    }
    return true;
}

/// @brief 编码 Note 扩展元数据。
/// @param metadata Note 元数据容器。
/// @return 来源分组属性数组。
Json encodeNoteMetadata(const ::MMM::NoteMetadata& metadata)
{
    return encodePropertyMap(metadata.note_properties);
}

/// @brief 编码 Timing 扩展元数据。
/// @param metadata Timing 元数据容器。
/// @return 来源分组属性数组。
Json encodeTimingMetadata(const ::MMM::TimingMetadata& metadata)
{
    return encodePropertyMap(metadata.timing_properties);
}

/// @brief 编码音频采样扩展元数据。
/// @param metadata 采样元数据容器。
/// @return 来源分组属性数组。
Json encodeSampleMetadata(const ::MMM::SampleMetadata& metadata)
{
    return encodePropertyMap(metadata.sample_properties);
}

/// @brief 编码 Note 的可选音频采样绑定。
/// @param binding 可选资源 ID 与音量。
/// @return 未绑定时为 null，否则为字段对象。
Json encodeSampleBinding(
    const std::optional<::MMM::AudioSampleBinding>& binding)
{
    if ( !binding ) return nullptr;
    return Json{
        { "resource", binding->m_audioResourceId },
        { "volume", binding->m_volume },
    };
}

/// @brief 解码 Note 的可选音频采样绑定。
/// @param source null 或绑定对象。
/// @param binding 接收结果的 optional。
/// @return 结构和字段类型合法时返回 true。
/// @note null 会显式 reset，避免复用对象保留旧绑定。
bool decodeSampleBinding(const Json&                               source,
                         std::optional<::MMM::AudioSampleBinding>& binding)
{
    if ( source.is_null() ) {
        binding.reset();
        return true;
    }
    if ( !source.is_object() ) return false;
    const auto resourceIt = source.find("resource");
    const auto volumeIt   = source.find("volume");
    if ( resourceIt == source.end() || !resourceIt->is_string() ||
         volumeIt == source.end() || !volumeIt->is_number() ) {
        return false;
    }
    binding = ::MMM::AudioSampleBinding{ resourceIt->get<std::string>(),
                                         volumeIt->get<float>() };
    return true;
}

/// @brief 编码一个不含 Polyline 子列表的 Note 多态对象。
/// @param note NOTE、HOLD、FLICK 或 POLYLINE 基类引用。
/// @return 公共字段及具体类型扩展字段组成的 JSON 对象。
///
/// @details type 决定安全的静态向下转换：HOLD 增加 duration，FLICK 增加
/// dtrack； POLYLINE 子物件由根编码函数单独处理，避免递归职责混入公共字段编码。
Json encodeNote(const ::MMM::Note& note)
{
    Json result{
        { "type", static_cast<std::uint32_t>(note.m_type) },
        { "timestamp", note.m_timestamp },
        { "track", note.m_track },
        { "collaboration_id", note.m_collaborationId },
        { "binding", encodeSampleBinding(note.m_sampleBinding) },
        { "metadata", encodeNoteMetadata(note.m_metadata) },
    };
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        result["duration"] = static_cast<const ::MMM::Hold&>(note).m_duration;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        result["dtrack"] = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }
    return result;
}

/// @brief 编码玩家物件，并按 Polyline 实际引用排除重复的根物件副本。
/// @param beatmap 待编码谱面。
/// @return 可写入协作文档的物件数组。
///
/// @details
/// BeatMap 为访问效率同时保存按类型容器和 Polyline 引用。第一遍收集所有实际
/// 子物件地址，随后只编码非子物件且未被 Polyline 引用的根对象。Polyline 自身
/// 编码一次，并按 m_subNotes 的业务顺序内嵌子对象。
///
/// @note 地址集合只在函数调用期间使用，不跨越容器修改或对象生命周期。
Json encodeObjects(const ::MMM::BeatMap& beatmap)
{
    // 不能只依赖 m_isSubNote，因为旧谱面可能以引用关系表达子物件归属。
    std::unordered_set<const ::MMM::Note*> polylineSubNotes;
    // Polyline 子列表在根对象内嵌，保持连接顺序和动态类型。
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        for ( const auto& subNote : polyline.m_subNotes ) {
            polylineSubNotes.insert(&subNote.get());
        }
    }

    Json result = Json::array();
    for ( const auto& note : beatmap.m_noteData.notes ) {
        if ( !note.m_isSubNote && !polylineSubNotes.contains(&note) ) {
            result.push_back(encodeNote(note));
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        if ( !hold.m_isSubNote && !polylineSubNotes.contains(&hold) ) {
            result.push_back(encodeNote(hold));
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        if ( !flick.m_isSubNote && !polylineSubNotes.contains(&flick) ) {
            result.push_back(encodeNote(flick));
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        Json encoded         = encodeNote(polyline);
        encoded["sub_notes"] = Json::array();
        for ( const auto& subNote : polyline.m_subNotes ) {
            encoded["sub_notes"].push_back(encodeNote(subNote.get()));
        }
        result.push_back(std::move(encoded));
    }
    return result;
}

/// @brief 编码一个根物件，并在折线中内嵌其子物件。
/// @param note 根物件引用。
/// @return 单个可放入 objects 数组的 JSON 对象。
///
/// @details 该函数服务快速稳定 ID 缓存，必须与 encodeObjects 对单个根对象的
///          表示完全一致。非 Polyline 直接返回公共编码；Polyline 追加有序子表。
Json encodeRootObject(const ::MMM::Note& note)
{
    Json encoded = encodeNote(note);
    if ( note.m_type != ::MMM::NoteType::POLYLINE ) return encoded;
    encoded["sub_notes"] = Json::array();
    const auto& polyline = static_cast<const ::MMM::Polyline&>(note);
    for ( const auto& subNote : polyline.m_subNotes ) {
        encoded["sub_notes"].push_back(encodeNote(subNote.get()));
    }
    return encoded;
}

/// @brief 玩家物件领域字段的双 64 位快速指纹。
///
/// @details 指纹用于判断稳定 ID 对象是否需要重新 JSON 编码，不用于安全认证或
///          跨进程协议。双通道混合降低普通碰撞概率，最终增量仍携带完整编码值。
struct ObjectFingerprint {
    /// @brief 第一条混合状态。
    std::uint64_t first{ 0x243F6A8885A308D3ULL };
    /// @brief 第二条独立混合状态。
    std::uint64_t second{ 0x13198A2E03707344ULL };

    /// @brief 比较两条指纹状态是否完全相等。
    bool operator==(const ObjectFingerprint&) const = default;
};

/// @brief 把一个整数混入物件指纹。
/// @param fingerprint 就地更新的双状态。
/// @param value 要混入的领域数值位模式。
/// @note 常量和旋转分别扰动两条状态，避免字段序列简单异或抵消。
void mixFingerprint(ObjectFingerprint& fingerprint, std::uint64_t value)
{
    fingerprint.first ^= value + 0x9E3779B97F4A7C15ULL +
                         (fingerprint.first << 6U) + (fingerprint.first >> 2U);
    fingerprint.second =
        std::rotl(fingerprint.second ^ value, 27) * 0x94D049BB133111EBULL;
}

/// @brief 把字符串字节混入物件指纹。
/// @param fingerprint 就地更新的双状态。
/// @param value 按原始字节混入的 UTF-8 或稳定 ID 文本。
/// @note 先混入长度，区分不同字段边界拼接得到的相同字节序列。
void mixFingerprint(ObjectFingerprint& fingerprint, std::string_view value)
{
    mixFingerprint(fingerprint, value.size());
    for ( const unsigned char byte : value ) {
        fingerprint.first = (fingerprint.first ^ byte) * 0x100000001B3ULL;
        fingerprint.second =
            (fingerprint.second + byte) * 0x9E3779B185EBCA87ULL;
    }
}

/// @brief 把无序属性表以顺序无关方式混入物件指纹。
/// @tparam Enum 属性来源枚举。
/// @param fingerprint 接收聚合结果的根指纹。
/// @param properties 无序来源与键值映射。
///
/// @details 每个条目独立计算指纹，再通过交换律聚合，避免 unordered_map 遍历
/// 顺序变化产生伪增量。条目数也参与根指纹，降低聚合抵消的歧义。
///
/// @par 顺序独立性
/// source、key、value 在单条目内部仍按固定顺序混合，只有不同条目之间采用 XOR
/// 与加法聚合。因此哈希表重排保持相同结果，而字段角色互换仍会改变指纹。
template<typename Enum>
void mixPropertyMapFingerprint(
    ObjectFingerprint& fingerprint,
    const std::unordered_map<
        Enum, std::unordered_map<std::string, std::string, ::MMM::StringHash,
                                 std::equal_to<>>>& properties)
{
    std::uint64_t aggregateFirst  = 0;
    std::uint64_t aggregateSecond = 0;
    std::size_t   entryCount      = 0;
    for ( const auto& [source, values] : properties ) {
        for ( const auto& [key, value] : values ) {
            ObjectFingerprint entry;
            mixFingerprint(entry, static_cast<std::uint32_t>(source));
            mixFingerprint(entry, key);
            mixFingerprint(entry, value);
            aggregateFirst ^=
                std::rotl(entry.first, static_cast<int>(entry.second & 63U));
            aggregateSecond +=
                entry.second ^ (entry.first * 0xD6E8FEB86659FD93ULL);
            ++entryCount;
        }
    }
    mixFingerprint(fingerprint, entryCount);
    mixFingerprint(fingerprint, aggregateFirst);
    mixFingerprint(fingerprint, aggregateSecond);
}

/// @brief 计算一个根物件及其折线子物件的领域字段指纹。
/// @param note 根物件或递归子物件。
/// @return 覆盖协作文档全部物件字段的双 64 位指纹。
///
/// @details 浮点值按位模式混入，避免格式化差异；Polyline 额外按业务顺序递归
/// 混入子指纹。任何影响 encodeRootObject 输出的字段都必须同步加入此函数。
///
/// @warning 新增协作序列化字段时若遗漏指纹更新，快速缓存可能漏发该字段变化。
///
/// @par 类型覆盖
/// 公共字段先进入指纹，随后按类型加入 duration、dtrack 或 Polyline 子表。子表
/// 数量和每个子对象双指纹按顺序混入，因此插入、删除、重排或修改任一子 Note
/// 都会使根 Polyline 失效并重新编码。
ObjectFingerprint fingerprintObject(const ::MMM::Note& note)
{
    ObjectFingerprint result;
    mixFingerprint(result, static_cast<std::uint32_t>(note.m_type));
    mixFingerprint(result, std::bit_cast<std::uint64_t>(note.m_timestamp));
    mixFingerprint(result, static_cast<std::uint32_t>(note.m_track));
    mixFingerprint(result, note.m_collaborationId);
    mixPropertyMapFingerprint(result, note.m_metadata.note_properties);
    mixFingerprint(result, note.m_sampleBinding.has_value() ? 1U : 0U);
    if ( note.m_sampleBinding ) {
        mixFingerprint(result, note.m_sampleBinding->m_audioResourceId);
        mixFingerprint(
            result,
            std::bit_cast<std::uint32_t>(note.m_sampleBinding->m_volume));
    }
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        mixFingerprint(result,
                       std::bit_cast<std::uint64_t>(
                           static_cast<const ::MMM::Hold&>(note).m_duration));
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        mixFingerprint(result,
                       static_cast<std::uint32_t>(
                           static_cast<const ::MMM::Flick&>(note).m_dtrack));
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        const auto& polyline = static_cast<const ::MMM::Polyline&>(note);
        mixFingerprint(result, polyline.m_subNotes.size());
        for ( const auto& subNote : polyline.m_subNotes ) {
            const auto child = fingerprintObject(subNote.get());
            mixFingerprint(result, child.first);
            mixFingerprint(result, child.second);
        }
    }
    return result;
}

/// @brief 编码基线中一个可按稳定 ID 快速比较的物件。
///
/// @details encoded 与 fingerprint 对应同一对象版本；seenGeneration 标记当前
/// 扫描是否已经遇到该稳定 ID，同时用于发现重复 ID 和识别删除项。
struct CachedEncodedObject {
    /// @brief 领域字段快速指纹。
    ObjectFingerprint fingerprint;
    /// @brief 上次成功提交的完整根对象 JSON。
    Json encoded;
    /// @brief 最近一次扫描到该对象的代次。
    std::uint64_t seenGeneration{ 0 };
};

/// @brief 以 collaborationId 索引上次成功发送的根对象编码。
using ObjectEncodingCache =
    std::unordered_map<std::string, CachedEncodedObject>;

/// @brief 一次快速物件增量及编码成功后需要提交的缓存变化。
///
/// @details changed/removed 是暂存事务；只有外层负载编码成功后才写回缓存，
/// 保证编码失败不会让基线提前前进并漏掉下一次重试。
struct CachedObjectDelta {
    /// @brief 可直接放入 objects_delta 的 added/removed 对象。
    Json delta;
    /// @brief 成功后插入或替换的缓存对象。
    std::vector<std::pair<std::string, CachedEncodedObject>> changed;
    /// @brief 成功后从缓存删除的稳定 ID。
    std::vector<std::string> removed;
};

/// @brief 编码全部时间线条目。
/// @param beatmap 待编码谱面。
/// @return 保持容器顺序的 Timing 数组。
/// @note 同时保存 BPM 派生字段和效果参数，接收端不重新推断原始输入。
Json encodeTimelines(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& timing : beatmap.m_timings ) {
        result.push_back(Json{
            { "timestamp", timing.m_timestamp },
            { "bpm", timing.m_bpm },
            { "beat_length", timing.m_beat_length },
            { "effect", static_cast<std::uint32_t>(timing.m_timingEffect) },
            { "value", timing.m_timingEffectParameter },
            { "metadata", encodeTimingMetadata(timing.m_metadata) },
        });
    }
    return result;
}

/// @brief 编码全部独立音频采样事件。
/// @param beatmap 待编码谱面。
/// @return 包含稳定 ID、定位、资源、音量和元数据的数组。
Json encodeAudioSamples(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& sample : beatmap.m_audioSamples ) {
        result.push_back(Json{
            { "collaboration_id", sample.m_collaborationId },
            { "timestamp", sample.m_timestamp },
            { "offset_ms", sample.m_offsetMs },
            { "track", sample.m_track },
            { "resource", sample.m_audioResourceId },
            { "volume", sample.m_volume },
            { "metadata", encodeSampleMetadata(sample.m_metadata) },
        });
    }
    return result;
}

/// @brief 编码独立于物件几何的谱面多批注。
/// @param beatmap 待编码谱面。
/// @return 以批注稳定标识寻址的记录数组。
/// @note target_kind 与 target_id 共同表达时间戳或具体对象目标，增量层按批注
///       自身 collaboration_id 替换，不按目标字段合并。
Json encodeAnnotations(const ::MMM::BeatMap& beatmap)
{
    Json result = Json::array();
    for ( const auto& annotation : beatmap.m_annotations ) {
        result.push_back(Json{
            { "collaboration_id", annotation.m_id },
            { "target_kind",
              static_cast<std::uint32_t>(annotation.m_targetKind) },
            { "target_id", annotation.m_targetId },
            { "timestamp", annotation.m_timestamp },
            { "author", annotation.m_author },
            { "content", annotation.m_content },
        });
    }
    return result;
}

/// @brief 编码谱面基础元数据与扩展属性。
/// @param beatmap 待编码谱面。
/// @return metadata 分类对象。
///
/// @details 文件系统路径统一转换为 UTF-8 协议文本，避免平台原生 path 编码进入
/// 文档。基础字段整体替换，不计算字段级增量，以保持版本应用的原子性。
///
/// @par 路径字段
/// map_path、main_audio_path、song_file_hint、main_cover_path 与 cover_path
/// 都只 表达项目内已有语义，不读取文件系统，也不解析资源存在性。跨端路径转换由
/// Utf8Path helper 统一处理，编码阶段不执行 I/O。
Json encodeMetadata(const ::MMM::BeatMap& beatmap)
{
    const auto& base = beatmap.m_baseMapMetadata;
    Json        result{
        { "name", base.name },
        { "title", base.title },
        { "title_unicode", base.title_unicode },
        { "artist", base.artist },
        { "artist_unicode", base.artist_unicode },
        { "album", base.album },
        { "map_path", Config::pathToUtf8(base.map_path) },
        { "main_audio_path", Config::pathToUtf8(base.main_audio_path) },
        { "song_file_hint", Config::pathToUtf8(base.song_file_hint) },
        { "main_cover_path", Config::pathToUtf8(base.main_cover_path) },
        { "cover_path", Config::pathToUtf8(base.cover_path) },
        { "cover_type", static_cast<std::uint32_t>(base.cover_type) },
        { "video_starttime", base.video_starttime },
        { "bgxoffset", base.bgxoffset },
        { "bgyoffset", base.bgyoffset },
        { "version", base.version },
        { "author", base.author },
        { "preference_bpm", base.preference_bpm },
        { "track_count", base.track_count },
        { "bgm_track_count", base.bgm_track_count },
        { "map_length", base.map_length },
        { "extra", encodePropertyMap(beatmap.m_metadata.map_properties) },
    };
    return result;
}

template<typename Value>
/// @brief 按目标 C++ 类型严格读取 JSON 字段。
/// @tparam Value bool、string、浮点或整数值类型。
/// @param source JSON 对象。
/// @param key 必需字段名。
/// @param value 成功时接收转换结果。
/// @return 字段存在且 JSON 数值类别严格匹配时返回 true。
///
/// @note 浮点允许 JSON 整数，因为二者都属于合法 number；无符号整数拒绝负数，
///       有符号整数拒绝浮点，防止静默截断或环绕。
bool readValue(const Json& source, std::string_view key, Value& value)
{
    const auto iterator = source.find(key);
    if ( iterator == source.end() ) return false;
    if constexpr ( std::is_same_v<Value, bool> ) {
        if ( !iterator->is_boolean() ) return false;
    } else if constexpr ( std::is_same_v<Value, std::string> ) {
        if ( !iterator->is_string() ) return false;
    } else if constexpr ( std::is_floating_point_v<Value> ) {
        if ( !iterator->is_number() ) return false;
    } else if constexpr ( std::is_unsigned_v<Value> ) {
        if ( !iterator->is_number_unsigned() ) return false;
    } else if constexpr ( std::is_integral_v<Value> ) {
        if ( !iterator->is_number_integer() ) return false;
    }
    value = iterator->get<Value>();
    return true;
}

/// @brief 解码所有 Note 派生类型共享的字段。
/// @param source 单个物件 JSON。
/// @param note 已按 type 创建的目标对象。
/// @return 公共字段、元数据和采样绑定均合法时返回 true。
/// @note 函数再次读取 type 并写入基类，确保创建分支与编码字段一致。
bool decodeCommonNote(const Json& source, ::MMM::Note& note)
{
    // metadata 与 binding 是必需键，即使其值分别为空表和 null，也必须显式出现。
    std::uint32_t rawType    = 0;
    const auto    metadataIt = source.find("metadata");
    const auto    bindingIt  = source.find("binding");
    if ( !source.is_object() || !readValue(source, "type", rawType) ||
         rawType > static_cast<std::uint32_t>(::MMM::NoteType::POLYLINE) ||
         !readValue(source, "timestamp", note.m_timestamp) ||
         !readValue(source, "track", note.m_track) ||
         !readValue(source, "collaboration_id", note.m_collaborationId) ||
         metadataIt == source.end() || bindingIt == source.end() ||
         !decodePropertyMap(*metadataIt, note.m_metadata.note_properties) ||
         !decodeSampleBinding(*bindingIt, note.m_sampleBinding) ) {
        return false;
    }
    // 枚举范围已验证，可以安全写入实际对象的基类类型字段。
    note.m_type = static_cast<::MMM::NoteType>(rawType);
    return true;
}

/// @brief 按多态类型创建 Note，并重建可选 Polyline 父子引用。
/// @param source 单个物件 JSON。
/// @param beatmap 接收按类型存储对象的目标谱面。
/// @param parent 非空时表示当前对象是该 Polyline 的子物件。
/// @return 类型、字段与子结构全部合法时返回 true。
///
/// @details
/// 对象先 emplace 到稳定的按类型容器，再把引用加入 Polyline 的通用和专用子表。
/// 子对象只允许 NOTE/HOLD/FLICK，禁止嵌套 Polyline。根 Polyline 解码完子表后以
/// 首个子对象同步自身定位，恢复编辑器依赖的派生不变量。
///
/// @warning 失败可能在临时 BeatMap 中留下部分对象；materialize 只在全部分类
///          成功后返回该对象，因此部分状态不会逃逸。
///
/// @par 容器与引用
/// NOTE、HOLD、FLICK 分别落入 BeatMap 的类型容器。作为 Polyline 子物件时，
/// decoded 地址随后包装为 reference_wrapper 放入 m_subNotes，并按具体类型同时
/// 加入 m_subHolds 或 m_subFlicks。容器稳定性由 BeatMap 的对象存储契约保证。
///
/// @par Polyline 约束
/// parent 非空时拒绝 POLYLINE，防止递归嵌套形成当前模型无法表达的图。根
/// Polyline 必须显式携带 sub_notes 数组；空数组合法，其定位保留编码根字段。
/// 非空时则以首个子物件定位覆盖根字段，恢复运行期绘制所需不变量。
bool appendDecodedNote(const Json& source, ::MMM::BeatMap& beatmap,
                       ::MMM::Polyline* parent)
{
    std::uint32_t rawType = 0;
    if ( !source.is_object() || !readValue(source, "type", rawType) ||
         rawType > static_cast<std::uint32_t>(::MMM::NoteType::POLYLINE) ) {
        return false;
    }
    const auto   type    = static_cast<::MMM::NoteType>(rawType);
    ::MMM::Note* decoded = nullptr;
    if ( type == ::MMM::NoteType::NOTE ) {
        auto& note = beatmap.m_noteData.notes.emplace_back();
        decoded    = &note;
    } else if ( type == ::MMM::NoteType::HOLD ) {
        auto& hold = beatmap.m_noteData.holds.emplace_back();
        if ( !readValue(source, "duration", hold.m_duration) ) return false;
        decoded = &hold;
    } else if ( type == ::MMM::NoteType::FLICK ) {
        auto& flick = beatmap.m_noteData.flicks.emplace_back();
        if ( !readValue(source, "dtrack", flick.m_dtrack) ) return false;
        decoded = &flick;
    } else if ( parent == nullptr ) {
        auto& polyline = beatmap.m_noteData.polylines.emplace_back();
        decoded        = &polyline;
    } else {
        return false;
    }
    if ( !decodeCommonNote(source, *decoded) ) return false;
    if ( parent != nullptr ) {
        // 子物件先标记，再分别写入有序通用引用和按类型快速访问引用。
        decoded->m_isSubNote = true;
        parent->m_subNotes.emplace_back(*decoded);
        if ( type == ::MMM::NoteType::HOLD ) {
            parent->m_subHolds.emplace_back(
                static_cast<::MMM::Hold&>(*decoded));
        } else if ( type == ::MMM::NoteType::FLICK ) {
            parent->m_subFlicks.emplace_back(
                static_cast<::MMM::Flick&>(*decoded));
        }
        return true;
    }
    // 只有根 Polyline 拥有 sub_notes；普通根物件到此已完成。
    if ( type != ::MMM::NoteType::POLYLINE ) return true;

    auto&      polyline   = static_cast<::MMM::Polyline&>(*decoded);
    const auto subNotesIt = source.find("sub_notes");
    if ( subNotesIt == source.end() || !subNotesIt->is_array() ) return false;
    for ( const auto& subNote : *subNotesIt ) {
        if ( !appendDecodedNote(subNote, beatmap, &polyline) ) return false;
    }
    if ( !polyline.m_subNotes.empty() ) {
        polyline.m_timestamp = polyline.m_subNotes.front().get().m_timestamp;
        polyline.m_track     = polyline.m_subNotes.front().get().m_track;
    }
    return true;
}

/// @brief 解码完整根物件数组。
/// @param source objects 数组。
/// @param beatmap 接收对象的临时谱面。
/// @return 每个根对象及其子结构均合法时返回 true。
bool decodeObjects(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ) return false;
    for ( const auto& note : source ) {
        if ( !appendDecodedNote(note, beatmap, nullptr) ) return false;
    }
    return true;
}

/// @brief 解码完整时间线数组。
/// @param source timelines 数组。
/// @param beatmap 接收 Timing 的临时谱面。
/// @return 字段类型、效果枚举和元数据均合法时返回 true。
/// @note effect 先限制到已知枚举范围，再执行静态转换。
bool decodeTimelines(const Json& source, ::MMM::BeatMap& beatmap)
{
    // 每个 entry 在临时 BeatMap 容器中构造，整体失败时 materialize 丢弃对象。
    if ( !source.is_array() ) return false;
    for ( const auto& entry : source ) {
        auto&         timing     = beatmap.m_timings.emplace_back();
        std::uint32_t effect     = 0;
        const auto    metadataIt = entry.find("metadata");
        if ( !entry.is_object() ||
             !readValue(entry, "timestamp", timing.m_timestamp) ||
             !readValue(entry, "bpm", timing.m_bpm) ||
             !readValue(entry, "beat_length", timing.m_beat_length) ||
             !readValue(entry, "effect", effect) || effect > 3U ||
             !readValue(entry, "value", timing.m_timingEffectParameter) ||
             metadataIt == entry.end() ||
             !decodePropertyMap(*metadataIt,
                                timing.m_metadata.timing_properties) ) {
            return false;
        }
        // 仅在全部字段及元数据成功后提交已校验的效果枚举。
        timing.m_timingEffect = static_cast<::MMM::TimingEffect>(effect);
    }
    return true;
}

/// @brief 解码完整独立音频采样数组。
/// @param source audio_samples 数组。
/// @param beatmap 接收采样的临时谱面。
/// @return 全部字段有效且非空稳定 ID 不重复时返回 true。
///
/// @details 空 collaborationId 为兼容旧文档允许存在；一旦提供 ID，则限制长度
/// 并在当前数组内要求唯一，保证后续 identity delta 可无歧义寻址。
bool decodeAudioSamples(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ) return false;
    // 只索引非空 ID；空 ID 兼容旧文档且不能作为 identity delta 地址。
    std::unordered_set<std::string> identities;
    identities.reserve(source.size());
    for ( const auto& entry : source ) {
        auto&      sample     = beatmap.m_audioSamples.emplace_back();
        const auto metadataIt = entry.find("metadata");
        if ( !entry.is_object() ||
             !readValue(entry, "collaboration_id", sample.m_collaborationId) ||
             sample.m_collaborationId.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             (!sample.m_collaborationId.empty() &&
              !identities.insert(sample.m_collaborationId).second) ||
             !readValue(entry, "timestamp", sample.m_timestamp) ||
             !readValue(entry, "offset_ms", sample.m_offsetMs) ||
             !readValue(entry, "track", sample.m_track) ||
             !readValue(entry, "resource", sample.m_audioResourceId) ||
             !readValue(entry, "volume", sample.m_volume) ||
             metadataIt == entry.end() ||
             !decodePropertyMap(*metadataIt,
                                sample.m_metadata.sample_properties) ) {
            return false;
        }
    }
    return true;
}

/// @brief 解码独立时间戳与物件多批注。
/// @param source 批注数组。
/// @param beatmap 接收批注的谱面。
/// @return 结构、长度和稳定标识均有效时返回 true。
///
/// @details 批注数量、ID、目标 ID、有限时间戳、正文和作者逐项受限。作者为空
/// 合法；非空作者必须已经是规范形式。非时间戳目标必须携带 targetId，避免产生
/// 无法解析目标的悬空批注。
///
/// @par 身份规则
/// 每条批注自身 ID 必须非空、唯一且受长度限制；targetId 只在指向物件、时间线
/// 或采样时必需。二者职责不同，允许多条批注指向同一目标，但不允许两条批注
/// 共享同一 collaboration_id。
///
/// @par 文本规则
/// content 必须非空且不超过模型上限。author 可为空以兼容匿名历史批注，非空时
/// 必须等于统一规范化结果，避免不同客户端对同一显示身份产生不同字节表示。
bool decodeAnnotations(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_array() ||
         source.size() > ::MMM::MAX_BEATMAP_ANNOTATION_COUNT ) {
        return false;
    }

    std::unordered_set<std::string> annotationIdentities;
    annotationIdentities.reserve(source.size());
    for ( const auto& entry : source ) {
        ::MMM::BeatmapAnnotation annotation;
        std::uint32_t            targetKind = 0U;
        if ( !entry.is_object() ||
             !readValue(entry, "collaboration_id", annotation.m_id) ||
             !readValue(entry, "target_kind", targetKind) ||
             targetKind >
                 static_cast<std::uint32_t>(
                     ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE) ||
             !readValue(entry, "target_id", annotation.m_targetId) ||
             !readValue(entry, "timestamp", annotation.m_timestamp) ||
             !readValue(entry, "author", annotation.m_author) ||
             !readValue(entry, "content", annotation.m_content) ||
             annotation.m_id.empty() ||
             annotation.m_id.size() > ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             annotation.m_targetId.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_ID_BYTES ||
             !std::isfinite(annotation.m_timestamp) ||
             annotation.m_content.empty() ||
             annotation.m_content.size() >
                 ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES ||
             (!annotation.m_author.empty() &&
              Config::normalizeCreatorIdentity(annotation.m_author) !=
                  annotation.m_author) ||
             !annotationIdentities.insert(annotation.m_id).second ) {
            return false;
        }
        annotation.m_targetKind =
            static_cast<::MMM::BeatmapAnnotationTargetKind>(targetKind);
        if ( annotation.m_targetKind !=
                 ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP &&
             annotation.m_targetId.empty() ) {
            return false;
        }
        beatmap.m_annotations.push_back(std::move(annotation));
    }
    return true;
}

/// @brief 解码基础谱面元数据与扩展属性。
/// @param source metadata 对象。
/// @param beatmap 接收结果的临时谱面。
/// @return 所有必需字段、枚举和扩展映射有效时返回 true。
///
/// @details 路径先以 UTF-8 字符串读取，只有完整字段校验成功后才转换成平台
/// filesystem::path。coverType 限制为已知范围，避免非法整数进入业务枚举。
///
/// @par 完整替换约束
/// metadata 不允许缺少“未变化”字段，因为整个分类按对象替换。即使字段当前为空
/// 或为零，发送端也必须显式编码，接收端据此清除旧值而不是意外保留旧元数据。
bool decodeMetadata(const Json& source, ::MMM::BeatMap& beatmap)
{
    if ( !source.is_object() ) return false;
    // 路径先保留协议 UTF-8 文本，避免字段校验中途修改平台 path 成员。
    auto&         base = beatmap.m_baseMapMetadata;
    std::string   mapPath;
    std::string   mainAudioPath;
    std::string   songFileHint;
    std::string   mainCoverPath;
    std::string   coverPath;
    std::uint32_t coverType = 0;
    const auto    extraIt   = source.find("extra");
    // 旧协作快照缺少专辑时清空目标值，防止整体替换误保留上一版元数据。
    // 若字段存在仍必须按字符串校验，错误类型应使整个快照解码失败。
    base.album.clear();
    if ( !readValue(source, "name", base.name) ||
         !readValue(source, "title", base.title) ||
         !readValue(source, "title_unicode", base.title_unicode) ||
         !readValue(source, "artist", base.artist) ||
         !readValue(source, "artist_unicode", base.artist_unicode) ||
         (source.contains("album") &&
          !readValue(source, "album", base.album)) ||
         !readValue(source, "map_path", mapPath) ||
         !readValue(source, "main_audio_path", mainAudioPath) ||
         !readValue(source, "song_file_hint", songFileHint) ||
         !readValue(source, "main_cover_path", mainCoverPath) ||
         !readValue(source, "cover_path", coverPath) ||
         !readValue(source, "cover_type", coverType) || coverType > 1U ||
         !readValue(source, "video_starttime", base.video_starttime) ||
         !readValue(source, "bgxoffset", base.bgxoffset) ||
         !readValue(source, "bgyoffset", base.bgyoffset) ||
         !readValue(source, "version", base.version) ||
         !readValue(source, "author", base.author) ||
         !readValue(source, "preference_bpm", base.preference_bpm) ||
         !readValue(source, "track_count", base.track_count) ||
         !readValue(source, "bgm_track_count", base.bgm_track_count) ||
         !readValue(source, "map_length", base.map_length) ||
         extraIt == source.end() ||
         !decodePropertyMap(*extraIt, beatmap.m_metadata.map_properties) ) {
        return false;
    }
    // 所有协议字段验证完成后再提交路径和枚举，保持解码阶段清晰分层。
    base.map_path        = Config::utf8ToPath(mapPath);
    base.main_audio_path = Config::utf8ToPath(mainAudioPath);
    base.song_file_hint  = Config::utf8ToPath(songFileHint);
    base.main_cover_path = Config::utf8ToPath(mainCoverPath);
    base.cover_path      = Config::utf8ToPath(coverPath);
    base.cover_type      = static_cast<::MMM::CoverType>(coverType);
    return true;
}

/// @brief 编码不含协议头的完整谱面分类文档。
/// @param beatmap 当前完整谱面。
/// @return 恰好包含五个业务分类的 JSON 对象。
/// @note version 与 snapshot 属于外层补丁语义，由调用方在发送前添加。
Json makeDocument(const ::MMM::BeatMap& beatmap)
{
    return Json{
        { "objects", encodeObjects(beatmap) },
        { "timelines", encodeTimelines(beatmap) },
        { "audio_samples", encodeAudioSamples(beatmap) },
        { "metadata", encodeMetadata(beatmap) },
        { "annotations", encodeAnnotations(beatmap) },
    };
}

/// @brief 只编码本次增量实际涉及的谱面类别。
/// @param beatmap 当前完整谱面。
/// @param flags 本次需要参与增量比较的类别。
/// @return 仅包含指定类别的临时文档。
///
/// @details Objects 有独立快速缓存路径，因此这里不编码；其他分类按 flags 惰性
/// 编码，避免一次局部编辑遍历所有谱面数据。结果仍是完整分类当前值，随后与
/// encodingBaseline 计算 added/removed。
Json makeMutationDocument(const ::MMM::BeatMap&       beatmap,
                          ::MMM::BeatmapMutationFlags flags)
{
    Json result = Json::object();
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Timelines) ) {
        result["timelines"] = encodeTimelines(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::AudioSamples) ) {
        result["audio_samples"] = encodeAudioSamples(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Metadata) ) {
        result["metadata"] = encodeMetadata(beatmap);
    }
    if ( hasBeatmapMutationFlag(flags,
                                ::MMM::BeatmapMutationFlags::Annotations) ) {
        result["annotations"] = encodeAnnotations(beatmap);
    }
    return result;
}

/// @brief 计算两个数组间保留重复项数量的增删集合。
/// @param before 已发送基线数组。
/// @param after 当前分类数组。
/// @return added 与 removed 数组组成的增量对象。
///
/// @details 元素以 JSON dump 作为完整值键并计算多重集计数差。随后按 after 和
/// before 原顺序输出，既保留重复元素数量，也让编码结果具备可理解顺序。该路径
/// 用于无稳定 ID 的时间线、采样，以及稳定 ID 不完整时的兼容回退。
Json makeArrayDelta(const Json& before, const Json& after)
{
    // 负数表示基线中尚需删除的副本，正数表示当前状态新增的副本。
    std::unordered_map<std::string, std::int64_t> countDifference;
    countDifference.reserve(before.size() + after.size());
    for ( const auto& value : before ) --countDifference[value.dump()];
    for ( const auto& value : after ) ++countDifference[value.dump()];

    // 按 after 顺序消费正差额，保留当前状态中重复元素的出现顺序。
    Json added = Json::array();
    for ( const auto& value : after ) {
        auto& remaining = countDifference[value.dump()];
        if ( remaining <= 0 ) continue;
        added.push_back(value);
        --remaining;
    }

    // 按 before 顺序消费负差额，携带接收端需要移除的完整旧值。
    Json removed = Json::array();
    for ( const auto& value : before ) {
        auto& remaining = countDifference[value.dump()];
        if ( remaining >= 0 ) continue;
        removed.push_back(value);
        ++remaining;
    }
    return Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
}

/// @brief 读取物件或注释条目的稳定协作标识。
/// @param value 协作文档数组中的单个条目。
/// @return 标识合法且非空时返回其只读地址。
/// @warning 返回指针引用 JSON 内部字符串，仅在 value 未修改且仍存活时有效。
const std::string* collaborationIdentity(const Json& value)
{
    if ( !value.is_object() ) return nullptr;
    const auto identity = value.find("collaboration_id");
    if ( identity == value.end() || !identity->is_string() ||
         identity->get_ref<const std::string&>().empty() ) {
        return nullptr;
    }
    return &identity->get_ref<const std::string&>();
}

/// @brief 只编码相对基线实际变化的玩家物件。
/// @param cache 上一次成功编码后的稳定 ID 物件缓存。
/// @param generation 本轮扫描标识。
/// @param beatmap 当前完整谱面。
/// @return 标识完整且唯一时返回轻量增量，否则返回空以触发兼容路径。
///
/// @details
/// 每个根对象按稳定 ID 查缓存。已存在且指纹相同的对象只更新 seenGeneration；
/// 指纹变化则同时输出旧编码到 removed、新编码到 added，并暂存缓存替换。扫描
/// 结束后，未见于当前代次的缓存对象作为删除项。
///
/// 新谱面中缺失或重复稳定 ID 会使整个快速路径失效，调用方改用完整 JSON 编码
/// 与兼容多重集比较。缓存只记录成功发送后的状态，当前函数不直接提交变化。
///
/// @par 代次标记
/// 缓存对象不在每轮扫描前整体清零，而是写入单调 generation。扫描结束时，
/// seenGeneration 不等于当前值的条目即为删除；只有 generation 极端回绕时才
/// 执行一次全量归零。
///
/// @par 事务边界
/// 已有对象的 encoded/fingerprint 不会在扫描时立即覆盖。变化内容保存在
/// result.changed，删除 ID 保存在 result.removed；只有外层编码成功后才提交。
std::optional<CachedObjectDelta> makeObjectIdentityDelta(
    ObjectEncodingCache& cache, std::uint64_t generation,
    const ::MMM::BeatMap& beatmap)
{
    std::unordered_set<std::string_view> newIdentities;
    Json                                 added   = Json::array();
    Json                                 removed = Json::array();
    CachedObjectDelta                    result;
    bool                                 valid = true;
    // append 同时验证 ID、计算指纹并构造事务性缓存变化。
    const auto append = [&](const ::MMM::Note& note) {
        if ( !valid || note.m_collaborationId.empty() ) {
            valid = false;
            return;
        }
        const auto fingerprint = fingerprintObject(note);
        const auto previous    = cache.find(note.m_collaborationId);
        if ( previous != cache.end() ) {
            // 同一代再次看到同一缓存 ID 表示当前谱面根对象身份重复。
            if ( previous->second.seenGeneration == generation ) {
                valid = false;
                return;
            }
            previous->second.seenGeneration = generation;
            if ( previous->second.fingerprint == fingerprint ) return;
            removed.push_back(previous->second.encoded);
        } else if ( !newIdentities.emplace(note.m_collaborationId).second ) {
            valid = false;
            return;
        }
        auto encoded = encodeRootObject(note);
        added.push_back(encoded);
        result.changed.emplace_back(
            note.m_collaborationId,
            CachedEncodedObject{ fingerprint, std::move(encoded), generation });
    };

    std::unordered_set<const ::MMM::Note*> polylineSubNotes;
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        for ( const auto& subNote : polyline.m_subNotes ) {
            polylineSubNotes.insert(&subNote.get());
        }
    }
    for ( const auto& note : beatmap.m_noteData.notes ) {
        if ( !note.m_isSubNote && !polylineSubNotes.contains(&note) ) {
            append(note);
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        if ( !hold.m_isSubNote && !polylineSubNotes.contains(&hold) ) {
            append(hold);
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        if ( !flick.m_isSubNote && !polylineSubNotes.contains(&flick) ) {
            append(flick);
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines )
        append(polyline);
    if ( !valid ) return std::nullopt;

    // 上一基线存在但本轮未见的对象属于删除，不立即擦除缓存。
    for ( const auto& [identity, cached] : cache ) {
        if ( cached.seenGeneration == generation ) continue;
        removed.push_back(cached.encoded);
        result.removed.push_back(identity);
    }
    result.delta = Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
    return result;
}

/// @brief 提交一次已成功编码的物件缓存变化。
/// @param cache 上一成功基线缓存。
/// @param delta 已成功封装并可视为发送基线的暂存变化。
///
/// @details 先删除消失 ID，再插入或替换变化对象。调用方必须只在外层负载编码
/// 成功且 encodingBaseline 同步更新后调用，维持缓存与基线一致。
void commitObjectIdentityDelta(ObjectEncodingCache& cache,
                               CachedObjectDelta&   delta)
{
    for ( const auto& identity : delta.removed ) cache.erase(identity);
    for ( auto& [identity, object] : delta.changed ) {
        cache.insert_or_assign(std::move(identity), std::move(object));
    }
}

/// @brief 从完整谱面建立稳定 ID 物件编码缓存。
/// @param beatmap 当前完整谱面。
/// @return 全部根对象拥有唯一非空 ID 时返回缓存，否则返回空。
/// @note 复用增量扫描实现，空缓存使所有对象进入 changed，再统一提交。
std::optional<ObjectEncodingCache> makeObjectEncodingCache(
    const ::MMM::BeatMap& beatmap)
{
    ObjectEncodingCache cache;
    auto                initial = makeObjectIdentityDelta(cache, 1U, beatmap);
    if ( !initial ) return std::nullopt;
    commitObjectIdentityDelta(cache, *initial);
    return cache;
}

/// @brief 将稳定 ID 物件增量应用到编码基线数组。
/// @param target 当前编码基线的物件数组。
/// @param delta 已成功发送的物件增量。
/// @return 输入结构完整时返回 true。
///
/// @details added 与 removed 中相同 ID 表示替换。先索引 added，再遍历 removed
/// 就地替换或删除，最后补入尚未应用的新增项。若目标缺少待替换 ID，新增阶段
/// 仍会追加当前值，使重复应用趋向相同结果。
///
/// @par 幂等倾向
/// 对同 ID 替换优先覆盖已有元素；纯删除在目标不存在时无操作；纯新增若目标已
/// 有同 ID 则覆盖而非追加。重复收到相同身份差量时不会制造重复对象，但协议层
/// 仍负责版本排序，本函数不判断差量新旧。
bool applyIdentityDeltaToArray(Json& target, const Json& delta)
{
    // 先验证完整 delta 形状，避免应用部分 added 后才发现 removed 非法。
    const auto added   = delta.find("added");
    const auto removed = delta.find("removed");
    if ( !target.is_array() || !delta.is_object() || added == delta.end() ||
         !added->is_array() || removed == delta.end() ||
         !removed->is_array() ) {
        return false;
    }
    // string_view 和 Json 指针都只借用 delta，整个函数内 delta 不被修改。
    std::unordered_map<std::string_view, const Json*> replacements;
    replacements.reserve(added->size());
    for ( const auto& value : *added ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity || !replacements.emplace(*identity, &value).second ) {
            return false;
        }
    }
    // removed 条目也必须具有身份，即使对应 ID 同时出现在 replacements 中。
    for ( const auto& value : *removed ) {
        if ( !collaborationIdentity(value) ) return false;
    }
    // 基线数组规模通常小于全谱面对象数，线性定位换取保持原有顺序。
    const auto findByIdentity = [&](std::string_view identity) {
        return std::find_if(
            target.begin(), target.end(), [identity](const Json& item) {
                const auto* itemIdentity = collaborationIdentity(item);
                return itemIdentity && *itemIdentity == identity;
            });
    };
    std::unordered_set<std::string_view> appliedReplacements;
    appliedReplacements.reserve(added->size());
    // 第一遍处理替换和纯删除，尽量保持目标数组中未变对象的相对顺序。
    for ( const auto& value : *removed ) {
        const auto& identity    = *collaborationIdentity(value);
        const auto  replacement = replacements.find(identity);
        auto        existing    = findByIdentity(identity);
        if ( replacement != replacements.end() ) {
            if ( existing != target.end() ) {
                *existing = *replacement->second;
                appliedReplacements.emplace(identity);
            }
        } else if ( existing != target.end() ) {
            target.erase(existing);
        }
    }
    // 第二遍补入纯新增，或覆盖缺少旧 removed 记录的同 ID 当前值。
    for ( const auto& value : *added ) {
        const auto& identity = *collaborationIdentity(value);
        if ( appliedReplacements.contains(identity) ) continue;
        auto existing = findByIdentity(identity);
        if ( existing != target.end() ) {
            *existing = value;
        } else {
            target.push_back(value);
        }
    }
    return true;
}

/// @brief 按稳定协作标识计算物件或注释的增删集合。
/// @param before 编码基线数组。
/// @param after 当前状态数组。
/// @return 标识缺失或重复时回退到兼容的完整值比较结果。
///
/// @details 唯一 ID 允许把同一对象字段变化表示为 removed old + added new，而非
/// 把数组位置变化误认为批量增删。输出顺序分别跟随 after 和 before，删除对象
/// 仍保留完整旧值供兼容接收端按值移除。
Json makeIdentityArrayDelta(const Json& before, const Json& after)
{
    // 两侧索引均要求身份唯一；任何歧义都整批回退，不混用两套算法。
    std::unordered_map<std::string_view, const Json*> beforeByIdentity;
    std::unordered_map<std::string_view, const Json*> afterByIdentity;
    beforeByIdentity.reserve(before.size());
    afterByIdentity.reserve(after.size());
    // 最后一遍追加只在旧文档存在而新文档缺失的纯删除项。
    for ( const auto& value : before ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity ||
             !beforeByIdentity.emplace(*identity, &value).second ) {
            return makeArrayDelta(before, after);
        }
    }
    // 新 ID 或同 ID 内容变化都进入 added；内容变化同时移除旧版本。
    for ( const auto& value : after ) {
        const auto* identity = collaborationIdentity(value);
        if ( !identity || !afterByIdentity.emplace(*identity, &value).second ) {
            return makeArrayDelta(before, after);
        }
    }

    Json added   = Json::array();
    Json removed = Json::array();
    for ( const auto& value : after ) {
        const auto& identity = *collaborationIdentity(value);
        const auto  previous = beforeByIdentity.find(identity);
        if ( previous == beforeByIdentity.end() ||
             *previous->second != value ) {
            added.push_back(value);
            if ( previous != beforeByIdentity.end() ) {
                removed.push_back(*previous->second);
            }
        }
    }
    for ( const auto& value : before ) {
        const auto& identity = *collaborationIdentity(value);
        if ( !afterByIdentity.contains(identity) ) removed.push_back(value);
    }
    return Json{
        { "added", std::move(added) },
        { "removed", std::move(removed) },
    };
}

/// @brief 判断数组增量是否没有任何净变化。
/// @param delta 已验证含 added/removed 的对象。
/// @return 两个数组都为空时返回 true。
bool arrayDeltaEmpty(const Json& delta)
{
    return delta.at("added").empty() && delta.at("removed").empty();
}

/// @brief 基于逻辑线程上一次实际状态构造可合并的分类增量。
/// @param baseline 上次成功编码后的完整分类基线。
/// @param current 本次 flags 涉及分类的当前编码。
/// @param flags 上层声明发生变化的分类集合。
/// @param objectDelta 可选快速稳定 ID 物件增量。
/// @return 至少有一个实际变化时返回 patch，否则返回空。
///
/// @details 数组分类统一生成 added/removed；Objects 优先使用快速缓存结果，
/// Objects 与 Annotations 具备稳定 ID 时按身份比较，其余数组按完整值多重集。
/// Metadata 以完整对象替换。flags 只是候选范围，实际无差异的分类不会写入补丁。
///
/// @par 分类策略
/// Objects 变化频繁且数量大，使用领域指纹缓存；Annotations 数量受限但也有稳定
/// ID，直接在 JSON 上按身份比较；Timelines 与 AudioSamples 为兼容历史空 ID，
/// 使用完整值多重集。不同策略最终统一生成相同 added/removed 线格式。
std::optional<Json> makeIncrementalPatch(const Json&                 baseline,
                                         const Json&                 current,
                                         ::MMM::BeatmapMutationFlags flags,
                                         std::optional<Json> objectDelta)
{
    Json patch{
        { "version", DOCUMENT_FORMAT_VERSION },
        { "snapshot", false },
    };
    bool changed = false;
    // 局部 lambda 统一分类选择、差量算法和空差量抑制规则。
    const auto appendArrayDelta = [&](std::string_view            category,
                                      std::string_view            deltaKey,
                                      ::MMM::BeatmapMutationFlags flag) {
        if ( !hasBeatmapMutationFlag(flags, flag) ) return;
        const bool identityAddressed =
            category == "objects" || category == "annotations";
        Json delta;
        if ( flag == ::MMM::BeatmapMutationFlags::Objects && objectDelta ) {
            delta = std::move(*objectDelta);
        } else {
            delta = identityAddressed
                        ? makeIdentityArrayDelta(baseline.at(category),
                                                 current.at(category))
                        : makeArrayDelta(baseline.at(category),
                                         current.at(category));
        }
        if ( arrayDeltaEmpty(delta) ) return;
        patch[std::string(deltaKey)] = std::move(delta);
        changed                      = true;
    };
    appendArrayDelta(
        "objects", "objects_delta", ::MMM::BeatmapMutationFlags::Objects);
    appendArrayDelta(
        "timelines", "timelines_delta", ::MMM::BeatmapMutationFlags::Timelines);
    appendArrayDelta("audio_samples",
                     "audio_samples_delta",
                     ::MMM::BeatmapMutationFlags::AudioSamples);
    appendArrayDelta("annotations",
                     "annotations_delta",
                     ::MMM::BeatmapMutationFlags::Annotations);
    if ( hasBeatmapMutationFlag(flags, ::MMM::BeatmapMutationFlags::Metadata) &&
         baseline.at("metadata") != current.at("metadata") ) {
        patch["metadata"] = current.at("metadata");
        changed           = true;
    }
    if ( !changed ) return std::nullopt;
    return patch;
}
}  // namespace

/// @brief 编解码器持有的接收文档与发送基线状态。
///
/// @details
/// document 是已应用的接收侧权威 JSON；encodingBaseline 是上次成功编码的
/// 发送侧状态。两者可以来自同一快照，也可以分别推进。物件缓存是发送基线的
/// 派生加速结构，任何接收 apply 都会使其失效，防止缓存与新文档版本错配。
class BeatmapDocumentCodec::Impl
{
public:
    /// @brief 当前接收侧完整分类文档。
    Json document = Json::object();
    /// @brief document 是否已经由快照初始化。
    bool hasDocument{ false };
    /// @brief 当前发送侧最近成功编码的分类基线。
    Json encodingBaseline = Json::object();
    /// @brief encodingBaseline 是否可用于生成增量。
    bool hasEncodingBaseline{ false };
    /// @brief 玩家物件稳定 ID 到上次编码值的缓存。
    ObjectEncodingCache objectEncodingCache;
    /// @brief objectEncodingCache 是否完整覆盖当前 objects 基线。
    bool hasObjectEncodingCache{ false };
    /// @brief 快速扫描代次；零值保留给回绕清理。
    std::uint64_t objectEncodingGeneration{ 1 };
};

/// @brief 后台已经完成全部扫描和编码的玩家物件基线。
///
/// @details 该值允许低频后台任务预先完成全谱面对象扫描；逻辑线程随后只移动
/// objects JSON 与缓存进入 Codec，不在提交路径重复遍历。shared_ptr 用于跨线程
/// 传递已经封闭的所有权，安装后内容被移动并不可复用。
class BeatmapDocumentCodec::ObjectEncodingBaseline
{
public:
    /// @brief 编码基线中的完整玩家物件数组。
    /// @note 必须与 cache 描述同一 BeatMap 版本。
    Json objects = Json::array();
    /// @brief 按稳定标识索引的领域物件编码缓存。
    /// @note 仅当全部根物件 ID 唯一且非空时才会构造该对象。
    ObjectEncodingCache cache;
};

/// @brief 创建空的协作文档编解码状态。
BeatmapDocumentCodec::BeatmapDocumentCodec() : m_impl(std::make_unique<Impl>())
{
}

/// @brief 销毁 PImpl 状态。
BeatmapDocumentCodec::~BeatmapDocumentCodec() = default;

/// @brief 编码完整快照或相对成功基线的分类增量。
/// @param beatmap 当前业务谱面。
/// @param flags 增量候选分类；快照模式下忽略分类裁剪。
/// @param snapshot true 生成完整快照，false 生成可合并增量。
/// @return 带外层头的 CBOR 负载，或明确编码错误。
///
/// @details
/// 快照成功后同时更新完整 encodingBaseline 并尝试建立物件快速缓存。增量要求
/// 已有发送基线；若只有接收 document，则先复制它作为基线。Objects 优先按
/// 稳定 ID 和领域指纹生成差量，无法保证唯一身份时回退完整 JSON 比较。
///
/// 只有补丁成功封装为负载后，分类基线和物件缓存才提交变化。因此压缩、CBOR
/// 或尺寸失败不会提前推进发送水位线，调用方可以用同一 BeatMap 安全重试。
///
/// @warning 该对象由单一逻辑线程使用，内部状态不提供并发同步。
///
/// @par 快照路径
/// snapshot=true 时无论 flags 为何都编码五分类，并在 payload 副本加入版本字段。
/// 成功后 encodingBaseline 更新为不含协议头字段的 current；对象 ID 完整时一并
/// 建立快速缓存，否则仅禁用优化，快照本身仍然有效。
///
/// @par 增量路径
/// snapshot=false 且 flags=None 立即返回 EmptyPayload。若缺少任何可用基线则返回
/// MissingSnapshot。实际比较后没有净变化也返回 EmptyPayload，避免发送空版本。
/// 分类基线只在 encodeDocumentPayload 成功后更新。
///
/// @par 快速对象路径
/// objectEncodingGeneration 每轮递增，缓存扫描只重编码指纹变化对象。ID 不完整
/// 时 current 临时补入完整 objects，并通过 makeIdentityArrayDelta 或多重集回退
/// 生成相同线格式；正确性不依赖缓存存在。
std::expected<ByteBuffer, BeatmapDocumentError> BeatmapDocumentCodec::encode(
    const ::MMM::BeatMap& beatmap, ::MMM::BeatmapMutationFlags flags,
    bool snapshot)
{
    if ( !snapshot && flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    // 快照编码全部分类；增量只先编码 flags 涉及的非 Objects 分类。
    Json current =
        snapshot ? makeDocument(beatmap) : makeMutationDocument(beatmap, flags);
    if ( snapshot ) {
        // 协议字段只加入发送 payload，不污染五分类 encodingBaseline。
        Json payload        = current;
        payload["version"]  = DOCUMENT_FORMAT_VERSION;
        payload["snapshot"] = true;
        auto encoded        = encodeDocumentPayload(payload);
        // 发送负载成功后才把当前状态确认为下一次增量基线。
        if ( encoded.has_value() ) {
            m_impl->encodingBaseline    = current;
            m_impl->hasEncodingBaseline = true;
            auto cache                  = makeObjectEncodingCache(beatmap);
            if ( cache ) {
                m_impl->objectEncodingCache    = std::move(*cache);
                m_impl->hasObjectEncodingCache = true;
            } else {
                m_impl->objectEncodingCache.clear();
                m_impl->hasObjectEncodingCache = false;
            }
            m_impl->objectEncodingGeneration = 1;
        }
        return encoded;
    }
    // 接收过完整文档但尚未主动编码时，可从 document 延续增量发送。
    if ( !m_impl->hasEncodingBaseline ) {
        if ( !m_impl->hasDocument ) {
            return std::unexpected(BeatmapDocumentError::MissingSnapshot);
        }
        m_impl->encodingBaseline    = m_impl->document;
        m_impl->hasEncodingBaseline = true;
    }
    std::optional<CachedObjectDelta> cachedObjectDelta;
    std::optional<Json>              objectDelta;
    if ( hasBeatmapMutationFlag(flags, ::MMM::BeatmapMutationFlags::Objects) ) {
        // 快速路径通过代次标记找出变化与删除，不重复 JSON 编码未变对象。
        if ( m_impl->hasObjectEncodingCache ) {
            ++m_impl->objectEncodingGeneration;
            // uint64 回绕时清零所有 seen
            // 标记，再从代次一继续，避免零与旧值碰撞。
            if ( m_impl->objectEncodingGeneration == 0 ) {
                for ( auto& [identity, object] : m_impl->objectEncodingCache ) {
                    static_cast<void>(identity);
                    object.seenGeneration = 0;
                }
                m_impl->objectEncodingGeneration = 1;
            }
            cachedObjectDelta =
                makeObjectIdentityDelta(m_impl->objectEncodingCache,
                                        m_impl->objectEncodingGeneration,
                                        beatmap);
            if ( cachedObjectDelta ) {
                objectDelta = cachedObjectDelta->delta;
            }
        }
        // 缓存不可用或身份不完整时，编码完整当前 objects 供兼容差量算法比较。
        if ( !objectDelta ) current["objects"] = encodeObjects(beatmap);
    }
    const bool usesFastObjectDelta = objectDelta.has_value();
    auto       patch               = makeIncrementalPatch(
        m_impl->encodingBaseline, current, flags, std::move(objectDelta));
    // flags 涉及的分类没有实际变化时不发送空网络负载。
    if ( !patch.has_value() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto encoded = encodeDocumentPayload(*patch);
    if ( !encoded.has_value() ) return encoded;

    // 负载已经成功生成，现在按分类事务性推进发送基线和派生缓存。
    const auto updateBaseline = [&](std::string_view            category,
                                    ::MMM::BeatmapMutationFlags flag) {
        if ( hasBeatmapMutationFlag(flags, flag) ) {
            if ( flag == ::MMM::BeatmapMutationFlags::Objects ) {
                const auto delta = patch->find("objects_delta");
                if ( usesFastObjectDelta && delta != patch->end() ) {
                    // 快速差量必须同时成功应用 JSON 基线并提交缓存事务。
                    if ( applyIdentityDeltaToArray(
                             m_impl->encodingBaseline["objects"], *delta) &&
                         cachedObjectDelta ) {
                        commitObjectIdentityDelta(m_impl->objectEncodingCache,
                                                  *cachedObjectDelta);
                    } else {
                        // 基线结构异常时宁可失去优化，下一轮回退完整编码保证正确。
                        m_impl->objectEncodingCache.clear();
                        m_impl->hasObjectEncodingCache = false;
                    }
                } else if ( !usesFastObjectDelta ) {
                    // 兼容路径直接采用本轮完整
                    // objects，并尝试重新建立快速缓存。
                    m_impl->encodingBaseline["objects"] = current.at("objects");
                    auto cache = makeObjectEncodingCache(beatmap);
                    if ( cache ) {
                        m_impl->objectEncodingCache      = std::move(*cache);
                        m_impl->hasObjectEncodingCache   = true;
                        m_impl->objectEncodingGeneration = 1;
                    } else {
                        m_impl->objectEncodingCache.clear();
                        m_impl->hasObjectEncodingCache = false;
                    }
                }
                return;
            }
            m_impl->encodingBaseline[std::string(category)] =
                current.at(category);
        }
    };
    updateBaseline("objects", ::MMM::BeatmapMutationFlags::Objects);
    updateBaseline("timelines", ::MMM::BeatmapMutationFlags::Timelines);
    updateBaseline("audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples);
    updateBaseline("metadata", ::MMM::BeatmapMutationFlags::Metadata);
    updateBaseline("annotations", ::MMM::BeatmapMutationFlags::Annotations);
    return encoded;
}

/// @brief 用上层已确认同步的 BeatMap 重置全部发送基线。
/// @param beatmap 已成为逻辑线程当前事实的完整谱面。
///
/// @details 该入口在远端快照应用或合并完成后调用，使后续本地增量相对上层实际
/// 状态计算。完整文档与快速缓存来自同一 BeatMap；身份无效时只禁用缓存优化。
void BeatmapDocumentCodec::synchronizeEncodingBaseline(
    const ::MMM::BeatMap& beatmap)
{
    // 五分类基线一次构造，后续 flags 增量可直接读取未涉及分类的稳定状态。
    m_impl->encodingBaseline    = makeDocument(beatmap);
    m_impl->hasEncodingBaseline = true;
    // 快速缓存失败只影响性能，不撤销已经有效的完整 JSON 基线。
    auto cache = makeObjectEncodingCache(beatmap);
    if ( cache ) {
        m_impl->objectEncodingCache    = std::move(*cache);
        m_impl->hasObjectEncodingCache = true;
    } else {
        m_impl->objectEncodingCache.clear();
        m_impl->hasObjectEncodingCache = false;
    }
    m_impl->objectEncodingGeneration = 1;
}

std::shared_ptr<BeatmapDocumentCodec::ObjectEncodingBaseline>
/// @brief 预计算可跨线程移交的玩家物件编码基线。
/// @param beatmap 后台任务持有的稳定谱面快照。
/// @return 身份完整时返回 objects 与缓存，否则返回空指针。
///
/// @details 先建立缓存验证全部稳定 ID，再编码完整 objects。调用方应保证 beatmap
/// 在函数期间不变；返回对象之后不再引用原 BeatMap。
BeatmapDocumentCodec::prepareObjectEncodingBaseline(
    const ::MMM::BeatMap& beatmap)
{
    // 先验证身份并构造缓存，失败时省去无意义的完整 objects JSON 编码。
    auto cache = makeObjectEncodingCache(beatmap);
    if ( !cache ) return nullptr;

    auto baseline     = std::make_shared<ObjectEncodingBaseline>();
    baseline->objects = encodeObjects(beatmap);
    baseline->cache   = std::move(*cache);
    return baseline;
}

/// @brief 安装后台预计算的玩家物件编码基线。
/// @param baseline prepareObjectEncodingBaseline 的结果，所有权在成功时被移动。
///
/// @details 只有完整 encodingBaseline 已存在时才安装，因为单独 objects 无法
/// 构成可发送基线。无效输入保持当前状态不变；成功后重置扫描代次。
void BeatmapDocumentCodec::synchronizeObjectEncodingBaseline(
    std::shared_ptr<ObjectEncodingBaseline> baseline)
{
    // 没有完整五分类基线时，单独安装 objects 会造成其他分类无法比较。
    if ( !baseline || !m_impl->hasEncodingBaseline ) return;
    m_impl->encodingBaseline["objects"] = std::move(baseline->objects);
    m_impl->objectEncodingCache         = std::move(baseline->cache);
    m_impl->hasObjectEncodingCache      = true;
    m_impl->objectEncodingGeneration    = 1;
}

std::expected<BeatmapPatchResult, BeatmapDocumentError>
/// @brief 只检查负载结构并报告涉及分类，不修改当前文档。
/// @param payload 完整协作文档二进制负载。
/// @return 快照标记与 mutation flags，或解析/结构错误。
///
/// @details inspect 用于路由和预检：先解外层负载并验证内层版本，再按 snapshot
/// 选择完整分类或 delta 分类形状。快照必须恰好包含全部五类；增量至少包含一个
/// 实际分类。这里不深度解码业务字段，最终约束由 apply 后 materialize 执行。
///
/// @par 返回语义
/// result.flags 只反映负载中实际出现且形状正确的分类，不采用发送者额外声明。
/// snapshot 返回 All 才合法；增量可以是任意非空组合。调用方可据此选择局部刷新
/// 范围，而无需先物化完整 BeatMap。
///
/// @par 非修改保证
/// inspect 不读取或写入 Impl::document、encodingBaseline 与对象缓存。即使负载
/// 无效或后续 apply 失败，调用前的 Codec 状态完全保持不变。
BeatmapDocumentCodec::inspect(std::span<const std::uint8_t> payload)
{
    // 空负载单独分类，便于调用方区分没有数据与损坏头部。
    if ( payload.empty() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto decoded = decodeDocumentPayload(payload);
    if ( !decoded.has_value() || !decoded->is_object() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    const Json&   patch    = decoded.value();
    std::uint32_t version  = 0;
    bool          snapshot = false;
    if ( !readValue(patch, "version", version) ||
         version != DOCUMENT_FORMAT_VERSION ||
         !readValue(patch, "snapshot", snapshot) ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }

    // 结果从 None 开始，只由实际存在且形状合法的分类键累加。
    BeatmapPatchResult result;
    result.isSnapshot = snapshot;
    bool valid        = true;
    // 分类检查只记录存在的合法键，不接受数组与对象类型互换。
    const auto inspectCategory = [&](std::string_view            key,
                                     ::MMM::BeatmapMutationFlags flag,
                                     bool                        mustBeArray) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( (mustBeArray && !iterator->is_array()) ||
             (!mustBeArray && !iterator->is_object()) ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };
    // delta 必须同时具有 added 与 removed 数组，缺一不可。
    const auto inspectDelta = [&](std::string_view            key,
                                  ::MMM::BeatmapMutationFlags flag) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( !iterator->is_object() ) {
            valid = false;
            return;
        }
        const auto added   = iterator->find("added");
        const auto removed = iterator->find("removed");
        if ( added == iterator->end() || !added->is_array() ||
             removed == iterator->end() || !removed->is_array() ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };

    // 快照检查原始分类，增量检查对应 *_delta 和可选完整 metadata。
    if ( snapshot ) {
        inspectCategory("objects", ::MMM::BeatmapMutationFlags::Objects, true);
        inspectCategory(
            "timelines", ::MMM::BeatmapMutationFlags::Timelines, true);
        inspectCategory(
            "audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples, true);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectCategory(
            "annotations", ::MMM::BeatmapMutationFlags::Annotations, true);
    } else {
        inspectDelta("objects_delta", ::MMM::BeatmapMutationFlags::Objects);
        inspectDelta("timelines_delta", ::MMM::BeatmapMutationFlags::Timelines);
        inspectDelta("audio_samples_delta",
                     ::MMM::BeatmapMutationFlags::AudioSamples);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectDelta("annotations_delta",
                     ::MMM::BeatmapMutationFlags::Annotations);
    }

    if ( !valid ||
         (snapshot && result.flags != ::MMM::BeatmapMutationFlags::All) ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }
    if ( result.flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    return result;
}

std::expected<BeatmapPatchResult, BeatmapDocumentError>
/// @brief 验证并应用完整快照或分类增量到内部 JSON 文档。
/// @param payload 完整协作文档二进制负载。
/// @return 实际涉及分类和快照标记，或错误。
///
/// @details
/// apply 先完成外层、版本、快照前置和所有分类结构检查，再修改 document。增量
/// 必须已有快照基线。若本实例还维护发送 encodingBaseline，同一补丁也应用到该
/// 基线，使收发复用场景保持一致。
///
/// Objects 与 Annotations 优先按稳定 ID 替换；格式不适合身份路径时按完整 JSON
/// 值兼容增删。任何接收应用都会清除对象编码缓存，因为缓存指纹无法证明仍与
/// 新文档同步，下一次编码会安全回退并重建。
///
/// @warning 该阶段只保证 JSON 补丁结构；业务字段完整性由 materialize 验证。
///
/// @par 预检阶段
/// 所有补丁分类在任何写入前检查。数组 delta 还要求当前 document 中存在对应
/// 数组；若发送基线启用，则 encodingBaseline 也必须具有同形分类。结构错误时
/// 返回 InvalidDocument，两个文档均不改变。
///
/// @par 提交阶段
/// 完整快照以新 JSON 对象一次替换五分类。增量通过同一 applyIncremental 闭包
/// 先更新接收文档，再在需要时更新发送基线。结构已预检，因此提交阶段不再返回
/// 中途错误；最后统一设置 hasDocument 并使快速缓存失效。
///
/// @par 业务验证边界
/// 为避免每次网络补丁都重复构造大型 BeatMap，apply 不深查 Note 字段或路径。
/// materialize 承担严格业务解码；房间层只有在其成功后才向编辑器发布新模型。
BeatmapDocumentCodec::apply(std::span<const std::uint8_t> payload)
{
    // 与 inspect 保持相同的外层和协议版本入口校验，不能信任预检曾经执行。
    if ( payload.empty() ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }
    auto decoded = decodeDocumentPayload(payload);
    if ( !decoded.has_value() || !decoded->is_object() ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    const Json&   patch    = decoded.value();
    std::uint32_t version  = 0;
    bool          snapshot = false;
    if ( !readValue(patch, "version", version) ||
         version != DOCUMENT_FORMAT_VERSION ||
         !readValue(patch, "snapshot", snapshot) ) {
        return std::unexpected(BeatmapDocumentError::InvalidPayload);
    }
    // 增量没有完整分类落点时不可应用，必须先接收快照。
    if ( !snapshot && !m_impl->hasDocument ) {
        return std::unexpected(BeatmapDocumentError::MissingSnapshot);
    }

    // 记录入口时的发送基线状态；接收快照本身不隐式开启本地增量发送基线。
    const bool         updateEncodingBaseline = m_impl->hasEncodingBaseline;
    BeatmapPatchResult result;
    result.isSnapshot          = snapshot;
    bool       valid           = true;
    const auto inspectCategory = [&](std::string_view            key,
                                     ::MMM::BeatmapMutationFlags flag,
                                     bool                        mustBeArray) {
        const auto iterator = patch.find(key);
        if ( iterator == patch.end() ) return;
        if ( (mustBeArray && !iterator->is_array()) ||
             (!mustBeArray && !iterator->is_object()) ) {
            valid = false;
            return;
        }
        result.flags |= flag;
    };
    // 应用前同时验证接收文档及可选发送基线都具有目标数组。
    const auto inspectArrayDelta = [&](std::string_view            deltaKey,
                                       std::string_view            category,
                                       ::MMM::BeatmapMutationFlags flag) {
        const auto deltaIt = patch.find(deltaKey);
        if ( deltaIt == patch.end() ) return;
        const auto addedIt   = deltaIt->find("added");
        const auto removedIt = deltaIt->find("removed");
        const auto targetIt  = m_impl->document.find(category);
        if ( !deltaIt->is_object() || addedIt == deltaIt->end() ||
             !addedIt->is_array() || removedIt == deltaIt->end() ||
             !removedIt->is_array() || targetIt == m_impl->document.end() ||
             !targetIt->is_array() ) {
            valid = false;
            return;
        }
        if ( m_impl->hasEncodingBaseline ) {
            const auto baselineIt = m_impl->encodingBaseline.find(category);
            if ( baselineIt == m_impl->encodingBaseline.end() ||
                 !baselineIt->is_array() ) {
                valid = false;
                return;
            }
        }
        result.flags |= flag;
    };

    if ( snapshot ) {
        inspectCategory("objects", ::MMM::BeatmapMutationFlags::Objects, true);
        inspectCategory(
            "timelines", ::MMM::BeatmapMutationFlags::Timelines, true);
        inspectCategory(
            "audio_samples", ::MMM::BeatmapMutationFlags::AudioSamples, true);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectCategory(
            "annotations", ::MMM::BeatmapMutationFlags::Annotations, true);
    } else {
        inspectArrayDelta(
            "objects_delta", "objects", ::MMM::BeatmapMutationFlags::Objects);
        inspectArrayDelta("timelines_delta",
                          "timelines",
                          ::MMM::BeatmapMutationFlags::Timelines);
        inspectArrayDelta("audio_samples_delta",
                          "audio_samples",
                          ::MMM::BeatmapMutationFlags::AudioSamples);
        inspectCategory(
            "metadata", ::MMM::BeatmapMutationFlags::Metadata, false);
        inspectArrayDelta("annotations_delta",
                          "annotations",
                          ::MMM::BeatmapMutationFlags::Annotations);
    }

    // 完整预检结束前不触碰任何持久 JSON，失败保持调用前状态。
    if ( !valid ||
         (snapshot && result.flags != ::MMM::BeatmapMutationFlags::All) ) {
        return std::unexpected(BeatmapDocumentError::InvalidDocument);
    }
    if ( result.flags == ::MMM::BeatmapMutationFlags::None ) {
        return std::unexpected(BeatmapDocumentError::EmptyPayload);
    }

    // 所有结构已预检，应用 lambda 只执行身份替换或兼容值增删。
    // 此后 delta 形状已完整验证，应用闭包无需再产生可失败的返回值。
    const auto applyArrayDelta = [&](Json&            document,
                                     std::string_view deltaKey,
                                     std::string_view category) {
        const auto deltaIt = patch.find(deltaKey);
        if ( deltaIt == patch.end() ) return;
        const auto addedIt   = deltaIt->find("added");
        const auto removedIt = deltaIt->find("removed");
        auto&      target    = document[std::string(category)];
        const bool replacesByIdentity =
            category == "objects" || category == "annotations";
        // 身份路径失败时回退旧值算法，保持对历史或不完整 ID 文档的兼容。
        if ( replacesByIdentity &&
             applyIdentityDeltaToArray(target, *deltaIt) ) {
            return;
        }
        const auto erasePrevious = [&](const Json& value) {
            const auto existing =
                std::find(target.begin(), target.end(), value);
            if ( existing != target.end() ) target.erase(existing);
        };
        // 兼容多重集路径严格先删旧值再追加新值，支持“修改=删除+新增”。
        for ( const auto& removed : *removedIt ) {
            erasePrevious(removed);
        }
        for ( const auto& added : *addedIt ) {
            target.push_back(added);
        }
    };

    if ( snapshot ) {
        // 使用全新对象组装快照，避免保留协议未声明的旧分类键。
        Json next             = Json::object();
        next["objects"]       = patch.at("objects");
        next["timelines"]     = patch.at("timelines");
        next["audio_samples"] = patch.at("audio_samples");
        next["metadata"]      = patch.at("metadata");
        next["annotations"]   = patch.at("annotations");
        m_impl->document      = std::move(next);
        if ( updateEncodingBaseline ) {
            m_impl->encodingBaseline = m_impl->document;
        }
    } else {
        // 同一个闭包保证 document 与 encodingBaseline 获得完全相同的补丁语义。
        const auto applyIncremental = [&](Json& document) {
            applyArrayDelta(document, "objects_delta", "objects");
            applyArrayDelta(document, "timelines_delta", "timelines");
            applyArrayDelta(document, "audio_samples_delta", "audio_samples");
            const auto metadata = patch.find("metadata");
            if ( metadata != patch.end() ) document["metadata"] = *metadata;
            applyArrayDelta(document, "annotations_delta", "annotations");
        };
        applyIncremental(m_impl->document);
        if ( updateEncodingBaseline ) {
            applyIncremental(m_impl->encodingBaseline);
        }
    }
    // 接收文档现已提交；快速物件缓存保守失效，下一次发送时按需重建。
    m_impl->hasDocument = true;
    m_impl->objectEncodingCache.clear();
    m_impl->hasObjectEncodingCache   = false;
    m_impl->objectEncodingGeneration = 1;
    return result;
}

/// @brief 把当前完整 JSON 文档解码为独立 BeatMap。
/// @return 全部分类和业务字段有效时返回谱面，否则返回空指针。
///
/// @details
/// 函数先要求五个分类键完整存在，再创建临时 BeatMap 并依次解码。任何分类失败
/// 都丢弃整个临时对象，保持调用者看不到部分文档。最终调用 sync 重建 NoteData
/// 派生索引和排序关系，使返回值可直接进入编辑器模型。
///
/// @note materialize 不改变 Codec 的 JSON 文档，失败后仍可由调用方决定重同步。
///
/// @par 解码顺序
/// Metadata 先建立谱面布局，Objects 重建多态容器与 Polyline 引用，Annotations
/// 验证稳定目标描述，随后解析 Timelines 和 AudioSamples。该顺序不允许分类互相
/// 弥补缺失字段，每一类都必须独立通过完整契约。
std::shared_ptr<::MMM::BeatMap> BeatmapDocumentCodec::materialize() const
{
    if ( !m_impl->hasDocument ) return nullptr;
    const auto objectsIt     = m_impl->document.find("objects");
    const auto timelinesIt   = m_impl->document.find("timelines");
    const auto samplesIt     = m_impl->document.find("audio_samples");
    const auto metadataIt    = m_impl->document.find("metadata");
    const auto annotationsIt = m_impl->document.find("annotations");
    if ( objectsIt == m_impl->document.end() ||
         timelinesIt == m_impl->document.end() ||
         samplesIt == m_impl->document.end() ||
         metadataIt == m_impl->document.end() ||
         annotationsIt == m_impl->document.end() ) {
        return nullptr;
    }

    // 所有解码写入新对象，避免失败污染任何现有业务谱面。
    auto beatmap = std::make_shared<::MMM::BeatMap>();
    if ( !decodeMetadata(*metadataIt, *beatmap) ||
         !decodeObjects(*objectsIt, *beatmap) ||
         !decodeAnnotations(*annotationsIt, *beatmap) ||
         !decodeTimelines(*timelinesIt, *beatmap) ||
         !decodeAudioSamples(*samplesIt, *beatmap) ) {
        return nullptr;
    }
    // 恢复容器间派生关系和编辑器依赖的同步状态。
    beatmap->sync();
    return beatmap;
}

std::unique_ptr<BeatmapDocumentCodec>
/// @brief 克隆当前接收文档，用于合并前后差异比较。
/// @return 有文档时返回只含 document 的独立 Codec，否则返回空指针。
///
/// @details 克隆不复制发送 encodingBaseline 或快速缓存，因为其用途是保存不可变
/// 接收版本，而不是从旧版本继续编码本地增量。JSON 深拷贝确保后续 apply 不会
/// 改变 previous 快照。
BeatmapDocumentCodec::cloneDocument() const
{
    if ( !m_impl->hasDocument ) return nullptr;
    auto clone                 = std::make_unique<BeatmapDocumentCodec>();
    clone->m_impl->document    = m_impl->document;
    clone->m_impl->hasDocument = true;
    return clone;
}

std::optional<std::vector<std::string>>
/// @brief 比较两个文档的玩家物件并返回变化稳定 ID。
/// @param previous 比较基线文档。
/// @return 新增、修改和删除 ID；文档或身份结构不可靠时返回空 optional。
///
/// @details
/// 两侧 objects 先建立唯一 ID 索引。当前数组顺序输出新增和内容变化 ID，随后
/// 按 previous 顺序追加删除 ID。返回空 vector 表示确认没有对象变化；nullopt
/// 则表示无法安全计算 ID 差量，上层必须采用全量刷新。
///
/// @warning 返回结果只描述根 objects；Polyline 子物件变化会改变根对象 JSON，
///          因而以 Polyline 自身稳定 ID 报告。
///
/// @par 结果区分
/// `std::nullopt` 表示调用方无法安全依赖 ID 列表，应刷新全部对象；空 vector
/// 表示比较成功且确认无对象变化。该区别避免身份损坏被误解释为“没有变化”。
BeatmapDocumentCodec::changedObjectIdentitiesComparedTo(
    const BeatmapDocumentCodec& previous) const
{
    if ( !m_impl->hasDocument || !previous.m_impl->hasDocument ) {
        return std::nullopt;
    }
    const auto currentObjects  = m_impl->document.find("objects");
    const auto previousObjects = previous.m_impl->document.find("objects");
    if ( currentObjects == m_impl->document.end() ||
         previousObjects == previous.m_impl->document.end() ||
         !currentObjects->is_array() || !previousObjects->is_array() ) {
        return std::nullopt;
    }

    std::unordered_map<std::string_view, const Json*> currentByIdentity;
    std::unordered_map<std::string_view, const Json*> previousByIdentity;
    currentByIdentity.reserve(currentObjects->size());
    previousByIdentity.reserve(previousObjects->size());
    // 缺失或重复 ID 会让按身份差量产生歧义，因此整体返回 nullopt。
    const auto indexObjects = [](const Json& objects, auto& index) {
        for ( const auto& object : objects ) {
            const auto* identity = collaborationIdentity(object);
            if ( !identity || !index.emplace(*identity, &object).second ) {
                return false;
            }
        }
        return true;
    };
    if ( !indexObjects(*currentObjects, currentByIdentity) ||
         !indexObjects(*previousObjects, previousByIdentity) ) {
        return std::nullopt;
    }

    std::vector<std::string> changed;
    changed.reserve(currentObjects->size() + previousObjects->size());
    // 当前新增和同 ID 内容变化优先按当前文档顺序报告。
    for ( const auto& object : *currentObjects ) {
        const auto& identity = *collaborationIdentity(object);
        const auto  before   = previousByIdentity.find(identity);
        if ( before == previousByIdentity.end() || *before->second != object ) {
            changed.push_back(identity);
        }
    }
    // 基线中已消失的 ID 作为删除项追加，且不会与当前循环重复。
    for ( const auto& object : *previousObjects ) {
        const auto& identity = *collaborationIdentity(object);
        if ( !currentByIdentity.contains(identity) ) {
            changed.push_back(identity);
        }
    }
    return changed;
}

std::expected<ByteBuffer, BeatmapDocumentError>
/// @brief 把当前接收文档重新封装为完整快照负载。
/// @return 已有文档时返回快照负载，否则返回 MissingSnapshot。
///
/// @details 在 JSON 副本上补充 version 与
/// snapshot，不改变内部五分类文档。该入口
/// 用于房主向新加入者转发当前权威状态，不重新 materialize/encode BeatMap。
BeatmapDocumentCodec::encodeCurrentSnapshot() const
{
    // 直接复用内部标准化 JSON，避免从 BeatMap 重编码引入字段或顺序差异。
    if ( !m_impl->hasDocument ) {
        return std::unexpected(BeatmapDocumentError::MissingSnapshot);
    }
    Json snapshot        = m_impl->document;
    snapshot["version"]  = DOCUMENT_FORMAT_VERSION;
    snapshot["snapshot"] = true;
    return encodeDocumentPayload(snapshot);
}

/// @brief 查询是否已经应用至少一个完整快照。
/// @return document 可供 materialize 或增量应用时返回 true。
bool BeatmapDocumentCodec::hasDocument() const
{
    // Codec 由单一逻辑线程访问，简单状态读取无需额外同步。
    return m_impl->hasDocument;
}

/// @brief 清除收发文档、基线和全部派生编码缓存。
///
/// @details reset 恢复与新构造对象等价的状态。下一次 apply 增量会返回
/// MissingSnapshot，下一次非快照 encode 也需要重新建立基线；扫描代次回到一。
void BeatmapDocumentCodec::reset()
{
    // 同时清除接收事实与发送水位线，避免重用对象时跨房间生成增量。
    m_impl->document            = Json::object();
    m_impl->hasDocument         = false;
    m_impl->encodingBaseline    = Json::object();
    m_impl->hasEncodingBaseline = false;
    m_impl->objectEncodingCache.clear();
    m_impl->hasObjectEncodingCache   = false;
    m_impl->objectEncodingGeneration = 1;
}
}  // namespace MMM::Network::Collaboration
