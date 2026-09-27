#include "logic/BeatmapLoadDiagnosticPublisher.h"

#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "mmm/beatmap/BeatMap.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace MMM::Logic
{
namespace
{

/// @brief 将 MMM Loader 诊断级别转换为 Event 层稳定枚举。
/// @param severity Loader 诊断级别。
/// @return 对应的事件提示级别。
/// @note 转换不依赖两个模块枚举的底层数值保持一致。
Event::BeatmapLoadDiagnosticSeverity convertSeverity(
    MMM::BeatmapLoadDiagnosticSeverity severity)
{
    // 加载器与事件层各自拥有枚举，显式映射避免内部枚举调整影响 UI 提示。
    switch ( severity ) {
    case MMM::BeatmapLoadDiagnosticSeverity::
        BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_INFO:
        return Event::BeatmapLoadDiagnosticSeverity::Info;
    case MMM::BeatmapLoadDiagnosticSeverity::
        BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING:
        return Event::BeatmapLoadDiagnosticSeverity::Warning;
    case MMM::BeatmapLoadDiagnosticSeverity::
        BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_ERROR:
        return Event::BeatmapLoadDiagnosticSeverity::Error;
    }
    // 未识别的级别保留可见警告，不静默当作普通信息丢失问题提示。
    return Event::BeatmapLoadDiagnosticSeverity::Warning;
}

}  // namespace

/// @brief 将谱面加载诊断转换成去重后的事件值集合。
/// @param beatmap 已完成加载并保存诊断信息的谱面。
/// @return 按首次出现顺序排列的独立事件，不借用谱面内字符串存储。
/// @note 此函数仅构造结果，不发布事件、不修改原诊断列表。
/// @details 不同类别即使关联同一路径也分别保留，以免遮蔽不同修复建议。
/// @pre 诊断码应属于本转换器显式支持的类型集合。
/// @warning 加载完成后的低频路径，允许分配容器与格式化路径，不用于每帧更新。
std::vector<Event::BeatmapLoadDiagnosticEvent> buildBeatmapLoadDiagnosticEvents(
    const MMM::BeatMap& beatmap)
{
    std::vector<Event::BeatmapLoadDiagnosticEvent> events;
    events.reserve(beatmap.m_loadDiagnostics.size());
    // 原诊断数是输出条目的上界，预留空间后去重只会减少实际条目数。

    std::unordered_set<std::string> emittedKeys;
    // 去重范围限于本次谱面加载，不跨文件或跨次加载屏蔽相同问题。
    emittedKeys.reserve(beatmap.m_loadDiagnostics.size());

    const std::string beatmapPath = Config::pathToUtf8Generic(
        // 使用词法规范化和通用分隔符，不访问文件系统解析真实路径。
        beatmap.m_baseMapMetadata.map_path.lexically_normal());
    for ( const auto& diagnostic : beatmap.m_loadDiagnostics ) {
        // 诊断码转换为界面可识别的事件类别，消息文本不参与类别猜测。
        Event::BeatmapLoadDiagnosticKind kind;
        switch ( diagnostic.m_code ) {
        case MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE:
            // 保留“存在原始 Malody 文件”的具体类别，供接收方提供对应操作。
            kind = Event::BeatmapLoadDiagnosticKind::
                LegacyMmmOriginalMalodyAvailable;
            break;
        case MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED:
            // 自动采样轨道迁移与旧格式提示分开，不能合并为同一种通用警告。
            kind = Event::BeatmapLoadDiagnosticKind::AudioSampleTrackRelocated;
            break;
        }

        const std::string relatedPath = Config::pathToUtf8Generic(
            // 关联路径先规范化再去重，消除普通路径表达差异带来的重复提示。
            diagnostic.m_relatedPath.lexically_normal());
        const std::string deduplicationKey =
            // 同类别、同关联路径视为同一问题，不把消息措辞差异当作新事件。
            std::to_string(static_cast<std::uint8_t>(kind)) + '\n' +
            relatedPath;
        // 谱面路径在本次转换中固定，无需重复放入每个去重键。
        if ( !emittedKeys.insert(deduplicationKey).second ) {
            // 保留首次出现记录及其级别、消息，不由后续重复诊断覆盖。
            continue;
        }

        // 每条事件都携带所属谱面路径，接收方可在多谱面场景定位问题来源。
        events.push_back(Event::BeatmapLoadDiagnosticEvent{
            .m_kind        = kind,
            .m_severity    = convertSeverity(diagnostic.m_severity),
            .m_beatmapPath = beatmapPath,
            .m_relatedPath = relatedPath,
            .m_message     = diagnostic.m_message,
            // 消息按值复制，发布后的展示数据不依赖原谱面持续存活。
        });
    }
    return events;
}

/// @brief 发布一次谱面加载的去重诊断事件。
/// @param beatmap 包含加载诊断的谱面模型。
/// @note 用户提示和后续操作由事件订阅者处理，此处不直接创建 UI。
/// @note 调用本函数不会清空诊断，重复调用会重新发布同一批事件。
/// @warning 加载通知路径，构造完整事件集合后逐个发布，不应反复用于状态轮询。
void publishBeatmapLoadDiagnostics(const MMM::BeatMap& beatmap)
{
    // 构造与分发分开，去重规则可独立复用，空诊断列表自然不产生通知。
    for ( const auto& event : buildBeatmapLoadDiagnosticEvents(beatmap) ) {
        Event::EventBus::instance().publish(event);
        // 按原诊断首次出现的顺序发布，不按哈希容器迭代顺序重排。
    }
}

}  // namespace MMM::Logic
