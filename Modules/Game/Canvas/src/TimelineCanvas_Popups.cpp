#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "canvas/TimeFormatUtils.h"
#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <imgui_internal.h>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 新建 Timing 行的高亮持续时间（秒）
constexpr double NEW_TIMING_HIGHLIGHT_DURATION = 3.0;

/// @brief 表格分拍位拟合使用的最大分母。
constexpr int TIMING_TABLE_MAX_BEAT_DENOMINATOR = 64;

/// @brief 表格拍位换算使用的误差阈值。
constexpr double TIMING_TABLE_BEAT_EPSILON = 1e-6;

/// @brief 表格分拍位拟合误差提示阈值，单位毫秒。
constexpr double TIMING_TABLE_FRACTION_WARNING_MS = 3.0;

/// @brief 时间线表格列数量。
constexpr int TIMING_TABLE_COLUMN_COUNT = 7;

/// @brief 时间线表格各列最小宽度。
constexpr std::array<float, TIMING_TABLE_COLUMN_COUNT>
    TIMING_TABLE_COLUMN_MIN_WIDTHS{ 44.0f, 150.0f, 110.0f, 130.0f,
                                    80.0f, 130.0f, 130.0f };

/// @brief 时间线表格局部滚动条宽度。
constexpr float TIMING_TABLE_SCROLLBAR_SIZE = 24.0f;

/// @brief 时间线表格局部滚动条拖拽块最小尺寸。
constexpr float TIMING_TABLE_SCROLLBAR_GRAB_MIN_SIZE = 28.0f;

/// @brief 时间线表格拍位换算使用的 BPM 锚点。
struct TimingTableBeatPoint {
    /// @brief BPM 时间点，单位秒。
    double time{ 0.0 };
    /// @brief 当前段 BPM。
    double bpm{ 120.0 };
    /// @brief 该时间点对应的连续拍位置。
    double beat{ 0.0 };
};

/// @brief 时间线表格可双向换算的连续拍位时间线。
using TimingTableBeatTimeline = std::vector<TimingTableBeatPoint>;

/// @brief 时间线表格中展示的拍号与分拍位。
struct TimingTableBeatPosition {
    /// @brief 连续拍位置的整数拍号部分。
    int beatIndex{ 0 };
    /// @brief 连续拍位置的小数分拍部分，范围通常为 [0, 1)。
    double fraction{ 0.0 };
};

/// @brief 表格分拍位拟合结果。
struct TimingTableFractionFit {
    /// @brief 拟合后的拍号。
    int beatIndex{ 0 };
    /// @brief 分拍分子。
    int numerator{ 0 };
    /// @brief 分拍分母。
    int denominator{ 1 };
    /// @brief 分拍小数值。
    double fraction{ 0.0 };
    /// @brief 拟合到该分数后的时间误差，单位毫秒。
    double errorMs{ 0.0 };
};

/// @brief 分拍位文本输入缓存。
using TimingTableFractionInputBuffer = std::array<char, 32>;

/// @brief 规整表格拍位换算使用的 BPM。
/// @param bpm 待规整 BPM。
/// @param fallbackBpm BPM 无效时使用的回退值。
/// @return 可用于除法换算的 BPM。
double sanitizeTimingTableBpm(double bpm, double fallbackBpm)
{
    if ( std::isfinite(bpm) && bpm > 0.0 ) {
        return std::min(bpm, 10000.0);
    }
    if ( std::isfinite(fallbackBpm) && fallbackBpm > 0.0 ) {
        return std::min(fallbackBpm, 10000.0);
    }
    return 120.0;
}

/// @brief 取得表格拍位换算使用的快照回退 BPM。
/// @param snapshot 当前渲染快照。
/// @return 有效回退 BPM。
double timingTableFallbackBpm(const Logic::RenderSnapshot& snapshot)
{
    return sanitizeTimingTableBpm(snapshot.fallbackBpm, 120.0);
}

/// @brief 从当前快照构建表格拍位换算时间线。
/// @param snapshot 当前渲染快照。
/// @return 按时间排序并合并同时间点后的 BPM 锚点。
/// @warning UI 热路径：表格窗口打开时每帧调用，只遍历快照中的 Scroll
/// 缓存，不访问文件系统。
TimingTableBeatTimeline buildTimingTableBeatTimeline(
    const Logic::RenderSnapshot& snapshot)
{
    struct BpmEvent {
        /// @brief BPM 时间点，单位秒。
        double time{ 0.0 };
        /// @brief BPM 值。
        double bpm{ 120.0 };
    };

    const double          fallbackBpm = timingTableFallbackBpm(snapshot);
    std::vector<BpmEvent> bpmEvents;
    bpmEvents.reserve(snapshot.scrollSegments.size());
    for ( const auto& segment : snapshot.scrollSegments ) {
        if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ||
             !std::isfinite(segment.time) ) {
            continue;
        }

        bpmEvents.push_back(
            { segment.time,
              sanitizeTimingTableBpm(segment.bpmValue, fallbackBpm) });
    }

    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.time < rhs.time; });

    TimingTableBeatTimeline timeline;
    timeline.reserve(bpmEvents.size());
    for ( const auto& event : bpmEvents ) {
        if ( !timeline.empty() && std::abs(timeline.back().time - event.time) <
                                      TIMING_TABLE_BEAT_EPSILON ) {
            timeline.back().bpm = event.bpm;
            continue;
        }

        if ( timeline.empty() ) {
            timeline.push_back({ event.time, event.bpm, 0.0 });
            continue;
        }

        const auto&  previous = timeline.back();
        const double beat =
            previous.beat + (event.time - previous.time) * previous.bpm / 60.0;
        timeline.push_back({ event.time, event.bpm, beat });
    }
    return timeline;
}

/// @brief 将秒时间转换为连续拍位置。
/// @param timeline 表格拍位换算时间线。
/// @param time 秒时间。
/// @param fallbackBpm 无 BPM 锚点时使用的回退 BPM。
/// @return 连续拍位置。
double timingTableTimeToBeat(const TimingTableBeatTimeline& timeline,
                             double time, double fallbackBpm)
{
    if ( !std::isfinite(time) ) return 0.0;

    const double bpm = sanitizeTimingTableBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return time * bpm / 60.0;
    }

    auto it =
        std::upper_bound(timeline.begin(),
                         timeline.end(),
                         time,
                         [](double value, const TimingTableBeatPoint& point) {
                             return value < point.time;
                         });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.beat + (time - point.time) * point.bpm / 60.0;
}

