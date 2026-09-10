#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

/// @file LoadMMMMap.hpp
/// @brief 原生 MMM JSON 的容错加载与旧版本迁移实现。
///
/// 加载器接受当前 v3 结构以及历史 v1/v2 字段，并统一迁移到 BeatMap 模型。
/// 旧格式兼容只发生在读取方向；再次保存时由 SaveMMMMap 生成规范 v3 JSON。
///
/// 顶层字段职责：
/// - `format_version` 选择兼容路径，缺失时按 v1 处理；
/// - `metadata` 保存公共谱面元数据和来源格式扩展；
/// - `timing` 保存节奏与滚动效果；
/// - `audio_samples` 保存独立自动采样；
/// - `note` 保存顶层玩家物件和内嵌折线节点；
/// - `annotations` 保存可定位时间或对象的多批注。
///
/// metadata.base 字段映射：
/// - `name` 映射编辑器展示名；
/// - `title` 与 `title_unicode` 映射两种标题；
/// - `artist` 与 `artist_unicode` 映射两种艺术家名；
/// - `version` 映射难度版本；
/// - `author` 映射谱师；
/// - `song_file_hint` 映射项目主音频提示；
/// - 旧 `audio` 字段仅服务 v1 主音频迁移；
/// - `cover` 与 `cover_img` 分别映射主背景和静态封面；
/// - `cover_type` 只接受 IMAGE 或 VIDEO；
/// - `video_starttime`、`bgxoffset`、`bgyoffset` 映射背景参数；
/// - `track_count` 与 `bgm_track_count` 分别定义玩家轨和 BGM 轨；
/// - `bpm` 与 `duration` 映射偏好 BPM 和谱面时长。
///
/// metadata.extra 使用来源命名空间数组：
/// - `osu` 属性进入 MapMetadataType::OSU；
/// - `malody` 属性进入 MapMetadataType::MALODY；
/// - `rm` 属性进入 MapMetadataType::RM；
/// - 未知来源和非字符串属性被忽略；
/// - 扩展属性不覆盖已经解析的公共字段。
///
/// timing 条目字段映射：
/// - `timestamp` 为效果时间；
/// - `bpm` 为参考 BPM；
/// - `beat_length` 为拍长；
/// - `effect` 通过稳定字符串转换为 TimingEffect；
/// - `param` 为效果参数；
/// - `extra` 仅接受 osu! 与 Malody 来源字符串属性。
///
/// audio_samples 条目字段映射：
/// - `collaboration_id` 是可选稳定身份；
/// - `timestamp` 是时间线触发位置；
/// - `offset_ms` 是 v3 资源内偏移；
/// - 旧 `offset` 作为 offset_ms 缺失时的兼容回退；
/// - `track` 是绝对 BGM 轨索引；
/// - `audio_ref` 是必需的音频资源标识；
/// - `volume` 是线性音量；
/// - `extra` 接受 Malody 与 MMM 自有字符串属性。
///
/// 自动采样轨道必须位于玩家轨之后。若输入把采样放在玩家轨区域，加载器会：
/// - 在 MMM 扩展元数据中保存 original_track；
/// - 把对象迁移到首条 BGM 轨；
/// - 记录 AUDIO_SAMPLE_TRACK_RELOCATED 诊断；
/// - 根据最终最大轨道补足 bgm_track_count；
/// - 不改变其他自动采样的资源、时间或偏移。
///
/// v1 没有显式 audio_samples。若旧 `metadata.base.audio` 非空且没有显式采样，
/// 加载器会在首条 BGM 轨构造零时间、零偏移、原音量的自动采样。若同目录仍有
/// 原始 Malody 文件，则额外记录诊断，提示旧 MMM 可能已丢失 SOUND 信息。
///
/// 玩家物件公共字段：
/// - `timestamp` 为起始时间；
/// - `track` 为玩家轨；
/// - `type` 为 note、hold、flick 或 polyline；
/// - `collaboration_id` 为稳定对象身份；
/// - `annotation` 为有长度上限的内联短批注；
/// - `sample` 为 v3 玩家命中采样对象；
/// - 旧 `bound_sound`、`bound_volume` 仅用于读取兼容。
///
/// 派生物件字段：
/// - Hold 使用 `duration`；
/// - Flick 使用 `dtrack`；
/// - Polyline 使用 `sub_notes`；
/// - 折线子节点重复使用基础物件类型与派生字段；
/// - 子节点不允许继续嵌套 Polyline。
///
/// Polyline 恢复分成三步：
/// - 预扫描所有 sub_notes 的时间戳与轨道键，识别旧文件重复顶层节点；
/// - 先把 JSON 节点读入 TempSub 值数组，避免拥有容器扩容造成引用失效；
/// - 根据批注或协作身份决定保留原结构，或执行零段清理与同类合并；
/// - 确定最终结构后才向 NoteData 各拥有容器插入对象并建立折线引用。
///
/// 未被批注依赖的折线会执行规范化：
/// - 零长度 Hold 被过滤；
/// - 零位移 Flick 被过滤；
/// - 相邻 Hold 合并持续时间；
/// - 相邻 Flick 合并位移；
/// - 合并产生的新零段在下一轮过滤；
/// - 无有效段时退化为普通 Note；
/// - 仅一段时退化为对应基础物件；
/// - 多段时保留 Polyline。
///
/// 存在批注或协作身份时不清洗结构，因为独立 annotations 可能通过 target_id
/// 指向折线或具体节点。合并、删除或降级这些对象会留下无法解析的批注目标。
/// 此时即使节点是零长度 Hold 或零位移 Flick，也必须按原始结构恢复。
///
/// annotations 条目约束：
/// - 总量不能超过 MAX_BEATMAP_ANNOTATION_COUNT；
/// - id 必须非空、长度合法且在文件内唯一；
/// - target_kind 只接受 timestamp、player_object、audio_sample；
/// - 对象型批注必须提供 target_id；
/// - target_id、author 与 content 分别执行长度限制；
/// - content 不能为空；
/// - 非法条目被跳过，不阻断其他谱面内容加载。
///
/// JSON 标量读取坚持以下规则：
/// - 对象类型不符时返回调用点提供的默认值；
/// - 字段缺失或类型不符时返回默认值；
/// - 浮点值必须有限；
/// - int 与 int64 转换前检查表达范围；
/// - uint32 轨道拒绝负值；
/// - 字符串只接受 JSON string，不隐式转换数值或布尔值。
///
/// 加载器维持以下生命周期约束：
/// - JSON 树在全部字段解析完成前保持存活；
/// - NoteData 类型容器拥有所有基础物件；
/// - Polyline 只保存对这些拥有对象的引用；
/// - 临时折线值不跨容器插入阶段保存引用；
/// - 空壳 Polyline 在降级后立即从拥有容器移除；
/// - BeatMap::sync 只在全部对象、采样和批注加载结束后调用。
///
/// 容错不等于猜测。未知对象、未知来源命名空间和越界身份直接忽略；已知字段
/// 使用局部默认值恢复。加载诊断只记录明确可识别的迁移，不为任意损坏 JSON
/// 推断作者意图。
///
/// 非法输入处理速查：
/// - 根节点不是对象时返回空谱面；
/// - JSON 语法错误时返回空谱面；
/// - metadata 不是对象时跳过全部元数据；
/// - base 字段类型错误时逐字段使用默认值；
/// - timing 不是数组时视为没有 Timing；
/// - timing 中非对象元素直接跳过；
/// - audio_samples 不是数组时视为没有显式采样；
/// - 自动采样缺少字符串 audio_ref 时跳过该项；
/// - 自动采样负轨道回退到首条 BGM 轨；
/// - 自动采样玩家区轨道会迁移并产生诊断；
/// - note 不是数组时视为没有玩家物件；
/// - note 中非对象元素直接跳过；
/// - 未知物件 type 按普通 Note 兼容；
/// - sub_notes 不是数组时按空折线处理；
/// - sub_notes 中非对象元素直接跳过；
/// - 未知子节点 type 按普通 Note 兼容；
/// - 超限物件内联批注不写入模型；
/// - 超限协作身份清空；
/// - annotations 不是数组时视为没有多批注；
/// - annotations 中非对象会终止当前批注读取；
/// - 超过批注总量上限时停止继续分配；
/// - 未知 target_kind 跳过对应批注；
/// - 重复批注 ID 跳过后出现的记录；
/// - 对象型批注缺少 target_id 时跳过；
/// - 空正文或超限正文时跳过批注；
/// - 伴随原始 .mc 不存在时不产生恢复诊断；
/// - 文件系统探测错误不影响已加载模型；
/// - 任意可选 extra 类型错误只丢失该扩展层；
/// - 最终 sync 只处理已经通过局部验证的数据。
///
/// 这些容错规则用于保证可打开性，不承诺损坏字段能够无损恢复。调用方应读取
/// m_loadDiagnostics 展示已发生的明确迁移，并在再次保存前让用户确认结果。

