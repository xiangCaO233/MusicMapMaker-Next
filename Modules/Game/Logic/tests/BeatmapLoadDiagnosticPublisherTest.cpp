#include "logic/BeatmapLoadDiagnosticPublisher.h"

#include "event/core/EventBus.h"
#include "mmm/beatmap/BeatMap.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{

/// @brief 验证一次载入内的重复 Loader 诊断只产生一条 UI 事件。
/// @return 事件字段和去重行为均正确时返回 true。
/// @note 直接构造 Loader 结果，不执行旧谱面解析或查找原始 Malody 文件。
/// @note 重复项完全相同，不对只有部分字段相同的诊断是否合并作出断言。
bool testDiagnosticConversionAndDeduplication()
{
    MMM::BeatMap beatmap;
    // 主谱面路径与关联原谱路径不同，避免转换时把两种定位信息混为一谈。
    beatmap.m_baseMapMetadata.map_path = "/project/chart/legacy_chart.mmm";

    // 类型与严重程度使用模型层枚举，后面检查它们是否映射到事件层枚举。
    const MMM::BeatmapLoadDiagnostic diagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,
        .m_severity = MMM::BeatmapLoadDiagnosticSeverity::
            BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
        .m_message     = "Loader detail",
        .m_relatedPath = "/project/chart/legacy_chart.mc",
    };
    // 完全相同的诊断出现两次，代表一次加载内部的重复报告。
    beatmap.m_loadDiagnostics.push_back(diagnostic);
    beatmap.m_loadDiagnostics.push_back(diagnostic);

    // 构造事件的纯转换入口不需要 UI 订阅者，也不应依赖翻译显示结果。
    const auto events = MMM::Logic::buildBeatmapLoadDiagnosticEvents(beatmap);
    // 先断言唯一条目再访问 front，空结果会直接失败而不会越界。
    // 同时校验路径与原始详情，防止去重正确却丢失用于提示的上下文。
    return events.size() == 1 &&
           events.front().m_kind == MMM::Event::BeatmapLoadDiagnosticKind::
                                        LegacyMmmOriginalMalodyAvailable &&
           events.front().m_severity ==
               MMM::Event::BeatmapLoadDiagnosticSeverity::Warning &&
           events.front().m_beatmapPath == "/project/chart/legacy_chart.mmm" &&
           events.front().m_relatedPath == "/project/chart/legacy_chart.mc" &&
           events.front().m_message == "Loader detail";
}

/// @brief 验证非法自动采样轨道迁移诊断具有独立且确定的事件类型。
/// @return 诊断类型、级别和详情均完整转换时返回 true。
/// @note 本例验证迁移结果的报告，不执行音频采样轨道迁移算法。
/// @note Warning 是提示的严重程度，不等同于谱面加载失败状态。
bool testAudioSampleTrackRelocationDiagnostic()
{
    // 与旧格式提示使用不同的诊断编码，避免所有警告被映射为同一事件类别。
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path = "/project/chart/relocated.mmm";
    // 路径从所属模型取出，诊断结构本身只携带可选的关联路径。
    // 不提供关联路径，转换后应保持为空，而不是回填主谱面或上一事件路径。
    beatmap.m_loadDiagnostics.push_back(MMM::BeatmapLoadDiagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED,
        .m_severity = MMM::BeatmapLoadDiagnosticSeverity::
            BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
        .m_message = "Moved x=2 to x=4",
    });

    // 保留 Loader 的具体迁移说明，不由转换层重新拼接或改写详情。
    const auto events = MMM::Logic::buildBeatmapLoadDiagnosticEvents(beatmap);
    return events.size() == 1 &&
           events.front().m_kind == MMM::Event::BeatmapLoadDiagnosticKind::
                                        AudioSampleTrackRelocated &&
           events.front().m_severity ==
               MMM::Event::BeatmapLoadDiagnosticSeverity::Warning &&
           events.front().m_beatmapPath == "/project/chart/relocated.mmm" &&
           events.front().m_relatedPath.empty() &&
           events.front().m_message == "Moved x=2 to x=4";
}