/// @brief 将连续拍位置转换为秒时间。
/// @param timeline 表格拍位换算时间线。
/// @param beat 连续拍位置。
/// @param fallbackBpm 无 BPM 锚点时使用的回退 BPM。
/// @return 秒时间。
double timingTableBeatToTime(const TimingTableBeatTimeline& timeline,
                             double beat, double fallbackBpm)
{
    if ( !std::isfinite(beat) ) return 0.0;

    const double bpm = sanitizeTimingTableBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return beat * 60.0 / bpm;
    }

    auto it =
        std::upper_bound(timeline.begin(),
                         timeline.end(),
                         beat,
                         [](double value, const TimingTableBeatPoint& point) {
                             return value < point.beat;
                         });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.time + (beat - point.beat) * 60.0 / point.bpm;
}

/// @brief 将连续拍位置拆成表格展示的拍号和分拍位。
/// @param beat 连续拍位置。
/// @return 规整后的表格拍位。
TimingTableBeatPosition splitTimingTableBeatPosition(double beat)
{
    if ( !std::isfinite(beat) ) {
        return {};
    }

    int beatIndex =
        static_cast<int>(std::floor(beat + TIMING_TABLE_BEAT_EPSILON));
    double fraction = beat - static_cast<double>(beatIndex);
    if ( fraction < TIMING_TABLE_BEAT_EPSILON ) {
        fraction = 0.0;
    }
    if ( fraction > 1.0 - TIMING_TABLE_BEAT_EPSILON ) {
        beatIndex++;
        fraction = 0.0;
    }
    return { beatIndex, fraction };
}

/// @brief 将连续拍位置拟合到最大 1/64 精度。
/// @param beat 连续拍位置。
/// @return 拟合后的分数拍位置。
TimingTableFractionFit fitTimingTableFraction(double beat)
{
    const auto position = splitTimingTableBeatPosition(beat);

    int    bestNumerator   = 0;
    int    bestDenominator = 1;
    double bestError       = std::abs(position.fraction);
    for ( int denominator = 1; denominator <= TIMING_TABLE_MAX_BEAT_DENOMINATOR;
          ++denominator ) {
        const int numerator = std::clamp(
            static_cast<int>(std::round(position.fraction * denominator)),
            0,
            denominator);
        const double fittedFraction =
            static_cast<double>(numerator) / static_cast<double>(denominator);
        const double error = std::abs(position.fraction - fittedFraction);
        if ( error + TIMING_TABLE_BEAT_EPSILON < bestError ||
             (std::abs(error - bestError) <= TIMING_TABLE_BEAT_EPSILON &&
              denominator < bestDenominator) ) {
            bestError       = error;
            bestNumerator   = numerator;
            bestDenominator = denominator;
        }
    }

    int beatIndex = position.beatIndex;
    if ( bestNumerator >= bestDenominator ) {
        beatIndex += bestNumerator / bestDenominator;
        bestNumerator %= bestDenominator;
    }
    if ( bestNumerator == 0 ) {
        return { beatIndex, 0, 1, 0.0, 0.0 };
    }

    const int divisor = std::gcd(bestNumerator, bestDenominator);
    bestNumerator /= divisor;
    bestDenominator /= divisor;
    return { beatIndex,
             bestNumerator,
             bestDenominator,
             static_cast<double>(bestNumerator) /
                 static_cast<double>(bestDenominator),
             0.0 };
}

/// @brief 将连续拍位置拟合为分数并计算时间误差。
/// @param beat 连续拍位置。
/// @param time 原始秒时间。
/// @param timeline 表格拍位换算时间线。
/// @param fallbackBpm 无 BPM 锚点时使用的回退 BPM。
/// @return 带时间误差的分数拍位置。
TimingTableFractionFit fitTimingTableFractionWithError(
    double beat, double time, const TimingTableBeatTimeline& timeline,
    double fallbackBpm)
{
    auto         fit        = fitTimingTableFraction(beat);
    const double fittedBeat = static_cast<double>(fit.beatIndex) + fit.fraction;
    const double fittedTime =
        timingTableBeatToTime(timeline, fittedBeat, fallbackBpm);
    fit.errorMs = std::abs(fittedTime - time) * 1000.0;
    return fit;
}

/// @brief 将分数拍位格式化为表格文本。
/// @param fit 分数拟合结果。
/// @return 分拍位文本。
std::string formatTimingTableFraction(const TimingTableFractionFit& fit)
{
    if ( fit.numerator == 0 ) {
        return "0";
    }
    return fmt::format("{}/{}", fit.numerator, fit.denominator);
}