/// @brief 从 MMM JSON 对象读取字符串字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的字符串或默认值。
/// @note 不接受数字、布尔值或 null 的隐式文本转换。
inline std::string readMMMString(const json& object, const char* key,
                                 std::string fallback = {})
{
    // 先验证容器和字段类型，避免 nlohmann::json 转换异常进入加载路径。
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_string() ) return fallback;
    return it->get_ref<const std::string&>();
}

/// @brief 从 MMM JSON 对象读取浮点字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的有限浮点数或默认值。
/// @note NaN 与正负无穷不进入谱面模型。
inline double readMMMDouble(const json& object, const char* key,
                            double fallback)
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return fallback;
    // JSON 数值可能来自浮点表示，读取后仍需阻止非有限结果传播。
    const double value = it->get<double>();
    return std::isfinite(value) ? value : fallback;
}

/// @brief 从 MMM JSON 对象读取整数字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的整数或默认值。
/// @note 数值按 C++ 转换规则截断小数，但越界时使用回退值。
inline int readMMMInt(const json& object, const char* key, int fallback)
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return fallback;

    // 先以 double 观察范围，避免超界值直接转换为 int 产生未定义结果。
    const double value = it->get<double>();
    if ( !std::isfinite(value) ||
         value < static_cast<double>(std::numeric_limits<int>::min()) ||
         value > static_cast<double>(std::numeric_limits<int>::max()) ) {
        return fallback;
    }
    return static_cast<int>(value);
}

/// @brief 从 MMM JSON 对象读取 64 位整数字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的 64 位整数或默认值。
/// @note 使用最近整数恢复历史 JSON 中以浮点形式保存的毫秒偏移。
inline std::int64_t readMMMInt64(const json& object, const char* key,
                                 std::int64_t fallback)
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return fallback;

    // long double 只用于范围比较；磁盘值仍由 JSON number 提供。
    const long double value = static_cast<long double>(it->get<double>());
    if ( !std::isfinite(value) ||
         value < static_cast<long double>(
                     std::numeric_limits<std::int64_t>::min()) ||
         value > static_cast<long double>(
                     std::numeric_limits<std::int64_t>::max()) ) {
        return fallback;
    }
    return static_cast<std::int64_t>(std::llround(value));
}

/// @brief 从 MMM JSON 对象读取非负轨道索引。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的非负整数或默认值。
/// @note 负轨道不会发生无符号回绕，而是明确回退。
inline uint32_t readMMMU32(const json& object, const char* key,
                           uint32_t fallback)
{
    // 复用有符号范围检查后再验证非负约束。
    const int parsed = readMMMInt(object, key, static_cast<int>(fallback));
    if ( parsed < 0 ) return fallback;
    return static_cast<uint32_t>(parsed);
}