/// @brief 验证无 Loader 诊断时发布入口不会通知订阅者。
/// @return 本地计数订阅者未被调用时返回 true。
/// @note 本例只观察发布结果，不单独断言内部事件向量的构造过程。
/// @note 缺少主谱面路径也不应自行制造诊断；诊断来源仍是 Loader 结果列表。
bool testNoDiagnosticHasNoBehavior()
{
    MMM::BeatMap beatmap;
    // 空模型没有诊断，但仍调用正常发布入口，覆盖无提示的常规加载结果。
    std::size_t receivedCount = 0;
    // 只订阅诊断事件，不把其它初始化事件计入接收次数。
    const auto subscriptionId =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(
                [&](const MMM::Event::BeatmapLoadDiagnosticEvent&) {
                    // 捕获局部计数器；本用例在退出作用域前必须解除订阅。
                    ++receivedCount;
                });

    MMM::Logic::publishBeatmapLoadDiagnostics(beatmap);
    // 在判断结果前解除订阅，失败返回也不会遗留引用已销毁栈变量的回调。
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(subscriptionId);
    return receivedCount == 0;
}

/// @brief 验证发布入口按一次载入的去重结果投递事件。
/// @return 相同诊断只发布一次时返回 true。
/// @note 与纯转换用例互补，确认公开发布入口确实消费了去重后的结果。
bool testPublisherUsesDeduplicatedEvents()
{
    // 使用独立模型隔离前面转换用例，去重状态不应由测试之间的历史建立。
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path = "/project/chart/legacy.mmm";
    const MMM::BeatmapLoadDiagnostic diagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,
        .m_severity = MMM::BeatmapLoadDiagnosticSeverity::
            BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
        .m_message     = "Loader detail",
        .m_relatedPath = "/project/chart/legacy.mc",
    };
    beatmap.m_loadDiagnostics = { diagnostic, diagnostic };

    // 记录投递次数与有效载荷，不能仅凭总次数判断发布内容正确。
    std::size_t receivedCount = 0;
    std::string receivedPath;
    // 用空字符串起始，确保没有回调时不能凭预填的期望路径通过载荷检查。
    const auto subscriptionId =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(
                [&](const MMM::Event::BeatmapLoadDiagnosticEvent& event) {
                    ++receivedCount;
                    // 复制字段而非保存事件引用，避免依赖发布入口中临时事件的生命周期。
                    receivedPath = event.m_relatedPath;
                });

    // 事件总线同步投递，返回后即可检查本次发布触发的回调结果。
    MMM::Logic::publishBeatmapLoadDiagnostics(beatmap);
    // 单例总线跨用例存在，订阅本身必须在本用例结束前清理。
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(subscriptionId);
    // 收到一次但关联路径错误仍应失败，避免只发出没有可定位内容的提示。
    return receivedCount == 1 && receivedPath == "/project/chart/legacy.mc";
}

}  // namespace

/// @brief 覆盖旧 MMM 加载诊断的事件转换、去重和空输入行为。
/// @return 所有断言通过时返回 0。
/// @note 不验证实际提示窗口、翻译或关联文件是否存在。
/// @note 去重范围只覆盖单次输入内的重复项，不验证跨次发布是否再次通知。
/// @note 两个具体编码均为警告，不覆盖其它严重程度的枚举映射。
int main()
{
    // 先验证字段转换，再验证总线投递，便于区分映射错误与发布流程错误。
    // 每个订阅用例自行解除回调，不依赖进程退出清理全局总线。
    // 所有路径仅作为字符串载荷，不会创建、读取或覆盖测试路径中的文件。
    // 保留短路顺序和既有退出码，不因补注释改变失败处理方式。
    return testDiagnosticConversionAndDeduplication() &&
                   testAudioSampleTrackRelocationDiagnostic() &&
                   testNoDiagnosticHasNoBehavior() &&
                   testPublisherUsesDeduplicatedEvents()
               ? 0
               : 1;
}