/// @brief 去除 ASCII 空白。
/// @param text 原始文本。
/// @return 去除首尾空白后的文本。
std::string_view trimTimingTableAsciiWhitespace(std::string_view text)
{
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析整数文本。
/// @param text 原始文本。
/// @return 成功时返回整数，否则返回空。
std::optional<int> parseTimingTableInteger(std::string_view text)
{
    text = trimTimingTableAsciiWhitespace(text);
    if ( text.empty() ) {
        return std::nullopt;
    }

    int  value = 0;
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析浮点文本。
/// @param text 原始文本。
/// @return 成功时返回双精度值，否则返回空。
std::optional<double> parseTimingTableDouble(std::string_view text)
{
    text = trimTimingTableAsciiWhitespace(text);
    if ( text.empty() ) {
        return std::nullopt;
    }

    double value = 0.0;
    auto   result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析分拍位输入文本。
/// @param text 输入文本，可为小数或 numerator/denominator。
/// @return 成功时返回分拍小数值，否则返回空。
std::optional<double> parseTimingTableFractionText(std::string_view text)
{
    text = trimTimingTableAsciiWhitespace(text);
    if ( text.empty() ) {
        return std::nullopt;
    }

    const size_t slashPos = text.find('/');
    if ( slashPos == std::string_view::npos ) {
        return parseTimingTableDouble(text);
    }

    const auto numerator   = parseTimingTableInteger(text.substr(0, slashPos));
    const auto denominator = parseTimingTableInteger(text.substr(slashPos + 1));
    if ( !numerator || !denominator || *denominator <= 0 ) {
        return std::nullopt;
    }
    return static_cast<double>(*numerator) / static_cast<double>(*denominator);
}

/// @brief 将文本复制到分拍位输入缓存。
/// @param buffer 输入缓存。
/// @param text 待写入文本。
void copyTimingTableTextToBuffer(TimingTableFractionInputBuffer& buffer,
                                 std::string_view                text)
{
    const size_t count = std::min(buffer.size() - 1U, text.size());
    std::copy_n(text.data(), count, buffer.data());
    buffer[count] = '\0';
}

/// @brief 绘制分拍位分数输入框。
/// @param id ImGui 控件 ID。
/// @param fit 当前拟合结果。
/// @param fraction 输出的分拍小数值。
/// @return 用户提交了合法输入时返回 true。
/// @warning UI 热路径：仅维护短文本缓存与解析用户输入，不访问文件系统。
bool drawTimingTableFractionInput(const char*                   id,
                                  const TimingTableFractionFit& fit,
                                  double&                       fraction)
{
    static std::unordered_map<ImGuiID, TimingTableFractionInputBuffer>
        editBuffers;

    const ImGuiID inputId = ImGui::GetID(id);
    auto&         buffer  = editBuffers[inputId];
    if ( ImGui::GetActiveID() != inputId ) {
        copyTimingTableTextToBuffer(buffer, formatTimingTableFraction(fit));
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(
        id, buffer.data(), buffer.size(), ImGuiInputTextFlags_CharsNoBlank);
    if ( !ImGui::IsItemDeactivatedAfterEdit() ) {
        return false;
    }

    const auto parsed = parseTimingTableFractionText(buffer.data());
    if ( !parsed || !std::isfinite(*parsed) ) {
        return false;
    }

    fraction = *parsed;
    return true;
}

/// @brief 将连续拍位置格式化为 Malody timing metadata 的 beat 数组。
/// @param beat 连续拍位置。
/// @return JSON 数组文本。
std::string makeMalodyBeatMetadataValue(double beat)
{
    const auto fit = fitTimingTableFraction(beat);
    if ( fit.numerator == 0 ) {
        return fmt::format("[{},0,1]", fit.beatIndex);
    }

    return fmt::format(
        "[{},{},{}]", fit.beatIndex, fit.numerator, fit.denominator);
}

/// @brief 读取指定 Timing 实体当前持有的元数据。
/// @param entity Timing 实体。
/// @return 找不到实体时返回空元数据。
::MMM::TimingMetadata readTimelineMetadataOrDefault(entt::entity entity)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) {
        return {};
    }

    auto& registry = session->getContext().timelineRegistry;
    if ( const auto* timeline =
             registry.try_get<const Logic::TimelineComponent>(entity) ) {
        return timeline->m_metadata;
    }
    return {};
}

/// @brief 使用指定连续拍位置生成带 Malody beat 的 Timing 元数据。
/// @param entity Timing 实体。
/// @param beat 连续拍位置。
/// @return 保留原有字段并更新 beat 后的元数据。
::MMM::TimingMetadata makeTimelineMetadataWithMalodyBeat(entt::entity entity,
                                                         double       beat)
{
    auto metadata = readTimelineMetadataOrDefault(entity);
    metadata.timing_properties[::MMM::TimingMetadataType::MALODY]["beat"] =
        makeMalodyBeatMetadataValue(beat);
    return metadata;
}

/// @brief 发布按拍位更新 Timing 的命令。
/// @param entity Timing 实体。
/// @param beatIndex 拍号输入值。
/// @param fraction 分拍位输入值。
/// @param rawValue 当前 Timing 原始参数。
/// @param timeline 表格拍位换算时间线。
/// @param fallbackBpm 无 BPM 锚点时使用的回退 BPM。
void publishTimingBeatPositionUpdate(entt::entity entity, int beatIndex,
                                     double fraction, double rawValue,
                                     const TimingTableBeatTimeline& timeline,
                                     double                         fallbackBpm)
{
    if ( entity == entt::null || !std::isfinite(fraction) ||
         !std::isfinite(rawValue) ) {
        return;
    }

    const double requestedBeat = static_cast<double>(beatIndex) + fraction;
    const double newTime       = std::max(
        0.0, timingTableBeatToTime(timeline, requestedBeat, fallbackBpm));
    const double exportBeat =
        timingTableTimeToBeat(timeline, newTime, fallbackBpm);
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
            entity,
            newTime,
            rawValue,
            makeTimelineMetadataWithMalodyBeat(entity, exportBeat) }));
}

/// @brief 查询表格列当前是否有效显示。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 当前帧列有效显示时返回 true。
bool isTableColumnEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsEnabled;
}

/// @brief 查询表格列的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 用户设置为显示时返回 true。
bool isTableColumnUserEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsUserEnabled;
}

/// @brief 排队设置表格列下一帧的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @param enabled 是否显示。
void queueTableColumnEnabled(ImGuiTable* table, int column, bool enabled)
{
    if ( !table || column < 0 || column >= table->ColumnsCount ) {
        return;
    }
    table->Columns[column].IsUserEnabledNextFrame = enabled;
}

/// @brief 绘制 Timing 表头右键菜单。
/// @warning UI 热路径：表格绘制时每帧调用，只处理 ImGui
/// 当前表格状态与菜单样式。
void renderTimingTableHeaderContextMenu()
{
    ImGuiTable* table = ImGui::GetCurrentTable();
    if ( !table ) {
        return;
    }

    ImGuiStyle&  style = ImGui::GetStyle();
    const ImVec2 popupPadding(std::max(style.WindowPadding.x, 8.0f),
                              std::max(style.WindowPadding.y, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                               std::max(style.ItemSpacing.y, 4.0f)));
    const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
    if ( !popupOpen ) {
        ImGui::PopStyleVar(2);
        return;
    }

    const int contextColumn =
        table->ContextPopupColumn >= 0 &&
                table->ContextPopupColumn < table->ColumnsCount
            ? table->ContextPopupColumn
            : -1;
    if ( contextColumn >= 0 && isTableColumnEnabled(table, contextColumn) &&
         ::MMM::UI::FeedbackMenuItem(
             TR("ui.resource_table.size_column_fit").data()) ) {
        ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.resource_table.size_all_default").data()) ) {
        ImGui::TableSetColumnWidthAutoAll(table);
    }

    if ( ::MMM::UI::FeedbackBeginMenu(TR("ui.resource_table.reset").data()) ) {
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.reset_all").data()) ) {
            ImGui::TableResetSettings(table);
        }
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.reset_columns").data()) ) {
            ImGui::TableSetColumnWidthAutoAll(table);
        }
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.show_all_columns").data()) ) {
            for ( int column = 0; column < table->ColumnsCount; ++column ) {
                queueTableColumnEnabled(table, column, true);
            }
        }
        ::MMM::UI::FeedbackEndMenu();
    }

    ImGui::Separator();

    const std::array<const char*, 7> columnLabels{
        "序号", "时间戳 (秒)", "拍号", "分拍位", "类型", "数值", "操作"
    };
    int enabledColumnCount = 0;
    for ( int column = 0; column < table->ColumnsCount; ++column ) {
        if ( isTableColumnUserEnabled(table, column) ) {
            enabledColumnCount++;
        }
    }
    for ( int column = 0; column < table->ColumnsCount &&
                          column < static_cast<int>(columnLabels.size());
          ++column ) {
        const bool enabled   = isTableColumnUserEnabled(table, column);
        const bool canToggle = !enabled || enabledColumnCount > 1;
        if ( ::MMM::UI::FeedbackMenuItem(
                 columnLabels[column], nullptr, enabled, canToggle) ) {
            queueTableColumnEnabled(table, column, !enabled);
        }
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

/// @brief 从交互元素中提取主 Timing 类型
::MMM::TimingEffect getElementEffect(
    const Logic::TimelineInteractiveElement& el)
{
    if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
        return ::MMM::TimingEffect::BPM;
    }
    if ( el.effects & Logic::System::SCROLL_EFFECT_JUMP ) {
        return ::MMM::TimingEffect::JUMP;
    }
    if ( el.effects & Logic::System::SCROLL_EFFECT_HS ) {
        return ::MMM::TimingEffect::HS;
    }
    return ::MMM::TimingEffect::SCROLL;
}