/// @brief 从原生 MMM JSON 加载谱面并迁移历史版本。
/// @param path 谱面文件路径。
/// @return 加载完成的谱面；文件或根 JSON 无效时返回空对象。
/// @details 各顶层数组相互独立解析，单条非法记录只影响自身。最后统一调用
/// sync 建立跨类型物件视图、排序和派生状态。
inline BeatMap loadMMMMap(const std::filesystem::path& path)
{
    // BeatMap 默认值也是各字段缺失时的最终安全基线。
    BeatMap       beatMap;
    std::ifstream file(path);
    if ( !file.is_open() ) {
        XERROR("Failed to open mmm map file: {}", Config::pathToUtf8(path));
        return beatMap;
    }

    // allow_exceptions=false 让语法错误返回 discarded，不依赖异常控制流。
    json root = json::parse(file, nullptr, false);
    if ( root.is_discarded() || !root.is_object() || file.bad() ) {
        XERROR("Failed to parse mmm map JSON: {}", Config::pathToUtf8(path));
        return beatMap;
    }

    // 只有成功得到对象根后才记录来源路径，解析失败对象保持默认状态。
    beatMap.m_baseMapMetadata.map_path = path;
    const int formatVersion            = readMMMInt(root, "format_version", 1);

    /// @brief 从 MMM extra 对象中复制字符串属性。
    /// @details 非对象或非字符串值被忽略，来源私有属性不做类型猜测。
    auto copyStringMetadataProperties = [](auto& props, const json& propsJson) {
        if ( !propsJson.is_object() ) return;
        for ( auto propIt = propsJson.begin(); propIt != propsJson.end();
              ++propIt ) {
            // 保持键和值原文，使对应来源保存器能够自行解释。
            if ( propIt.value().is_string() ) {
                props[propIt.key()] =
                    propIt.value().template get_ref<const std::string&>();
            }
        }
    };

    /// @brief 加载玩家物件的来源格式扩展元数据。
    /// @note 仅识别 osu、malody 与 mmm 三个稳定命名空间。
    auto loadNoteMetadata = [copyStringMetadataProperties](Note&       note,
                                                           const json& nJson) {
        auto extraIt = nJson.find("extra");
        if ( extraIt == nJson.end() || !extraIt->is_array() ) return;
        // 数组中每个对象可包含来源键；损坏元素不影响其余来源。
        for ( const auto& extraItem : *extraIt ) {
            if ( !extraItem.is_object() ) continue;
            for ( auto it = extraItem.begin(); it != extraItem.end(); ++it ) {
                NoteMetadataType mtype;
                if ( it.key() == "osu" )
                    mtype = NoteMetadataType::OSU;
                else if ( it.key() == "malody" )
                    mtype = NoteMetadataType::MALODY;
                else if ( it.key() == "mmm" )
                    mtype = NoteMetadataType::MMM;
                else
                    continue;
                auto& props = note.m_metadata.note_properties[mtype];
                copyStringMetadataProperties(props, it.value());
            }
        }
    };

    /// @brief 从 MMM 玩家物件对象加载有界编辑器注释。
    /// @note 超过字节上限的文本整体忽略，不截断 UTF-8 字节序列。
    auto loadNoteAnnotation = [](Note& note, const json& noteJson) {
        auto annotationIt = noteJson.find("annotation");
        if ( annotationIt == noteJson.end() || !annotationIt->is_string() ) {
            return;
        }
        const auto& annotation = annotationIt->get_ref<const std::string&>();
        // 按序列化字节数限制，避免截断多字节字符形成非法 UTF-8。
        if ( annotation.size() <= MAX_NOTE_ANNOTATION_BYTES ) {
            note.m_annotation = annotation;
        }
    };

    /// @brief 从 MMM 物件对象加载可选稳定协作标识。
    /// @note 空身份合法；超限身份清空，不能作为批注目标参与解析。
    auto loadNoteCollaborationIdentity = [](Note& note, const json& noteJson) {
        const std::string identity =
            readMMMString(noteJson, "collaboration_id");
        if ( identity.size() <= MAX_BEATMAP_ANNOTATION_ID_BYTES ) {
            note.m_collaborationId = identity;
        }
    };

    /// @brief 从 MMM 玩家物件对象读取可选命中采样绑定。
    /// @return v3 sample 优先，缺失时尝试 v1/v2 平铺字段。
    auto readNoteSampleBinding =
        [](const json& noteJson) -> std::optional<AudioSampleBinding> {
        // 规范对象存在且 audio_ref 非空时，不再读取旧平铺键。
        auto sampleIt = noteJson.find("sample");
        if ( sampleIt != noteJson.end() && sampleIt->is_object() ) {
            const std::string audioResourceId =
                readMMMString(*sampleIt, "audio_ref");
            if ( !audioResourceId.empty() ) {
                return AudioSampleBinding{ audioResourceId,
                                           static_cast<float>(readMMMDouble(
                                               *sampleIt, "volume", 1.0)) };
            }
        }

        // 旧 bound_sound 只作为兼容后备，保存时不会再次生成。
        const std::string audioResourceId =
            readMMMString(noteJson, "bound_sound");
        if ( audioResourceId.empty() ) return std::nullopt;
        return AudioSampleBinding{ audioResourceId,
                                   static_cast<float>(readMMMDouble(
                                       noteJson, "bound_volume", 1.0)) };
    };

    /// @brief 从 MMM 物件对象加载可选命中采样绑定。
    /// @note 无有效绑定时显式清空，防止复用对象残留旧状态。
    auto loadNoteSampleBinding = [readNoteSampleBinding](Note&       note,
                                                         const json& noteJson) {
        const auto binding = readNoteSampleBinding(noteJson);
        if ( !binding ) {
            note.clearSampleBinding();
            return;
        }
        note.setSampleBinding(*binding);
    };

    /// @brief 将可选命中采样绑定应用到玩家物件。
    /// @note 用于折线降级与节点构造时统一处理存在和清空分支。
    auto applyNoteSampleBinding =
        [](Note& note, const std::optional<AudioSampleBinding>& binding) {
            if ( binding )
                note.setSampleBinding(*binding);
            else
                note.clearSampleBinding();
        };

    /// @brief 从 MMM extra 数组加载采样对象附加元数据。
    /// @note 采样扩展只识别 malody 与 mmm，未知来源不创建空属性表。
    auto loadSampleMetadata = [copyStringMetadataProperties](
                                  AudioSampleEvent& sample,
                                  const json&       sampleJson) {
        auto extraIt = sampleJson.find("extra");
        if ( extraIt == sampleJson.end() || !extraIt->is_array() ) return;
        // 与谱面和物件扩展使用相同的单键对象数组约定。
        for ( const auto& extraItem : *extraIt ) {
            if ( !extraItem.is_object() ) continue;
            for ( auto it = extraItem.begin(); it != extraItem.end(); ++it ) {
                SampleMetadataType type;
                if ( it.key() == "malody" )
                    type = SampleMetadataType::MALODY;
                else if ( it.key() == "mmm" )
                    type = SampleMetadataType::MMM;
                else
                    continue;
                auto& props = sample.m_metadata.sample_properties[type];
                copyStringMetadataProperties(props, it.value());
            }
        }
    };

    // 第一阶段解析元数据。整个 metadata 缺失时保留 BeatMap 默认值。
    auto metadataIt = root.find("metadata");
    if ( metadataIt != root.end() && metadataIt->is_object() ) {
        const auto& metadata = *metadataIt;
        // base 不是对象时，各 readMMM 辅助函数统一返回局部默认值。
        if ( metadata.contains("base") ) {
            const auto& base                = metadata["base"];
            beatMap.m_baseMapMetadata.name  = readMMMString(base, "name");
            beatMap.m_baseMapMetadata.title = readMMMString(base, "title");
            beatMap.m_baseMapMetadata.title_unicode =
                readMMMString(base, "title_unicode");
            beatMap.m_baseMapMetadata.artist = readMMMString(base, "artist");
            beatMap.m_baseMapMetadata.artist_unicode =
                readMMMString(base, "artist_unicode");
            beatMap.m_baseMapMetadata.version = readMMMString(base, "version");
            beatMap.m_baseMapMetadata.author  = readMMMString(base, "author");
            // v1 使用 audio 作为主音频；新版本使用 song_file_hint 与自动采样。
            const std::filesystem::path legacyAudioPath =
                Config::utf8ToPath(readMMMString(base, "audio"));
            beatMap.m_baseMapMetadata.song_file_hint =
                Config::utf8ToPath(readMMMString(base, "song_file_hint"));
            // 新提示缺失时仍以旧字段补足项目资源选择信息。
            if ( beatMap.m_baseMapMetadata.song_file_hint.empty() ) {
                beatMap.m_baseMapMetadata.song_file_hint = legacyAudioPath;
            }
            // 仅旧版本把 audio 写回
            // main_audio_path，新版本避免产生双重音频来源。
            if ( formatVersion < 2 ) {
                beatMap.m_baseMapMetadata.main_audio_path = legacyAudioPath;
            } else {
                beatMap.m_baseMapMetadata.main_audio_path.clear();
            }
            beatMap.m_baseMapMetadata.main_cover_path =
                Config::utf8ToPath(readMMMString(base, "cover"));
            beatMap.m_baseMapMetadata.cover_path =
                Config::utf8ToPath(readMMMString(base, "cover_img"));
            const int coverType = readMMMInt(
                base,
                "cover_type",
                static_cast<int>(beatMap.m_baseMapMetadata.cover_type));
            // cover_type 是外部输入枚举，只接受模型明确定义的两个值。
            if ( coverType == static_cast<int>(CoverType::IMAGE) ||
                 coverType == static_cast<int>(CoverType::VIDEO) ) {
                beatMap.m_baseMapMetadata.cover_type =
                    static_cast<CoverType>(coverType);
            }
            beatMap.m_baseMapMetadata.video_starttime =
                readMMMInt(base,
                           "video_starttime",
                           beatMap.m_baseMapMetadata.video_starttime);
            beatMap.m_baseMapMetadata.bgxoffset = readMMMInt(
                base, "bgxoffset", beatMap.m_baseMapMetadata.bgxoffset);
            beatMap.m_baseMapMetadata.bgyoffset = readMMMInt(
                base, "bgyoffset", beatMap.m_baseMapMetadata.bgyoffset);
            beatMap.m_baseMapMetadata.track_count =
                readMMMU32(base, "track_count", 4);
            beatMap.m_baseMapMetadata.bgm_track_count =
                std::max(0, readMMMInt(base, "bgm_track_count", 0));
            beatMap.m_baseMapMetadata.preference_bpm =
                readMMMDouble(base, "bpm", 120.0);
            beatMap.m_baseMapMetadata.map_length =
                readMMMDouble(base, "duration", 0.0);
        }

        // 来源扩展与 base 分开解析，单个命名空间异常不影响公共元数据。
        auto metadataExtraIt = metadata.find("extra");
        if ( metadataExtraIt != metadata.end() &&
             metadataExtraIt->is_array() ) {
            for ( const auto& extraItem : *metadataExtraIt ) {
                if ( !extraItem.is_object() ) continue;
                for ( auto it = extraItem.begin(); it != extraItem.end();
                      ++it ) {
                    MapMetadataType type;
                    // 磁盘名称到内部枚举的映射集中在此处。
                    if ( it.key() == "osu" )
                        type = MapMetadataType::OSU;
                    else if ( it.key() == "malody" )
                        type = MapMetadataType::MALODY;
                    else if ( it.key() == "rm" )
                        type = MapMetadataType::RM;
                    else
                        continue;

                    auto& props = beatMap.m_metadata.map_properties[type];
                    copyStringMetadataProperties(props, it.value());
                }
            }
        }
    }

    // 第二阶段解析 Timing；非对象元素直接跳过，不占用模型条目。
    auto timingIt = root.find("timing");
    if ( timingIt != root.end() && timingIt->is_array() ) {
        for ( const auto& tJson : *timingIt ) {
            if ( !tJson.is_object() ) continue;
            // 每项从安全默认值开始构造，避免局部字段错误留下未初始化状态。
            Timing t;
            t.m_timestamp   = readMMMDouble(tJson, "timestamp", 0.0);
            t.m_bpm         = readMMMDouble(tJson, "bpm", 120.0);
            t.m_beat_length = readMMMDouble(tJson, "beat_length", 500.0);
            // 字符串转换函数负责未知效果名称的稳定回退语义。
            t.m_timingEffect =
                timingEffectFromString(readMMMString(tJson, "effect", "bpm"));
            t.m_timingEffectParameter = readMMMDouble(tJson, "param", 0.0);

            // 私有来源属性在公共 Timing 字段完成后附加。
            auto timingExtraIt = tJson.find("extra");
            if ( timingExtraIt != tJson.end() && timingExtraIt->is_array() ) {
                for ( const auto& extraItem : *timingExtraIt ) {
                    if ( !extraItem.is_object() ) continue;
                    for ( auto it = extraItem.begin(); it != extraItem.end();
                          ++it ) {
                        TimingMetadataType type;
                        if ( it.key() == "osu" )
                            type = TimingMetadataType::OSU;
                        else if ( it.key() == "malody" )
                            type = TimingMetadataType::MALODY;
                        else
                            continue;

                        auto& props = t.m_metadata.timing_properties[type];
                        copyStringMetadataProperties(props, it.value());
                    }
                }
            }
            // 保留文件数组次序，最终规范排序由 sync 统一完成。
            beatMap.m_timings.push_back(t);
        }
    }

    // 第三阶段解析自动采样；缺少字符串 audio_ref 的项没有可播放身份，跳过。
    auto sampleIt = root.find("audio_samples");
    if ( sampleIt != root.end() && sampleIt->is_array() ) {
        for ( const auto& sampleJson : *sampleIt ) {
            if ( !sampleJson.is_object() ) continue;
            const auto audioReferenceIt = sampleJson.find("audio_ref");
            if ( audioReferenceIt == sampleJson.end() ||
                 !audioReferenceIt->is_string() ) {
                continue;
            }
            const std::string audioResourceId =
                audioReferenceIt->get_ref<const std::string&>();

            // 必需字段验证通过后才插入拥有容器，避免留下半初始化占位项。
            AudioSampleEvent& sample = beatMap.m_audioSamples.emplace_back();
            const std::string sampleIdentity =
                readMMMString(sampleJson, "collaboration_id");
            if ( sampleIdentity.size() <= MAX_BEATMAP_ANNOTATION_ID_BYTES ) {
                sample.m_collaborationId = sampleIdentity;
            }
            sample.m_timestamp = readMMMDouble(sampleJson, "timestamp", 0.0);
            // v3 offset_ms 优先，旧 offset 仅作为字段缺失时的兼容值。
            sample.m_offsetMs = readMMMInt64(
                sampleJson, "offset_ms", readMMMInt64(sampleJson, "offset", 0));
            sample.m_track =
                readMMMU32(sampleJson,
                           "track",
                           static_cast<uint32_t>(std::max(
                               0, beatMap.m_baseMapMetadata.track_count)));
            sample.m_audioResourceId = audioResourceId;
            sample.m_volume =
                static_cast<float>(readMMMDouble(sampleJson, "volume", 1.0));
            loadSampleMetadata(sample, sampleJson);

            // 自动采样不得占据玩家轨；非法轨道统一迁移并保留原始值供诊断。
            const int playableTrackCount =
                std::max(0, beatMap.m_baseMapMetadata.track_count);
            if ( sample.m_track < static_cast<uint32_t>(playableTrackCount) ) {
                const uint32_t originalTrack = sample.m_track;
                // original_track 放入 MMM 私有扩展，后续工具可展示或人工修复。
                sample.m_metadata.sample_properties[SampleMetadataType::MMM]
                                                   ["original_track"] =
                    std::to_string(originalTrack);
                sample.m_track = static_cast<uint32_t>(playableTrackCount);
                beatMap.m_loadDiagnostics.push_back(
                    { .m_code = BeatmapLoadDiagnosticCode::
                          AUDIO_SAMPLE_TRACK_RELOCATED,
                      .m_severity = BeatmapLoadDiagnosticSeverity::
                          BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
                      .m_message =
                          fmt::format("MMM 自动采样 '{}' 的轨道 {} 不属于 BGM "
                                      "区，已迁移到首条 BGM 轨 {}",
                                      sample.m_audioResourceId,
                                      originalTrack,
                                      playableTrackCount),
                      .m_relatedPath = path });
            }
            // 根据实际采样最大轨道补足声明，保证所有自动采样都落在 BGM 区间。
            if ( sample.m_track >= static_cast<uint32_t>(playableTrackCount) ) {
                const std::uint64_t requiredBgmTrackCount64 =
                    static_cast<std::uint64_t>(sample.m_track) -
                    static_cast<std::uint64_t>(playableTrackCount) + 1;
                const int requiredBgmTrackCount =
                    static_cast<int>(std::min<std::uint64_t>(
                        requiredBgmTrackCount64,
                        static_cast<std::uint64_t>(
                            std::numeric_limits<int>::max())));
                beatMap.m_baseMapMetadata.bgm_track_count =
                    std::max(beatMap.m_baseMapMetadata.bgm_track_count,
                             requiredBgmTrackCount);
            }
        }
    }

    // v1 只在没有显式数组时迁移旧主音频，避免新旧字段并存时重复播放。
    // 旧语义等价于从曲首以原音量播放一次，并位于首条 BGM 轨。
    if ( formatVersion < 2 && beatMap.m_audioSamples.empty() &&
         !beatMap.m_baseMapMetadata.song_file_hint.empty() ) {
        AudioSampleEvent& sample = beatMap.m_audioSamples.emplace_back();
        sample.m_track           = static_cast<uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        sample.m_audioResourceId =
            Config::pathToUtf8(beatMap.m_baseMapMetadata.song_file_hint);
        beatMap.m_baseMapMetadata.bgm_track_count =
            std::max(1, beatMap.m_baseMapMetadata.bgm_track_count);

        // 同名 .mc 只生成恢复建议，不自动替换用户明确打开的 MMM 文件。
        std::filesystem::path originalMalodyPath = path;
        originalMalodyPath.replace_extension(".mc");
        std::error_code filesystemError;
        // 文件系统探测使用 error_code，诊断路径不能令谱面加载抛出异常。
        if ( std::filesystem::is_regular_file(originalMalodyPath,
                                              filesystemError) &&
             !filesystemError ) {
            beatMap.m_loadDiagnostics.push_back(
                { .m_code = BeatmapLoadDiagnosticCode::
                      LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,
                  .m_severity = BeatmapLoadDiagnosticSeverity::
                      BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
                  .m_message     = "旧版 MMM 已丢失部分 Malody SOUND "
                                   "信息，建议重新导入同目录的原始 .mc 文件",
                  .m_relatedPath = std::move(originalMalodyPath) });
        }
    }

    // 第四阶段解析玩家物件及折线结构。
    auto noteIt = root.find("note");
    if ( noteIt != root.end() && noteIt->is_array() ) {
        // 旧保存器可能把折线节点同时写进 sub_notes 和顶层 note。预扫描节点键，
        // 后续基础物件分支用时间戳与轨道识别重复项。
        std::set<std::pair<double, uint32_t>> subNoteKeys;
        // 第一遍只收集身份键，不构造模型或保存 JSON 引用。
        for ( const auto& nJson : *noteIt ) {
            if ( !nJson.is_object() ) continue;
            if ( readMMMString(nJson, "type", "note") == "polyline" ) {
                auto subNotesIt = nJson.find("sub_notes");
                if ( subNotesIt != nJson.end() && subNotesIt->is_array() ) {
                    for ( const auto& snJson : *subNotesIt ) {
                        if ( !snJson.is_object() ) continue;
                        // set 自动去重，同位置节点共享同一旧数据防御键。
                        subNoteKeys.insert(
                            { readMMMDouble(snJson, "timestamp", 0.0),
                              readMMMU32(snJson, "track", 0) });
                    }
                }
            }
        }

        // 第二遍才构造拥有对象；未知 type 按普通 Note 兼容处理。
        for ( const auto& nJson : *noteIt ) {
            if ( !nJson.is_object() ) continue;
            std::string type = readMMMString(nJson, "type", "note");
            if ( type == "polyline" ) {
                // 先建立父级空壳加载公共属性，规范化后可能降级并移除。
                Polyline& poly   = beatMap.m_noteData.polylines.emplace_back();
                poly.m_type      = NoteType::POLYLINE;
                poly.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                poly.m_track     = readMMMU32(nJson, "track", 0);
                loadNoteSampleBinding(poly, nJson);

                // 父级扩展、内联批注与协作身份必须在结构保护判断前加载。
                loadNoteMetadata(poly, nJson);
                loadNoteAnnotation(poly, nJson);
                loadNoteCollaborationIdentity(poly, nJson);

                /// @brief 写入类型拥有容器之前的折线节点值对象。
                /// @details 值语义允许安全清洗，不受 vector 扩容影响。
                struct TempSub {
                    /// @brief 节点基础类型。
                    NoteType type;
                    /// @brief 节点起始时间。
                    double timestamp;
                    /// @brief Hold 时长，其他类型为零。
                    double duration;
                    /// @brief 玩家轨道。
                    int track;
                    /// @brief Flick 位移，其他类型为零。
                    int dtrack;
                    /// @brief 子物件的可选命中采样绑定。
                    std::optional<AudioSampleBinding> sampleBinding;
                    /// @brief 子物件的独立编辑器注释。
                    std::string annotation;
                    /// @brief 子物件的稳定协作标识。
                    std::string collaborationId;
                };
                // 父级内联批注已足以禁止任何结构清洗或降级。
                std::vector<TempSub> tempSubs;
                bool preserveAnnotatedStructure = !poly.m_annotation.empty();

                // sub_notes 缺失时保持空数组，再依据父级保护状态决定退化。
                auto subNotesIt = nJson.find("sub_notes");
                if ( subNotesIt != nJson.end() && subNotesIt->is_array() ) {
                    // 先观察全部节点批注，不能在确认结构无引用前删除零段。
                    preserveAnnotatedStructure =
                        preserveAnnotatedStructure ||
                        std::any_of(
                            subNotesIt->begin(),
                            subNotesIt->end(),
                            [](const json& subNoteJson) {
                                const auto annotationIt =
                                    subNoteJson.find("annotation");
                                return annotationIt != subNoteJson.end() &&
                                       annotationIt->is_string() &&
                                       !annotationIt
                                            ->get_ref<const std::string&>()
                                            .empty();
                            });
                    // 非对象元素直接跳过，不为损坏元素构造占位节点。
                    for ( const auto& snJson : *subNotesIt ) {
                        if ( !snJson.is_object() ) continue;
                        std::string stype =
                            readMMMString(snJson, "type", "note");
                        // 从确定默认值开始，再由具体类型补充 duration 或
                        // dtrack。
                        TempSub sn;
                        sn.timestamp = readMMMDouble(snJson, "timestamp", 0.0);
                        sn.track     = readMMMInt(snJson, "track", 0);
                        sn.duration  = 0.0;
                        sn.dtrack    = 0;
                        sn.sampleBinding = readNoteSampleBinding(snJson);
                        // 超限批注不截断，避免切断 UTF-8 多字节字符。
                        const std::string annotation =
                            readMMMString(snJson, "annotation");
                        if ( annotation.size() <= MAX_NOTE_ANNOTATION_BYTES ) {
                            sn.annotation = annotation;
                        }
                        sn.collaborationId =
                            readMMMString(snJson, "collaboration_id");
                        // 超限身份整体清空，不能作为批注可引用的稳定 ID。
                        if ( sn.collaborationId.size() >
                             MAX_BEATMAP_ANNOTATION_ID_BYTES ) {
                            sn.collaborationId.clear();
                        }
                        // 任一有效节点身份都保护整条折线的原始节点边界。
                        preserveAnnotatedStructure =
                            preserveAnnotatedStructure ||
                            !sn.collaborationId.empty();
                        if ( stype == "hold" ) {
                            // 未受保护的亚阈值 Hold 可在进入临时数组前跳过。
                            sn.duration =
                                readMMMDouble(snJson, "duration", 0.0);
                            if ( sn.duration < 1e-4 &&
                                 !preserveAnnotatedStructure ) {
                                continue;
                            }
                            sn.type = NoteType::HOLD;
                        } else if ( stype == "flick" ) {
                            sn.type   = NoteType::FLICK;
                            sn.dtrack = readMMMInt(snJson, "dtrack", 0);
                        } else {
                            sn.type = NoteType::NOTE;
                        }
                        // 普通 Note 也保留于受保护结构，保证其 ID
                        // 与批注仍可解析。
                        tempSubs.push_back(sn);
                    }

                    // 无结构引用时才清洗，并反复执行直到一整轮没有变化。
                    bool changed = !preserveAnnotatedStructure;
                    while ( changed ) {
                        changed = false;

                        // 第一阶段删除零长度 Hold 与零位移 Flick。
                        auto it =
                            std::remove_if(tempSubs.begin(),
                                           tempSubs.end(),
                                           [](const auto& s) {
                                               if ( s.type == NoteType::HOLD )
                                                   return s.duration < 1e-4;
                                               if ( s.type == NoteType::FLICK )
                                                   return s.dtrack == 0;
                                               return false;
                                           });
                        // 删除会改变邻接关系，因此标记下一轮继续处理。
                        if ( it != tempSubs.end() ) {
                            tempSubs.erase(it, tempSubs.end());
                            changed = true;
                        }

                        // 第二阶段合并相邻同类段，不跨越不同类型节点。
                        if ( tempSubs.size() > 1 ) {
                            for ( size_t i = 0; i < tempSubs.size() - 1; ) {
                                auto& curr = tempSubs[i];
                                auto& next = tempSubs[i + 1];
                                if ( curr.type == next.type ) {
                                    if ( curr.type == NoteType::HOLD ) {
                                        // 时长相加；前段无采样时继承后段绑定。
                                        curr.duration += next.duration;
                                        if ( !curr.sampleBinding ) {
                                            curr.sampleBinding =
                                                next.sampleBinding;
                                        }
                                        tempSubs.erase(tempSubs.begin() + i +
                                                       1);
                                        changed = true;
                                        continue;
                                    } else if ( curr.type == NoteType::FLICK ) {
                                        // 位移相加；抵消为零后由下一轮过滤。
                                        curr.dtrack += next.dtrack;
                                        if ( !curr.sampleBinding ) {
                                            curr.sampleBinding =
                                                next.sampleBinding;
                                        }
                                        tempSubs.erase(tempSubs.begin() + i +
                                                       1);
                                        changed = true;
                                        continue;
                                    }
                                }
                                // 未合并才前进；合并分支保持索引继续检查链式段。
                                i++;
                            }
                        }
                    }
                }

                // 依据固定点结果选择最小表示；受保护结构始终保留 Polyline。
                if ( tempSubs.empty() && !preserveAnnotatedStructure ) {
                    // 无有效段时退化为父锚点处普通 Note，并继承父级属性。
                    Note& n       = beatMap.m_noteData.notes.emplace_back();
                    n.m_type      = NoteType::NOTE;
                    n.m_timestamp = poly.m_timestamp;
                    n.m_track     = poly.m_track;
                    applyNoteSampleBinding(n, poly.getSampleBinding());
                    n.m_metadata        = poly.m_metadata;
                    n.m_annotation      = poly.m_annotation;
                    n.m_collaborationId = poly.m_collaborationId;
                    // 临时空壳已不代表最终对象，立即从拥有容器移除。
                    beatMap.m_noteData.polylines
                        .pop_back();  // 移除预先创建的空壳
                } else if ( tempSubs.size() == 1 &&
                            !preserveAnnotatedStructure ) {
                    // 唯一有效段退化为对应基础类型，父级身份与元数据继续保留。
                    const auto& s = tempSubs[0];
                    if ( s.type == NoteType::HOLD ) {
                        // 子段采样优先，不存在时回退父折线采样。
                        Hold& h       = beatMap.m_noteData.holds.emplace_back();
                        h.m_type      = NoteType::HOLD;
                        h.m_timestamp = s.timestamp;
                        h.m_track     = s.track;
                        h.m_duration  = s.duration;
                        applyNoteSampleBinding(h,
                                               s.sampleBinding
                                                   ? s.sampleBinding
                                                   : poly.getSampleBinding());
                        h.m_metadata        = poly.m_metadata;
                        h.m_annotation      = poly.m_annotation;
                        h.m_collaborationId = poly.m_collaborationId;
                    } else if ( s.type == NoteType::FLICK ) {
                        Flick& f = beatMap.m_noteData.flicks.emplace_back();
                        f.m_type = NoteType::FLICK;
                        f.m_timestamp = s.timestamp;
                        f.m_track     = s.track;
                        f.m_dtrack    = s.dtrack;
                        applyNoteSampleBinding(f,
                                               s.sampleBinding
                                                   ? s.sampleBinding
                                                   : poly.getSampleBinding());
                        f.m_metadata        = poly.m_metadata;
                        f.m_annotation      = poly.m_annotation;
                        f.m_collaborationId = poly.m_collaborationId;
                    } else {
                        Note& n       = beatMap.m_noteData.notes.emplace_back();
                        n.m_type      = NoteType::NOTE;
                        n.m_timestamp = s.timestamp;
                        n.m_track     = s.track;
                        applyNoteSampleBinding(n,
                                               s.sampleBinding
                                                   ? s.sampleBinding
                                                   : poly.getSampleBinding());
                        n.m_metadata        = poly.m_metadata;
                        n.m_annotation      = poly.m_annotation;
                        n.m_collaborationId = poly.m_collaborationId;
                    }
                    // 基础物件完成构造后移除不再使用的 Polyline 空壳。
                    beatMap.m_noteData.polylines
                        .pop_back();  // 移除预先创建的空壳
                } else {
                    // 多段或受保护结构保留 Polyline，并在类型拥有容器构造节点。
                    for ( const auto& s : tempSubs ) {
                        if ( s.type == NoteType::HOLD ) {
                            // Hold 加入通用节点视图与 Hold 分类视图。
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp       = s.timestamp;
                            h.m_track           = s.track;
                            h.m_duration        = s.duration;
                            h.m_isSubNote       = true;
                            h.m_annotation      = s.annotation;
                            h.m_collaborationId = s.collaborationId;
                            applyNoteSampleBinding(h, s.sampleBinding);
                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);
                        } else if ( s.type == NoteType::FLICK ) {
                            // Flick 加入通用节点视图与 Flick 分类视图。
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp       = s.timestamp;
                            f.m_track           = s.track;
                            f.m_dtrack          = s.dtrack;
                            f.m_isSubNote       = true;
                            f.m_annotation      = s.annotation;
                            f.m_collaborationId = s.collaborationId;
                            applyNoteSampleBinding(f, s.sampleBinding);
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
                        } else {
                            // 普通节点只加入通用 m_subNotes 视图。
                            Note& n  = beatMap.m_noteData.notes.emplace_back();
                            n.m_type = NoteType::NOTE;
                            n.m_timestamp       = s.timestamp;
                            n.m_track           = s.track;
                            n.m_isSubNote       = true;
                            n.m_annotation      = s.annotation;
                            n.m_collaborationId = s.collaborationId;
                            applyNoteSampleBinding(n, s.sampleBinding);
                            poly.m_subNotes.push_back(n);
                        }
                    }
                    // 非空折线父锚点规范化为首节点，服务统一排序与命中。
                    if ( !tempSubs.empty() ) {
                        poly.m_timestamp = tempSubs.front().timestamp;
                        poly.m_track     = tempSubs.front().track;
                    }
                }
            } else if ( type == "hold" ) {
                // 命中预扫描键的旧版重复 Hold 已随所属折线恢复，跳过顶层副本。
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                // 非重复 Hold 进入拥有容器并加载全部正交属性。
                Hold& h       = beatMap.m_noteData.holds.emplace_back();
                h.m_type      = NoteType::HOLD;
                h.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                h.m_track     = readMMMU32(nJson, "track", 0);
                h.m_duration  = readMMMDouble(nJson, "duration", 0.0);
                loadNoteSampleBinding(h, nJson);
                loadNoteMetadata(h, nJson);
                loadNoteAnnotation(h, nJson);
                loadNoteCollaborationIdentity(h, nJson);
            } else if ( type == "flick" ) {
                // 命中预扫描键的旧版重复 Flick 不再构造顶层副本。
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                // 非重复 Flick 保存时间、轨道和离散目标轨偏移。
                Flick& f      = beatMap.m_noteData.flicks.emplace_back();
                f.m_type      = NoteType::FLICK;
                f.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                f.m_track     = readMMMU32(nJson, "track", 0);
                f.m_dtrack    = readMMMInt(nJson, "dtrack", 0);
                loadNoteSampleBinding(f, nJson);
                loadNoteMetadata(f, nJson);
                loadNoteAnnotation(f, nJson);
                loadNoteCollaborationIdentity(f, nJson);
            } else {
                // 未知 type 按普通 Note 兼容；旧版重复键仍优先跳过。
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                // 普通 Note 只需公共字段、采样、元数据、批注和身份。
                Note& n       = beatMap.m_noteData.notes.emplace_back();
                n.m_type      = NoteType::NOTE;
                n.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                n.m_track     = readMMMU32(nJson, "track", 0);
                loadNoteSampleBinding(n, nJson);
                loadNoteMetadata(n, nJson);
                loadNoteAnnotation(n, nJson);
                loadNoteCollaborationIdentity(n, nJson);
            }
        }
    }

    // 第五阶段加载独立多批注；单条非法记录不阻断其他谱面内容。
    auto annotationIt = root.find("annotations");
    if ( annotationIt != root.end() && annotationIt->is_array() ) {
        // ID 集合同时执行文件内唯一性校验。
        std::set<std::string> annotationIds;
        for ( const auto& annotationJson : *annotationIt ) {
            // 达到总量上限后终止，避免恶意数组继续消耗内存。
            if ( beatMap.m_annotations.size() >= MAX_BEATMAP_ANNOTATION_COUNT ||
                 !annotationJson.is_object() ) {
                break;
            }
            BeatmapAnnotation annotation;
            annotation.m_id       = readMMMString(annotationJson, "id");
            annotation.m_targetId = readMMMString(annotationJson, "target_id");
            annotation.m_timestamp =
                readMMMDouble(annotationJson, "timestamp", 0.0);
            annotation.m_author  = readMMMString(annotationJson, "author");
            annotation.m_content = readMMMString(annotationJson, "content");

            // 缺少 target_kind 时兼容为纯时间点批注。
            const std::string targetKind =
                readMMMString(annotationJson, "target_kind", "timestamp");
            if ( targetKind == "player_object" ) {
                annotation.m_targetKind =
                    BeatmapAnnotationTargetKind::PLAYER_OBJECT;
            } else if ( targetKind == "audio_sample" ) {
                annotation.m_targetKind =
                    BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
            } else if ( targetKind != "timestamp" ) {
                continue;
            }

            // 长度、唯一性和对象目标约束集中检查，通过后才进入拥有容器。
            if ( annotation.m_id.empty() ||
                 annotation.m_id.size() > MAX_BEATMAP_ANNOTATION_ID_BYTES ||
                 annotation.m_targetId.size() >
                     MAX_BEATMAP_ANNOTATION_ID_BYTES ||
                 annotation.m_author.size() >
                     MAX_BEATMAP_ANNOTATION_AUTHOR_BYTES ||
                 annotation.m_content.empty() ||
                 annotation.m_content.size() >
                     MAX_BEATMAP_ANNOTATION_CONTENT_BYTES ||
                 !annotationIds.insert(annotation.m_id).second ||
                 (annotation.m_targetKind !=
                      BeatmapAnnotationTargetKind::TIMESTAMP &&
                  annotation.m_targetId.empty()) ) {
                continue;
            }
            // 记录已经完整验证，可安全移动到最终数组。
            beatMap.m_annotations.push_back(std::move(annotation));
        }
    }

    // 全部拥有容器和折线引用就绪后，统一重建排序视图与派生元数据。
    beatMap.sync();

    return beatMap;
}

}  // namespace MMM
