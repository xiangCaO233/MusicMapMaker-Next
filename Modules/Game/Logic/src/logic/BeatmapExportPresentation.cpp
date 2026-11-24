#include "logic/BeatmapExportPresentation.h"

#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/metadata/BaseMapMeta.h"
#include "mmm/metadata/MapMetadata.h"
#include "mmm/metadata/NoteMetadata.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>

/**
 * @file BeatmapExportPresentation.cpp
 * @brief 从活动谱面提取保存对话框需要的兼容性和命名信息。
 *
 * UI 只得到拥有型字符串和特征标志。约定的导出格式决定要检查的元数据和
 * 物件容器，文件名清理、对话框翻译和警告文案格式化在会话锁外完成。
 * 捕获过程不会修改模型，也不会保存工程。实际导出仍需由对应格式重新校验。
 */
namespace MMM::Logic
{
namespace
{

/// @brief 去除 ASCII 空白，用于解析 Malody mode 元数据。
/// @param text 原始字符串视图。
/// @return 去除首尾空白后的字符串视图。
/// @note 只处理谱面元数据允许出现的四种 ASCII 空白，不进行 Unicode 归一化。
std::string_view trimAsciiWhitespace(std::string_view text)
{
    // remove_prefix 只移动视图起点，不复制底层字符串。
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    // 尾部同样在视图上收缩，返回值仍引用调用方存储。
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析整数字符串。
/// @param text 待解析文本。
/// @return 成功时返回整数，否则返回空。
/// @note 使用 from_chars 避免区域设置和异常，并要求完整文本都被消费。
std::optional<int> parseAsciiInteger(std::string_view text)
{
    // 元数据允许首尾 ASCII 空白，解析前统一裁掉。
    text = trimAsciiWhitespace(text);
    // 空白元数据不是有效 mode。
    if ( text.empty() ) return std::nullopt;

    int value = 0;
    // from_chars 不分配内存，也不会抛出异常。
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        // 范围错误或残留非数字字符均视为解析失败。
        return std::nullopt;
    }
    return value;
}

/// @brief 获取谱面当前 Malody mode 元数据，缺省时按导出器默认 slide(7) 处理。
/// @param beatMap 当前谱面。
/// @return mode 元数据有效时返回 mode；无法解析时返回空。
/// @note 缺少 MALODY 或 mode 字段时沿用导出器的 Slide 模式默认值 7。
std::optional<int> resolveMalodyModeForCompatibilityWarning(
    const BeatMap& beatMap)
{
    // 7 与 Malody Slide 模式协议值一致。
    int mode = 7;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("mode") ) {
            // 显式 mode 必须完整解析，非法值不能静默回退以免漏警告。
            auto parsedMode = parseAsciiInteger(it->second.at("mode"));
            if ( !parsedMode ) return std::nullopt;
            mode = *parsedMode;
        }
    }
    // 返回协议整数，调用方再映射为支持的导出模式。
    return mode;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的基础元数据。
/// @param meta 基础谱面元数据。
/// @return 存在不支持字段时返回 true。
/// @note 只要任一非 RM 基础字段携带有效数据就需要兼容性提醒。
bool hasUnsupportedImdBaseMetadata(const BaseMapMeta& meta)
{
    return !meta.title.empty() || !meta.title_unicode.empty() ||
           !meta.artist.empty() || !meta.artist_unicode.empty() ||
           !meta.album.empty() || !meta.author.empty() ||
           !meta.song_file_hint.empty() || !meta.main_audio_path.empty() ||
           !meta.main_cover_path.empty() || !meta.cover_path.empty() ||
           meta.video_starttime != 0 || meta.bgxoffset != 0 ||
           meta.bgyoffset != 0;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的谱面扩展元数据。
/// @param metadata 谱面扩展元数据。
/// @return 存在不支持字段时返回 true。
/// @note RM 仅允许 mapLength 与 tabRows，其余来源或键都会在导出中丢失。
bool hasUnsupportedImdMapMetadata(const MapMetadata& metadata)
{
    // map_properties 按来源分组，空属性组不构成数据丢失。
    for ( const auto& [source, properties] : metadata.map_properties ) {
        if ( properties.empty() ) continue;
        // 非 RM 来源没有对应 IMD 字段，必须发出警告。
        if ( source != MapMetadataType::RM ) return true;
        for ( const auto& [key, value] : properties ) {
            (void)value;
            // RM 白名单只包含格式原生维护的两个字段。
            if ( key != "mapLength" && key != "tabRows" ) return true;
        }
    }
    return false;
}

/// @brief 判断谱面物件是否包含 RM/IMD 无法保存的额外物件元数据。
/// @param beatMap 当前谱面。
/// @return 存在不支持字段时返回 true。
/// @warning 低频导出确认路径：遍历完整物件及其元数据，不得用于逐帧 UI。
bool hasUnsupportedImdNoteMetadata(const BeatMap& beatMap)
{
    // m_allNotes 提供所有音符类型的统一引用视图。
    for ( const auto& noteRef : beatMap.m_allNotes ) {
        const auto& note = noteRef.get();
        for ( const auto& [source, properties] :
              note.m_metadata.note_properties ) {
            // 空属性组不会产生导出损失。
            if ( properties.empty() ) continue;
            // 非 RM 来源的物件元数据无法写入 IMD。
            if ( source != NoteMetadataType::RM ) return true;
            for ( const auto& [key, value] : properties ) {
                (void)value;
                // RM 物件只原生支持 Parameter 字段。
                if ( key != "Parameter" ) return true;
            }
        }
    }
    return false;
}

/// @brief 读取 BMS 自动采样和普通物件的音量差异。
/// @param beatmap 由会话锁保护的活动谱面。
/// @return 任意可保存的采样绑定使用非默认音量时返回 true。
/// @note 不查 Flick 或折线的音量；格式不支持这些物件，另有专门提示。
bool hasNonDefaultBmsVolume(const BeatMap& beatmap)
{
    // 优先查短的自动采样列表，发现差异即不遍历玩家物件。
    if ( std::ranges::any_of(beatmap.m_audioSamples, [](const auto& sample) {
             return sample.m_volume != 1.0F;
         }) )
        return true;
    const auto containsVolume = [](const auto& notes) {
        // 普通音符和长条的绑定契约一致，未绑定的物件没有可降级的音量。
        return std::ranges::any_of(notes, [](const auto& note) {
            return note.m_sampleBinding &&
                   note.m_sampleBinding->m_volume != 1.0F;
        });
    };
    return containsVolume(beatmap.m_noteData.notes) ||
           containsVolume(beatmap.m_noteData.holds);
}

}  // namespace

/// @brief 在同一个会话版本中读取命名字段，返回后即可释放模型借用。
/// @return 缺少活动谱面时为空，名称为空则由选择器按旧规则生成默认文件名。
/// @warning 仅供用户主动打开或改动导出选项时调用，不用于常规渲染。
std::optional<BeatmapExportNaming> captureBeatmapExportNaming()
{
    auto& engine = EditorEngine::instance();
    // 活动索引和元数据由同一会话临界区保护，不能用旧索引去读新谱面。
    std::lock_guard lock(engine.getSessionMutex());
    const auto      session = engine.getActiveSession();
    // 欢迎页或载入中没有可用谱面，调用方按空结果显示默认内容。
    if ( !session || !session->getContext().currentBeatmap )
        return std::nullopt;
    // 只借用基础元数据，不读取导出无关的扩展字段或音符。
    const auto& meta = session->getContext().currentBeatmap->m_baseMapMetadata;
    // ZIP 与散装 IMD 原本都优先取 Unicode，再回退普通标题及内部名称。
    const auto& title = !meta.title_unicode.empty()
                            ? meta.title_unicode
                            : (!meta.title.empty() ? meta.title : meta.name);
    // return 初始化每个字符串的自有存储，离开作用域即可安全释放会话锁。
    return BeatmapExportNaming{
        meta.name, title, meta.version, meta.track_count
    };
}

/// @brief 只分析请求格式的谱面能力差异，不在会话锁下格式化 UI 文案。
/// @param format 影响扫描范围的目标格式，由调用方根据最终扩展名选择。
/// @return 一致版本的结构标志，关闭当前会话后仍然有效。
/// @warning BMS 和 IMD 兼容性检查最多遍历整张谱面，仅用于导出流程。
std::optional<BeatmapExportCompatibility> captureBeatmapExportCompatibility(
    BeatmapExportFormat format)
{
    auto& engine = EditorEngine::instance();
    // 组合标记必须来自同一版本；查询返回后不保留谱面引用。
    std::lock_guard lock(engine.getSessionMutex());
    const auto      session = engine.getActiveSession();
    // 欢迎页或载入中没有可用谱面，调用方按空结果显示默认内容。
    if ( !session || !session->getContext().currentBeatmap )
        return std::nullopt;
    const auto& beatmap = *session->getContext().currentBeatmap;

    BeatmapExportCompatibility result;
    // 四种目标都需要操作轨数和扩展物件标志，不用遍历物件数组。
    result.trackCount  = beatmap.m_baseMapMetadata.track_count;
    result.hasFlick    = !beatmap.m_noteData.flicks.empty();
    result.hasPolyline = !beatmap.m_noteData.polylines.empty();
    if ( format == BeatmapExportFormat::Malody ) {
        // MC 仅解析现有模式，不能为候选选项触发 timing 或物件扫描。
        result.malodyMode = resolveMalodyModeForCompatibilityWarning(beatmap);
        return result;
    }
    if ( format == BeatmapExportFormat::Bms ) {
        // BMS 默认保存 BPM/STOP，Scroll 的支持还取决于导出的兼容性选项。
        result.hasBmsTimingDifferences =
            std::ranges::any_of(beatmap.m_timings, [](const auto& timing) {
                return timing.m_timingEffect != TimingEffect::BPM &&
                       timing.m_timingEffect != TimingEffect::STOP;
            });
        // 标准 BMS 对逐物件增益的降级与 timing 能力相互独立。
        result.hasNonDefaultSampleVolume = hasNonDefaultBmsVolume(beatmap);
        return result;
    }

    // osu 与 IMD 共用 timing 判定。负 Scroll 单独记录给 osu!；IMD 的
    // 既有提示只覆盖 Jump、HS、Scroll，不扩大到其它历史效果。
    for ( const auto& timing : beatmap.m_timings ) {
        const auto effect = timing.m_timingEffect;
        if ( effect == TimingEffect::JUMP || effect == TimingEffect::HS ) {
            result.hasJumpOrHs             = true;
            result.hasImdTimingDifferences = true;
        } else if ( effect == TimingEffect::SCROLL ) {
            // 即使已遇到正的 Scroll，也要保留之后出现负倍率的证据。
            result.hasImdTimingDifferences = true;
            result.hasNegativeScroll |= timing.m_timingEffectParameter < 0.0;
        }
    }
    if ( format == BeatmapExportFormat::Imd ) {
        // Map 与物件各保留一个退化标志；UI 可分别生成兼容性提示。
        result.hasImdMapMetadataDifferences =
            hasUnsupportedImdBaseMetadata(beatmap.m_baseMapMetadata) ||
            hasUnsupportedImdMapMetadata(beatmap.m_metadata);
        // 物件元数据有独立文案，不因已发现谱面字段差异而跳过检查。
        result.hasImdNoteMetadataDifferences =
            hasUnsupportedImdNoteMetadata(beatmap);
    }
    return result;
}

}  // namespace MMM::Logic