/// @brief 获取 Timing 类型对应实体
entt::entity getElementEntity(const Logic::TimelineInteractiveElement& el)
{
    switch ( getElementEffect(el) ) {
    case ::MMM::TimingEffect::BPM: return el.bpmEntity;
    case ::MMM::TimingEffect::JUMP: return el.jumpEntity;
    case ::MMM::TimingEffect::HS: return el.hsEntity;
    case ::MMM::TimingEffect::SCROLL: return el.scrollEntity;
    }
    return entt::null;
}

/// @brief 获取 Timing 类型对应原始值
double getElementRawValue(const Logic::TimelineInteractiveElement& el)
{
    switch ( getElementEffect(el) ) {
    case ::MMM::TimingEffect::BPM: return el.bpmValue;
    case ::MMM::TimingEffect::JUMP: return el.jumpValue;
    case ::MMM::TimingEffect::HS: return el.hsValue;
    case ::MMM::TimingEffect::SCROLL: return el.scrollValue;
    }
    return 0.0;
}

/// @brief 获取 Timeline UI 中展示用的类型文本
const char* getEffectLabel(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return "BPM";
    case ::MMM::TimingEffect::SCROLL: return "流速 (SV)";
    case ::MMM::TimingEffect::JUMP: return "Jump";
    case ::MMM::TimingEffect::HS: return "HS";
    }
    return "Timing";
}

/// @brief 获取 Timeline UI 中展示用的类型颜色
ImVec4 getEffectColor(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    case ::MMM::TimingEffect::SCROLL: return ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    case ::MMM::TimingEffect::JUMP: return ImVec4(0.35f, 0.6f, 1.0f, 1.0f);
    case ::MMM::TimingEffect::HS: return ImVec4(1.0f, 0.88f, 0.25f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

/// @brief 获取用于绑定时间点批量编辑窗口的谱面快照键。
std::string_view getTimingPointsTableBeatmapKey(
    const Logic::RenderSnapshot& snapshot)
{
    if ( !snapshot.beatmapPathKey.empty() ) {
        return snapshot.beatmapPathKey;
    }
    return snapshot.beatmapName;
}

/// @brief 从当前 Session 收集完整 Timing 列表，供表格窗口编辑使用。
std::vector<Logic::TimelineInteractiveElement> collectTimelineElements()
{
    std::vector<Logic::TimelineInteractiveElement> elements;
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) return elements;

    auto& registry = session->getContext().timelineRegistry;
    auto  view     = registry.view<const Logic::TimelineComponent>();
    elements.reserve(view.size());

    for ( auto entity : view ) {
        const auto& tc = view.get<const Logic::TimelineComponent>(entity);
        Logic::TimelineInteractiveElement el;
        el.time = tc.m_timestamp;
        el.y    = 0.0f;

        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            el.effects   = Logic::System::SCROLL_EFFECT_BPM;
            el.bpmEntity = entity;
            el.bpmValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::SCROLL ) {
            el.effects      = Logic::System::SCROLL_EFFECT_SCROLL;
            el.scrollEntity = entity;
            el.scrollValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::JUMP ) {
            el.effects    = Logic::System::SCROLL_EFFECT_JUMP;
            el.jumpEntity = entity;
            el.jumpValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::HS ) {
            el.effects  = Logic::System::SCROLL_EFFECT_HS;
            el.hsEntity = entity;
            el.hsValue  = tc.m_value;
        }

        elements.push_back(el);
    }

    std::stable_sort(
        elements.begin(), elements.end(), [](const auto& a, const auto& b) {
            if ( std::abs(a.time - b.time) > 1e-6 ) return a.time < b.time;
            return a.effects < b.effects;
        });
    return elements;
}

/// @brief 读取当前活跃谱面的动画时间。
/// @param fallbackTime 活跃会话不可用时使用的兜底时间。
/// @return 当前活跃谱面的动画时间，单位秒。
double getActiveSessionTimelineTime(double fallbackTime)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) {
        return fallbackTime;
    }

    const double timelineTime = session->getContext().animateTime;
    return std::isfinite(timelineTime) ? timelineTime : fallbackTime;
}

/// @brief 查找最靠近目标时间的 Timing 行索引。
/// @param elements 已按时间排序的 Timing 元素列表。
/// @param targetTime 目标判定线时间，单位秒。
/// @return 命中的行索引；无可用行时返回 -1。
int findNearestTimelineElementIndex(
    const std::vector<Logic::TimelineInteractiveElement>& elements,
    double                                                targetTime)
{
    if ( elements.empty() || !std::isfinite(targetTime) ) {
        return -1;
    }

    auto next =
        std::lower_bound(elements.begin(),
                         elements.end(),
                         targetTime,
                         [](const Logic::TimelineInteractiveElement& element,
                            double time) { return element.time < time; });
    if ( next == elements.begin() ) {
        return 0;
    }
    if ( next == elements.end() ) {
        return static_cast<int>(elements.size() - 1U);
    }

    const auto prev      = std::prev(next);
    const auto prevDelta = std::abs(prev->time - targetTime);
    const auto nextDelta = std::abs(next->time - targetTime);
    return static_cast<int>((prevDelta <= nextDelta ? prev : next) -
                            elements.begin());
}

/// @brief 将存储值转换成编辑器显示值
/// @warning UI 热路径：表格绘制时逐行调用，只做常量时间数值归一化。
double getDisplayValue(::MMM::TimingEffect, double rawValue,
                       entt::entity = entt::null)
{
    return rawValue;
}

/// @brief 将编辑器显示值转换成存储值
/// @warning UI 热路径：用户提交编辑值时调用，不应访问文件系统或执行重型同步。
double getStoredValue(::MMM::TimingEffect, double displayValue,
                      entt::entity = entt::null)
{
    return displayValue;
}

/// @brief 从创建弹窗索引获取 Timing 类型
::MMM::TimingEffect getCreateEffect(int createType)
{
    switch ( createType ) {
    case 0: return ::MMM::TimingEffect::BPM;
    case 2: return ::MMM::TimingEffect::JUMP;
    case 3: return ::MMM::TimingEffect::HS;
    case 1:
    default: return ::MMM::TimingEffect::SCROLL;
    }
}

/// @brief 获取创建弹窗默认参数
double getDefaultCreateValue(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 120.0;
    case ::MMM::TimingEffect::SCROLL: return 1.0;
    case ::MMM::TimingEffect::JUMP: return 1000.0;
    case ::MMM::TimingEffect::HS: return 1.0;
    }
    return 1.0;
}

/// @brief 获取用于“保持画布速度”计算的基准 BPM。
double getKeepSpeedReferenceBpm()
{
    double refBpm = 120.0;
    if ( auto session = Logic::EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                refBpm = beatmap->m_baseMapMetadata.preference_bpm;
            }
        }
    }
    return refBpm;
}

/// @brief 根据新 BPM 计算保持画布下落速度所需的流速存储值。
double getKeepSpeedScrollValue(double bpm)
{
    double refBpm      = getKeepSpeedReferenceBpm();
    double safeBpm     = bpm > 1e-6 ? bpm : refBpm;
    double scrollSpeed = refBpm / safeBpm;
    return scrollSpeed > 1e-6 ? scrollSpeed : 1.0;
}

/// @brief 创建与新 BPM 同时间点的保持速度流速事件。
void createKeepSpeedScrollEvent(double time, double bpm)
{
    double finalScrollValue = getKeepSpeedScrollValue(bpm);
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
            time, ::MMM::TimingEffect::SCROLL, finalScrollValue }));
}

/// @brief 绘制按偏好格式显示、仍可编辑原始秒值的时间输入控件。
bool drawTimeEditor(const char* id, double& value,
                    const Logic::RenderSnapshot* snapshot)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Seconds ) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble(id, &value, 0.001, 0.01, "%.3f");
        return ImGui::IsItemDeactivatedAfterEdit();
    }

    std::string label = formatCanvasTime(value, snapshot) + "##" + id;
    if ( ::MMM::UI::FeedbackButton(label.c_str(), ImVec2(-FLT_MIN, 0.0f)) ) {
        ImGui::OpenPopup(id);
    }
    if ( ImGui::IsItemHovered() ) {
        const auto timeText = formatCanvasTime(value, snapshot);
        ImGui::SetTooltip("%s", timeText.c_str());
    }

    bool changed = false;
    if ( ImGui::BeginPopup(id) ) {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputDouble("##Seconds", &value, 0.001, 0.01, "%.3f");
        changed = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndPopup();
    }
    return changed;
}

/// @brief 绘制占满弹窗内容区宽度的双精度输入框。
/// @warning UI 热路径：仅写入 ImGui 下一控件宽度并绘制输入框。
bool drawFullWidthInputDouble(const char* id, double& value, double step,
                              double stepFast, const char* format)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputDouble(id, &value, step, stepFast, format);
    return ImGui::IsItemDeactivatedAfterEdit();
}

/// @brief 计算横向按钮行的等宽按钮宽度。
/// @warning UI 热路径：只读取当前内容区宽度和样式间距。
float calcButtonRowWidth(int buttonCount, float minWidth)
{
    if ( buttonCount <= 0 ) return minWidth;
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float usableWidth = ImGui::GetContentRegionAvail().x -
                              spacing * static_cast<float>(buttonCount - 1);
    return std::max(minWidth, usableWidth / static_cast<float>(buttonCount));
}
}  // namespace

/// @brief 开始跟踪一次“保持画布速度”创建出的 BPM/Scroll 联动。
void TimelineCanvas::beginKeepSpeedBinding(double time)
{
    m_keepSpeedBindingActive       = true;
    m_keepSpeedBindingTime         = time;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = true;
}

/// @brief 刷新当前“保持画布速度”联动关联的实体。
void TimelineCanvas::refreshKeepSpeedBinding(
    const std::vector<Logic::TimelineInteractiveElement>& elements)
{
    if ( !m_keepSpeedBindingActive ) return;

    auto chooseNewest = [](entt::entity current,
                           entt::entity candidate) -> entt::entity {
        if ( candidate == entt::null ) return current;
        if ( current == entt::null ) return candidate;
        return entt::to_integral(candidate) > entt::to_integral(current)
                   ? candidate
                   : current;
    };

    for ( const auto& el : elements ) {
        if ( std::abs(el.time - m_keepSpeedBindingTime) > 1e-6 ) continue;

        if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
            m_keepSpeedBindingBpmEntity =
                chooseNewest(m_keepSpeedBindingBpmEntity, el.bpmEntity);
        }
        if ( el.effects & Logic::System::SCROLL_EFFECT_SCROLL ) {
            m_keepSpeedBindingScrollEntity =
                chooseNewest(m_keepSpeedBindingScrollEntity, el.scrollEntity);
        }
    }
}

/// @brief 判断表格行是否属于当前临时联动。
bool TimelineCanvas::isKeepSpeedBindingEntity(entt::entity entity) const
{
    return m_keepSpeedBindingActive && entity != entt::null &&
           (entity == m_keepSpeedBindingBpmEntity ||
            entity == m_keepSpeedBindingScrollEntity);
}

/// @brief 使用编辑中的 BPM 值刷新联动 Scroll 值。
void TimelineCanvas::updateKeepSpeedBindingScroll(double bpm)
{
    if ( !m_keepSpeedBindingActive ||
         m_keepSpeedBindingScrollEntity == entt::null ) {
        return;
    }

    Event::EventBus::instance().publish(Event::LogicCommandEvent(
        Logic::CmdUpdateTimelineEvent{ m_keepSpeedBindingScrollEntity,
                                       m_keepSpeedBindingTime,
                                       getKeepSpeedScrollValue(bpm) }));
}

/// @brief 结束“保持画布速度”临时联动并恢复普通编辑状态。
void TimelineCanvas::finishKeepSpeedBinding()
{
    m_keepSpeedBindingActive       = false;
    m_keepSpeedBindingTime         = -1.0;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = false;
}

/// @brief 渲染 Timeline 时间点编辑弹窗。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventEditorPopup()
{
    if ( m_isPopupOpen ) {
        ::MMM::UI::FeedbackOpenPopup("TimelineEventEditor");
    }

    float dpiScale   = Config::AppConfig::instance().getWindowContentScale();
    float popupWidth = std::floor(380.0f * dpiScale);

    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("TimelineEventEditor",
                          &m_isPopupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(popupWidth, 0.0f)) ) {
        std::string typeTitle = m_editType;

        ImGui::Text(
            "%s", TR_FMT("ui.timeline.event_editor.title", typeTitle).c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##Time", m_editTime, m_currentSnapshot);

        ::MMM::TimingEffect editEffect =
            (m_editType == "BPM")    ? ::MMM::TimingEffect::BPM
            : (m_editType == "Jump") ? ::MMM::TimingEffect::JUMP
            : (m_editType == "HS")   ? ::MMM::TimingEffect::HS
                                     : ::MMM::TimingEffect::SCROLL;

        if ( editEffect == ::MMM::TimingEffect::BPM ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            drawFullWidthInputDouble("##Value", m_editValue, 0.1, 1.0, "%.2f");
        } else if ( editEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble("##Value", m_editValue, 1.0, 10.0, "%.3f");
        } else if ( editEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            drawFullWidthInputDouble("##Value", m_editValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            drawFullWidthInputDouble("##Value", m_editValue, 0.01, 0.1, "%.4f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float actionButtonWidth =
            calcButtonRowWidth(3, std::floor(80.0f * dpiScale));
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.apply").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            double finalValue =
                getStoredValue(editEffect, m_editValue, m_editingEntity);

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                    m_editingEntity, m_editTime, finalValue }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.delete").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ m_editingEntity }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.cancel").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::EndPopup();
    }
}

/// @brief 渲染 Timeline 时间点创建弹窗。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventCreationPopup()
{
    if ( m_isCreatePopupOpen ) {
        ::MMM::UI::FeedbackOpenPopup("TimelineCreateEvent");
    }

    float dpiScale   = Config::AppConfig::instance().getWindowContentScale();
    float popupWidth = std::floor(430.0f * dpiScale);

    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("TimelineCreateEvent",
                          &m_isCreatePopupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(popupWidth, 0.0f)) ) {
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::Separator();
        ImGui::Spacing();

        // 自动计算下一项 RadioButton 宽度并在空间充足时在同行显示的辅助函数
        auto getRadioButtonWidth = [](const char* label) -> float {
            ImGuiStyle& style      = ImGui::GetStyle();
            float       circleSize = ImGui::GetFrameHeight();
            float       textWidth  = ImGui::CalcTextSize(label).x;
            return circleSize + style.ItemSpacing.x + textWidth +
                   style.FramePadding.x * 2.0f;
        };

        auto wrapToNextLineIfNoSpace = [&](float nextItemWidth) {
            float lastX2 = ImGui::GetItemRectMax().x;
            float windowVisibleX2 =
                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            if ( lastX2 + spacing + nextItemWidth < windowVisibleX2 ) {
                ImGui::SameLine(0.0f, spacing);
            }
        };

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.pos_type").data());

        std::string posClickLabel =
            m_isTimeSnapped
                ? TR("ui.timeline.event_creator.pos_click_snapped").data()
                : TR("ui.timeline.event_creator.pos_click").data();
        std::string posCurrentLabel =
            TR("ui.timeline.event_creator.pos_current").data();

        if ( ::MMM::UI::FeedbackRadioButton(
                 posClickLabel.c_str(), &m_createPosType, 0) ) {
            m_createTimeManual =
                m_isTimeSnapped ? m_createTimeSnapped : m_createTimeRaw;
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth(posCurrentLabel.c_str()));

        if ( ::MMM::UI::FeedbackRadioButton(
                 posCurrentLabel.c_str(), &m_createPosType, 1) ) {
            m_createTimeManual = m_currentSnapshot->currentTime;
        }

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##CreateTime", m_createTimeManual, m_currentSnapshot);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool professionalMode = Config::AppConfig::instance()
                                          .getEditorSettings()
                                          .timelineProfessionalMode;
        if ( !professionalMode ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_creator.type").data());
            if ( ::MMM::UI::FeedbackRadioButton("BPM", &m_createType, 0) ) {
                m_createValue =
                    getDefaultCreateValue(getCreateEffect(m_createType));
            }

            wrapToNextLineIfNoSpace(getRadioButtonWidth("Scroll"));

            if ( ::MMM::UI::FeedbackRadioButton("Scroll", &m_createType, 1) ) {
                m_createValue =
                    getDefaultCreateValue(getCreateEffect(m_createType));
            }

            wrapToNextLineIfNoSpace(getRadioButtonWidth("Jump"));

            if ( ::MMM::UI::FeedbackRadioButton("Jump", &m_createType, 2) ) {
                m_createValue =
                    getDefaultCreateValue(getCreateEffect(m_createType));
            }

            wrapToNextLineIfNoSpace(getRadioButtonWidth("HS"));

            if ( ::MMM::UI::FeedbackRadioButton("HS", &m_createType, 3) ) {
                m_createValue =
                    getDefaultCreateValue(getCreateEffect(m_createType));
            }

            ImGui::Spacing();
        }

        ::MMM::TimingEffect createEffect = getCreateEffect(m_createType);
        if ( createEffect == ::MMM::TimingEffect::BPM ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            drawFullWidthInputDouble(
                "##BPMValue", m_createValue, 0.1, 1.0, "%.2f");
            ImGui::Spacing();
            ::MMM::UI::FeedbackCheckbox(
                TR("ui.timeline.event_creator.keep_speed").data(),
                &m_keepSpeedOnBpmChange);
        } else if ( createEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble(
                "##JumpValue", m_createValue, 1.0, 10.0, "%.3f");
        } else if ( createEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            drawFullWidthInputDouble(
                "##HSValue", m_createValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            drawFullWidthInputDouble(
                "##ScrollValue", m_createValue, 0.01, 0.1, "%.3f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float actionButtonWidth =
            calcButtonRowWidth(2, std::floor(100.0f * dpiScale));
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_creator.create").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            ::MMM::TimingEffect type = getCreateEffect(m_createType);
            double finalValue        = getStoredValue(type, m_createValue);

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    m_createTimeManual, type, finalValue }));
            m_lastCreatedTimingTime   = m_createTimeManual;
            m_lastCreatedTimingEffect = type;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;

            if ( type == ::MMM::TimingEffect::BPM && m_keepSpeedOnBpmChange ) {
                createKeepSpeedScrollEvent(m_createTimeManual, m_createValue);
                beginKeepSpeedBinding(m_createTimeManual);
            }

            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.cancel").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::EndPopup();
    }
}

/// @brief 渲染可批量编辑时间点的表格窗口（非模态）。
/// @warning UI 热路径：打开时每帧执行；无谱面快照时只关闭窗口并清理绑定。
void TimelineCanvas::renderTimingPointsTableWindow()
{
    if ( !m_isTableWindowOpen ) {
        m_tableBeatmapKey.clear();
        finishKeepSpeedBinding();
        return;
    }

    auto closeTableWindow = [this]() {
        m_isTableWindowOpen = false;
        m_tableBeatmapKey.clear();
        m_tableScrollToCurrentTimePending = false;
        finishKeepSpeedBinding();
    };

    auto&         engine      = Logic::EditorEngine::instance();
    const int32_t activeIndex = engine.getActiveSessionIndex();
    const auto*   activeEntry = engine.getSessionEntry(activeIndex);
    if ( !activeEntry || activeEntry->isLogoPlaceholder || !m_currentSnapshot ||
         !m_currentSnapshot->hasBeatmap ) {
        closeTableWindow();
        return;
    }

    const std::string_view currentBeatmapKey =
        getTimingPointsTableBeatmapKey(*m_currentSnapshot);
    if ( currentBeatmapKey.empty() ) {
        closeTableWindow();
        return;
    }
    if ( m_tableBeatmapKey.empty() ) {
        m_tableBeatmapKey.assign(currentBeatmapKey.data(),
                                 currentBeatmapKey.size());
    } else if ( m_tableBeatmapKey.size() != currentBeatmapKey.size() ||
                !std::equal(m_tableBeatmapKey.begin(),
                            m_tableBeatmapKey.end(),
                            currentBeatmapKey.begin()) ) {
        closeTableWindow();
        return;
    }

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(820, 450), ImGuiCond_FirstUseEver);

    std::string windowTitle =
        std::string(TR("ui.timeline.timing_points_table.title").data()) +
        "###TimingPointsTableWindow";
    const bool wasOpenBeforeBegin = m_isTableWindowOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &m_isTableWindowOpen);
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin,
                                                &m_isTableWindowOpen);
    if ( opened ) {
        auto       elements = collectTimelineElements();
        const auto beatTimeline =
            buildTimingTableBeatTimeline(*m_currentSnapshot);
        const double tableFallbackBpm =
            timingTableFallbackBpm(*m_currentSnapshot);
        refreshKeepSpeedBinding(elements);
        const double tableCurrentTime =
            getActiveSessionTimelineTime(m_currentSnapshot->currentTime);

        // 顶层工具栏
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加 BPM") ) {
            constexpr double DEFAULT_BPM_VALUE = 120.0;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ tableCurrentTime,
                                               ::MMM::TimingEffect::BPM,
                                               DEFAULT_BPM_VALUE }));
            m_lastCreatedTimingTime   = tableCurrentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::BPM;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;

            if ( m_keepSpeedOnBpmChange ) {
                createKeepSpeedScrollEvent(tableCurrentTime, DEFAULT_BPM_VALUE);
                beginKeepSpeedBinding(tableCurrentTime);
            }
        }
        ImGui::SameLine();
        ::MMM::UI::FeedbackCheckbox(
            TR("ui.timeline.event_creator.keep_speed").data(),
            &m_keepSpeedOnBpmChange);
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加流速 (SV)") ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    tableCurrentTime, ::MMM::TimingEffect::SCROLL, 1.0 }));
            m_lastCreatedTimingTime   = tableCurrentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::SCROLL;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加 Jump") ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    tableCurrentTime, ::MMM::TimingEffect::JUMP, 1000.0 }));
            m_lastCreatedTimingTime   = tableCurrentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::JUMP;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加 HS") ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    tableCurrentTime, ::MMM::TimingEffect::HS, 1.0 }));
            m_lastCreatedTimingTime   = tableCurrentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::HS;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( elements.empty() ) {
            ImGui::BeginDisabled();
        }
        if ( ::MMM::UI::FeedbackButton("定位判定线") ) {
            m_tableScrollToCurrentTimePending = true;
            m_tableScrollTargetTime           = tableCurrentTime;
        }
        if ( elements.empty() ) {
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // 批量修改工具
        if ( ImGui::TreeNode("批量修改工具##BulkTools") ) {
            // 批量偏移
            static double bulkOffsetValue = 0.0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("批量时间偏移 (秒):");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkOffsetInput", &bulkOffsetValue, 0.001, 0.01, "%.3f");
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("应用时间偏移") &&
                 std::abs(bulkOffsetValue) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    entt::entity ent    = getElementEntity(el);
                    double       rawVal = getElementRawValue(el);
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                            ent, el.time + bulkOffsetValue, rawVal }));
                }
                bulkOffsetValue = 0.0;
            }

            // 批量缩放
            static double bulkScaleValue = 1.0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("批量流速缩放倍率:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkScaleInput", &bulkScaleValue, 0.01, 0.1, "%.2f");
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("应用流速缩放") &&
                 std::abs(bulkScaleValue - 1.0) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    if ( el.effects & Logic::System::SCROLL_EFFECT_SCROLL ) {
                        double dispScroll =
                            getDisplayValue(::MMM::TimingEffect::SCROLL,
                                            el.scrollValue,
                                            el.scrollEntity);
                        double newDisp = dispScroll * bulkScaleValue;
                        double newVal =
                            getStoredValue(::MMM::TimingEffect::SCROLL,
                                           newDisp,
                                           el.scrollEntity);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    el.scrollEntity, el.time, newVal }));
                    }
                }
                bulkScaleValue = 1.0;
            }

            ImGui::TreePop();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 渲染表格
        const float tableScrollbarSize = std::max(
            ImGui::GetStyle().ScrollbarSize,
            std::floor(TIMING_TABLE_SCROLLBAR_SIZE * std::max(dpiScale, 1.0f)));
        const float tableScrollbarGrabMinSize =
            std::max(ImGui::GetStyle().GrabMinSize,
                     std::floor(TIMING_TABLE_SCROLLBAR_GRAB_MIN_SIZE *
                                std::max(dpiScale, 1.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, tableScrollbarSize);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,
                            tableScrollbarGrabMinSize);
        if ( ImGui::BeginTable(
                 "TimingPointsTableMainV3",
                 7,
                 ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                     ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                 ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing())) ) {
            const ImGuiTableColumnFlags initialColumnFlags =
                ImGuiTableColumnFlags_WidthFixed;
            ImGui::TableSetupColumn(
                "序号", initialColumnFlags, TIMING_TABLE_COLUMN_MIN_WIDTHS[0]);
            ImGui::TableSetupColumn("时间戳 (秒)",
                                    initialColumnFlags,
                                    TIMING_TABLE_COLUMN_MIN_WIDTHS[1]);
            ImGui::TableSetupColumn(
                "拍号", initialColumnFlags, TIMING_TABLE_COLUMN_MIN_WIDTHS[2]);
            ImGui::TableSetupColumn("分拍位",
                                    initialColumnFlags,
                                    TIMING_TABLE_COLUMN_MIN_WIDTHS[3]);
            ImGui::TableSetupColumn(
                "类型", initialColumnFlags, TIMING_TABLE_COLUMN_MIN_WIDTHS[4]);
            ImGui::TableSetupColumn(
                "数值", initialColumnFlags, TIMING_TABLE_COLUMN_MIN_WIDTHS[5]);
            ImGui::TableSetupColumn(
                "操作", initialColumnFlags, TIMING_TABLE_COLUMN_MIN_WIDTHS[6]);
            if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                table->DisableDefaultContextMenu = true;
            }
            ImGui::TableHeadersRow();
            renderTimingTableHeaderContextMenu();

            const int scrollTargetIndex =
                m_tableScrollToCurrentTimePending
                    ? findNearestTimelineElementIndex(elements,
                                                      m_tableScrollTargetTime)
                    : -1;
            if ( m_tableScrollToCurrentTimePending && scrollTargetIndex < 0 ) {
                m_tableScrollToCurrentTimePending = false;
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(elements.size()));
            if ( scrollTargetIndex >= 0 ) {
                clipper.IncludeItemByIndex(scrollTargetIndex);
            }
            while ( clipper.Step() ) {
                for ( int idx = clipper.DisplayStart; idx < clipper.DisplayEnd;
                      ++idx ) {
                    const auto&         el         = elements[idx];
                    int                 displayIdx = idx + 1;
                    ::MMM::TimingEffect effect     = getElementEffect(el);
                    entt::entity        ent        = getElementEntity(el);
                    const bool          isKeepSpeedBindingRow =
                        isKeepSpeedBindingEntity(ent);
                    bool isRecentlyCreated =
                        (ImGui::GetTime() <=
                         m_lastCreatedTimingHighlightUntil) &&
                        (effect == m_lastCreatedTimingEffect) &&
                        (std::abs(el.time - m_lastCreatedTimingTime) <= 1e-6);

                    ImGui::TableNextRow();
                    if ( m_tableScrollToCurrentTimePending &&
                         idx == scrollTargetIndex ) {
                        ImGui::SetScrollHereY(0.5f);
                        m_tableScrollToCurrentTimePending = false;
                    }
                    if ( isKeepSpeedBindingRow ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(180, 225, 255, 115));
                    } else if ( isRecentlyCreated ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(255, 245, 170, 95));
                    }

                    // 第 0 列：序号
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("#%d", displayIdx);

                    // 第 1 列：时间戳（秒）
                    ImGui::TableSetColumnIndex(1);
                    double tVal = el.time;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string tId = fmt::format("##T_{}", displayIdx);
                    if ( drawTimeEditor(
                             tId.c_str(), tVal, m_currentSnapshot) ) {
                        entt::entity ent    = getElementEntity(el);
                        double       rawVal = getElementRawValue(el);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, tVal, rawVal }));
                    }

                    const double continuousBeat = timingTableTimeToBeat(
                        beatTimeline, el.time, tableFallbackBpm);
                    const auto fractionFit =
                        fitTimingTableFractionWithError(continuousBeat,
                                                        el.time,
                                                        beatTimeline,
                                                        tableFallbackBpm);

                    // 第 2 列：拍号
                    ImGui::TableSetColumnIndex(2);
                    int beatIndexValue = fractionFit.beatIndex;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string beatId = fmt::format("##Beat_{}", displayIdx);
                    ImGui::InputInt(beatId.c_str(), &beatIndexValue, 1, 4);
                    if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                        publishTimingBeatPositionUpdate(ent,
                                                        beatIndexValue,
                                                        fractionFit.fraction,
                                                        getElementRawValue(el),
                                                        beatTimeline,
                                                        tableFallbackBpm);
                    }

                    // 第 3 列：分拍位
                    ImGui::TableSetColumnIndex(3);
                    double      fractionValue = fractionFit.fraction;
                    std::string fractionId =
                        fmt::format("##BeatFrac_{}", displayIdx);
                    const bool hasFractionWarning =
                        fractionFit.errorMs >= TIMING_TABLE_FRACTION_WARNING_MS;
                    if ( hasFractionWarning ) {
                        ImGui::PushStyleColor(
                            ImGuiCol_FrameBg,
                            ImVec4(0.50f, 0.17f, 0.12f, 0.38f));
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(1.0f, 0.76f, 0.42f, 1.0f));
                    }
                    if ( drawTimingTableFractionInput(
                             fractionId.c_str(), fractionFit, fractionValue) ) {
                        publishTimingBeatPositionUpdate(ent,
                                                        fractionFit.beatIndex,
                                                        fractionValue,
                                                        getElementRawValue(el),
                                                        beatTimeline,
                                                        tableFallbackBpm);
                    }
                    if ( hasFractionWarning ) {
                        if ( ImGui::IsItemHovered() ) {
                            ImGui::SetTooltip("分拍位拟合误差 %.3f ms",
                                              fractionFit.errorMs);
                        }
                        ImGui::PopStyleColor(2);
                    }

                    // 第 4 列：类型
                    ImGui::TableSetColumnIndex(4);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          getEffectColor(effect));
                    ImGui::TextUnformatted(getEffectLabel(effect));
                    ImGui::PopStyleColor();

                    // 第 5 列：数值
                    ImGui::TableSetColumnIndex(5);
                    double vVal =
                        getDisplayValue(effect, getElementRawValue(el), ent);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string vId = fmt::format("##V_{}", displayIdx);
                    const bool  isBoundBpm =
                        m_keepSpeedBindingActive &&
                        ent == m_keepSpeedBindingBpmEntity &&
                        effect == ::MMM::TimingEffect::BPM;
                    const bool isBoundScroll =
                        m_keepSpeedBindingActive &&
                        ent == m_keepSpeedBindingScrollEntity &&
                        effect == ::MMM::TimingEffect::SCROLL;
                    if ( isBoundBpm && m_keepSpeedBindingFocusBpm ) {
                        ImGui::SetKeyboardFocusHere();
                        m_keepSpeedBindingFocusBpm = false;
                    }
                    if ( isBoundScroll ) {
                        ImGui::BeginDisabled();
                    }
                    ImGui::InputDouble(
                        vId.c_str(),
                        &vVal,
                        effect == ::MMM::TimingEffect::BPM ? 0.1 : 0.01,
                        effect == ::MMM::TimingEffect::BPM ? 1.0 : 0.1,
                        effect == ::MMM::TimingEffect::BPM ? "%.2f" : "%.4f");
                    if ( isBoundScroll ) {
                        ImGui::EndDisabled();
                        if ( ImGui::IsItemHovered(
                                 ImGuiHoveredFlags_AllowWhenDisabled) ) {
                            ImGui::SetTooltip(
                                "保持画布速度联动中，修改 BPM 后自动刷新");
                        }
                    }
                    if ( isBoundBpm && ImGui::IsItemEdited() ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                        updateKeepSpeedBindingScroll(vVal);
                    }
                    if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                        if ( isBoundBpm ) {
                            updateKeepSpeedBindingScroll(vVal);
                            finishKeepSpeedBinding();
                        }
                    } else if ( isBoundBpm && ImGui::IsItemDeactivated() ) {
                        finishKeepSpeedBinding();
                    }

                    // 第 6 列：操作
                    ImGui::TableSetColumnIndex(6);
                    std::string seekId =
                        fmt::format("跳转##Seek_{}", displayIdx);
                    if ( ::MMM::UI::FeedbackButton(seekId.c_str()) ) {
                        float visualOffset = Config::AppConfig::instance()
                                                 .getVisualConfig()
                                                 .getEffectiveVisualOffset();
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdSeek{ el.time - visualOffset }));
                    }
                    ImGui::SameLine();
                    std::string delId = fmt::format("删除##Del_{}", displayIdx);
                    if ( ::MMM::UI::FeedbackButton(delId.c_str()) ) {
                        entt::entity ent = getElementEntity(el);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdDeleteTimelineEvent{ ent }));
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::Canvas
