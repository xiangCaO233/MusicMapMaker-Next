/// @file TimelineCanvas_Popups.cpp
/// @brief 实现时间点创建/编辑弹窗及独立批量编辑表格窗口。
///
/// 本文件负责把 Timing 秒时间、连续拍位、分数拍位与 Malody beat metadata
/// 互相转换，并将所有用户提交封装为逻辑命令。表格数据具有独立快照来源，
/// 不依赖 Timeline 主窗口是否显示，也不直接消费主画布或批注表状态。
///
/// 弹窗和表格只在提交瞬间读取少量会话元数据；常规绘制使用准备好的快照。
/// 所有数值解析通过无异常 helper 完成，BPM 统一使用合法边界钳制语义。
/// 表格列显隐、局部滚动、搜索和窗口恢复均保持各自独立 UI 状态。
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "canvas/AuxiliaryWindowUi.h"
#include "canvas/TimelineCanvas.h"
#include "canvas/TimelineTableSnapshotState.h"
#include "canvas/TimelineTableWindowState.h"
#include "canvas/TimingTableFraction.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <imgui_internal.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 新建 Timing 行的高亮持续时间（秒）
constexpr double NEW_TIMING_HIGHLIGHT_DURATION = 3.0;

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

/// @brief 时间点表格可搜索的 Timing 属性数量。
constexpr std::size_t TIMING_TABLE_SEARCH_EFFECT_COUNT = 4;

/// @brief 在 Timeline 快照尚未到达时绘制可关闭的时间点表格窗口。
///
/// @details 占位窗口职责：
/// - 只在表格已请求打开但匹配快照尚未准备好时使用。
/// - 占位窗口与真实表格共用固定 ImGui 内部 ID。
/// - 因此 Dock 节点、位置、尺寸和关闭按钮状态可连续继承。
/// - 初次尺寸与真实表格默认尺寸一致。
/// - 用户仍可通过标题栏关闭等待中的窗口。
/// - 关闭反馈通过统一窗口关闭按钮入口处理。
/// - `shouldRecover` 控制窗口是否移回可达显示区域。
/// - `shouldFocus` 只请求一次下一次 Begin 聚焦。
/// - DPI 变化时可达性与圆角按当前比例重新计算。
/// - focusedAndReachable 同时考虑窗口焦点、屏幕可达性和 popup。
/// - 任意子 popup 打开时不立即丢失此前焦点状态。
/// - 内容只显示同步提示，不访问空快照字段。
/// - 本函数不主动拉取、轮询或等待逻辑线程。
/// - 快照准备仍由 UiFrameData 生命周期异步推进。
/// - 每帧返回后上层可继续渲染其它独立窗口。
/// @param windowOpen 窗口打开状态。
/// @param shouldRecover 是否需要恢复窗口位置。
/// @param shouldFocus 是否需要聚焦窗口。
/// @param focusedAndReachable 上一帧记录的聚焦可访问状态。
/// @warning UI 低频等待路径：只在打开请求早于首帧快照时短暂执行。
void renderTimingTableSyncingWindow(bool& windowOpen, bool& shouldRecover,
                                    bool& shouldFocus,
                                    bool& focusedAndReachable)
{
    // 同步占位窗口沿用用户全局圆角、内边距和元素间距偏好。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    const float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    const ImVec2 itemSpacing{
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
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

    ImGui::SetNextWindowSize(ImVec2(820.0F, 450.0F), ImGuiCond_FirstUseEver);
    // 初次尺寸与真实表格一致，快照到达后窗口不会发生明显跳变。
    if ( shouldFocus ) ImGui::SetNextWindowFocus();

    const std::string windowTitle =
        TR("ui.timeline.timing_points_table.title").toString() +
        "###TimingPointsTableWindow";
    // 翻译标题可变，固定内部 ID 保持 Dock 与 ini 状态连续。
    const bool wasOpenBeforeBegin = windowOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &windowOpen);
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin,
                                                &windowOpen);
    if ( windowOpen ) {
        // 恢复位置必须在 Begin 之后读取当前窗口，再按 DPI 判断可达范围。
        recoverCurrentAuxiliaryWindow(shouldRecover, dpiScale);
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool reachable = isCurrentAuxiliaryWindowReachable(dpiScale);
        const bool popupOpen = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        focusedAndReachable = resolveTimelineTableWindowFocusedAndReachable(
            focusedAndReachable, reachable, focused, popupOpen);
        // 子 popup 打开时保留此前焦点，避免菜单暂时夺焦导致激活逻辑误判。
    } else {
        focusedAndReachable = false;
    }
    shouldRecover = false;
    shouldFocus   = false;
    // 恢复与聚焦请求是单次脉冲，无论窗口是否成功打开都在本帧消费。
    if ( opened ) {
        ImGui::TextDisabled(
            "%s", TR("ui.timeline.timing_points_table.syncing").data());
    }
    ImGui::End();
    // 六项样式必须与入口 PushStyleVar 数量严格成对。
    ImGui::PopStyleVar(6);
}

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

/// @brief 分拍位文本输入缓存。
using TimingTableFractionInputBuffer = std::array<char, 32>;

/// @brief 按原值方向规整表格拍位换算使用的 BPM。
/// @param bpm 待规整 BPM。
/// @param fallbackBpm 原值为 NaN 时使用的回退值。
/// @return 位于安全计算范围内且可用于除法换算的 BPM。
double sanitizeTimingTableBpm(double bpm, double fallbackBpm)
{
    // 有限越界值钳制到最接近自身的合法边界，非有限值使用有效回退。
    return ::MMM::normalizeBpmValue(bpm, fallbackBpm);
}

/// @brief 取得表格拍位换算使用的快照回退 BPM。
/// @param snapshot 当前渲染快照。
/// @return 有效回退 BPM。
double timingTableFallbackBpm(const Common::Render::RenderSnapshot& snapshot)
{
    return sanitizeTimingTableBpm(snapshot.fallbackBpm, 120.0);
}

/// @brief 从当前快照构建表格拍位换算时间线。
///
/// @details 锚点构建：
/// - 输入 ScrollSegment 是渲染侧缓存，不要求表格访问 Timeline registry。
/// - 只有 BPM effect 位生效的 segment 进入候选。
/// - 非有限事件时间被忽略。
/// - 每个 BPM 值单独通过公共 normalize 入口规整。
/// - 有限低于下界的 BPM 钳制到下界。
/// - 有限高于上界的 BPM 钳制到上界。
/// - NaN 或无穷 BPM 使用规范化 fallback。
/// - fallback 本身不可用时最终回退常用 120。
/// - 候选按时间稳定排序。
/// - 相同时间候选在 epsilon 内合并。
/// - 合并时后出现 BPM 作为该点最终生效值。
/// - 第一锚点的累计 beat 定义为零。
/// - 后续锚点使用上一段 BPM 对秒差积分。
/// - 锚点数组保持分段连续语义。
/// - 结果允许从第一锚点向前外推。
/// - 结果允许从最后锚点向后外推。
/// - 空结果由正反换算 helper 使用 fallback 线性处理。
/// - 本函数不修改 snapshot 或配置。
/// - 构建结果按值持有，不保留 segment 地址。
/// - 表格同一帧的全部行复用一份结果。
/// @param snapshot 当前渲染快照。
/// @return 按时间排序并合并同时间点后的 BPM 锚点。
/// @warning UI 热路径：表格窗口打开时每帧调用，只遍历快照中的 Scroll
/// 缓存，不访问文件系统。
TimingTableBeatTimeline buildTimingTableBeatTimeline(
    const Common::Render::RenderSnapshot& snapshot)
{
    // 内部事件只保留构建锚点所需的时间和规范 BPM，不携带实体 ID。
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
        // 其它 SV、Jump、HS 分段不改变秒与 beat 的换算基准。
        if ( (segment.effects & Common::Render::SCROLL_EFFECT_BPM) == 0 ||
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
    // 稳定排序使同时间事件保持快照顺序，后出现值可稳定覆盖前值。

    TimingTableBeatTimeline timeline;
    timeline.reserve(bpmEvents.size());
    for ( const auto& event : bpmEvents ) {
        if ( !timeline.empty() && std::abs(timeline.back().time - event.time) <
                                      TIMING_TABLE_BEAT_EPSILON ) {
            // 同一时刻没有 beat 距离，只替换该锚点生效 BPM。
            timeline.back().bpm = event.bpm;
            continue;
        }

        if ( timeline.empty() ) {
            // 首锚点定义自身 beat 为零；相对换算允许从此向前外推。
            timeline.push_back({ event.time, event.bpm, 0.0 });
            continue;
        }

        const auto&  previous = timeline.back();
        const double beat =
            previous.beat + (event.time - previous.time) * previous.bpm / 60.0;
        // 新锚点累计 beat 使用上一段 BPM 对时间差积分。
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
    // 非有限输入不能进入排序查找或 metadata，安全回退连续拍零点。
    if ( !std::isfinite(time) ) return 0.0;

    const double bpm = sanitizeTimingTableBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        // 没有 BPM 事件时按规范 fallback 建立全局线性映射。
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
    // 目标早于首锚点时使用首段向前外推，否则使用时间之前最后一段。
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
    // 与正向换算对非有限值采用相同零点回退，保持往返边界一致。
    if ( !std::isfinite(beat) ) return 0.0;

    const double bpm = sanitizeTimingTableBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        // BPM 已规范为正数，可安全执行 60/BPM 反算。
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
    // beat 锚点单调时 upper_bound 取得目标之前最后一个有效分段。
    return point.time + (beat - point.beat) * 60.0 / point.bpm;
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
    // 公共拟合器先选择有限分母分数，再反算时间衡量显示近似误差。
    auto         fit        = fitTimingTableFraction(beat);
    const double fittedBeat = static_cast<double>(fit.beatIndex) + fit.fraction;
    const double fittedTime =
        timingTableBeatToTime(timeline, fittedBeat, fallbackBpm);
    fit.errorMs = std::abs(fittedTime - time) * 1000.0;
    // 误差只用于 UI 警告，不改变真实秒时间或提交值。
    return fit;
}

/// @brief 将分数拍位格式化为表格文本。
/// @param fit 分数拟合结果。
/// @return 分拍位文本。
std::string formatTimingTableFraction(const TimingTableFractionFit& fit)
{
    if ( fit.numerator == 0 ) {
        // 整拍的分拍位显示为简洁的零，不输出 0/1。
        return "0";
    }
    return fmt::format("{}/{}", fit.numerator, fit.denominator);
}

/// @brief 去除 ASCII 空白。
/// @param text 原始文本。
/// @return 去除首尾空白后的文本。
std::string_view trimTimingTableAsciiWhitespace(std::string_view text)
{
    // 只裁剪输入控件可能产生的 ASCII 空白，不尝试 Unicode 归一化。
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
    // from_chars 无区域设置、无分配且不抛异常，适合 UI 提交路径。
    text = trimTimingTableAsciiWhitespace(text);
    if ( text.empty() ) {
        return std::nullopt;
    }

    int  value = 0;
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        // 必须消费完整文本，拒绝合法前缀后附带无效字符的输入。
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析浮点文本。
/// @param text 原始文本。
/// @return 成功时返回双精度值，否则返回空。
std::optional<double> parseTimingTableDouble(std::string_view text)
{
    // SafeParse helper 返回解析长度和错误码，不使用 std::stod 异常接口。
    text = trimTimingTableAsciiWhitespace(text);
    if ( text.empty() ) {
        return std::nullopt;
    }

    const auto result = Internal::parseFloatingPrefix(text);
    if ( result.error != std::errc{} || result.parsedLength != text.size() ) {
        // 只接受完整浮点文本，尾随字符由 UI 保留等待用户修正。
        return std::nullopt;
    }
    return result.value;
}

/// @brief 解析分拍位输入文本。
/// @param text 输入文本，可为小数或 numerator/denominator。
/// @return 成功时返回分拍小数值，否则返回空。
std::optional<double> parseTimingTableFractionText(std::string_view text)
{
    // 无斜线时允许直接输入小数，带斜线时要求严格 numerator/denominator。
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
        // 分母必须为正，防止除零和符号落在分母造成显示不一致。
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
    // 固定数组复制始终为 NUL 结尾预留一字节，超长文本安全截断。
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
    // 每个 ImGuiID 维护独立短缓冲，多行同时编辑不会互相覆盖。
    static std::unordered_map<ImGuiID, TimingTableFractionInputBuffer>
        editBuffers;

    const ImGuiID inputId = ImGui::GetID(id);
    auto&         buffer  = editBuffers[inputId];
    if ( ImGui::GetActiveID() != inputId ) {
        // 非活动状态同步最新拟合值；活动输入保留用户尚未提交的文本。
        copyTimingTableTextToBuffer(buffer, formatTimingTableFraction(fit));
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(
        id, buffer.data(), buffer.size(), ImGuiInputTextFlags_CharsNoBlank);
    if ( !ImGui::IsItemDeactivatedAfterEdit() ) {
        // 只在结束编辑时解析，中间非法状态不会触发逻辑命令。
        return false;
    }

    const auto parsed = parseTimingTableFractionText(buffer.data());
    if ( !parsed || !std::isfinite(*parsed) ) {
        // 非法提交保留原业务值，下一帧恢复规范化文本。
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
///
/// @details 更新步骤：
/// - 先验证实体、fraction 和原始参数均有效。
/// - beatIndex 可以为负，最终秒时间边界负责钳制。
/// - 整数拍号与 fraction 相加形成 requestedBeat。
/// - requestedBeat 通过当前 BPM 时间线反算秒时间。
/// - 反算时间最小钳制为零。
/// - 钳制后再正算 exportBeat。
/// - exportBeat 描述最终实际保存的位置。
/// - 读取实体原 metadata 的其它字段。
/// - 只覆盖 Malody timing_properties 中的 beat。
/// - 参数值保持当前行原始存储值。
/// - 更新命令同时提交时间、参数和新 metadata。
/// - UI 不预先修改行数据或 registry。
/// - 快照回流后表格重新排序并拟合显示值。
/// - 无效输入保留原行，等待用户修正。
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
///
/// @details 列菜单约束：
/// - 只在当前 ImGui table 上下文有效时绘制。
/// - 菜单通过表头右键 popup 入口打开。
/// - 固定必要列不能被用户隐藏。
/// - 可选列读取 `IsUserEnabled` 展示当前勾选状态。
/// - 点击项只排队修改下一帧显隐状态。
/// - 当前表格布局期间不直接改 Columns 数组结构。
/// - 至少保留核心时间、类型和值列可见。
/// - 菜单标签使用翻译文本与固定内部 ID。
/// - 关闭 popup 不改变未点击列。
/// - 本函数不保存 AppConfig，列设置由 ImGui table ini 管理。
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
/// @details 按 BPM、Scroll、Jump、HS 的既定优先级选择元素代表类型；
/// 表格收集阶段已拆分复合元素，回退类型仅用于防御异常快照。
::MMM::TimingEffect getElementEffect(
    const Common::Render::TimelineInteractiveElement& el)
{
    if ( el.effects & Common::Render::SCROLL_EFFECT_BPM ) {
        return ::MMM::TimingEffect::BPM;
    }
    if ( el.effects & Common::Render::SCROLL_EFFECT_JUMP ) {
        return ::MMM::TimingEffect::JUMP;
    }
    if ( el.effects & Common::Render::SCROLL_EFFECT_HS ) {
        return ::MMM::TimingEffect::HS;
    }
    return ::MMM::TimingEffect::SCROLL;
}

/// @brief 获取 Timing 类型对应实体
/// @param element Timeline 交互元素。
/// @param effect 目标 Timing 类型。
/// @return 对应实体；未知类型返回 `entt::null`。
/// @details UI 只传递实体 ID，不在此访问 registry。
entt::entity getElementEntity(
    const Common::Render::TimelineInteractiveElement& el)
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
/// @param element Timeline 交互元素。
/// @param effect 目标 Timing 类型。
/// @return 对应存储参数；未知类型返回零。
/// @details 显示值转换由独立 helper 处理，不能在此混入单位格式化。
double getElementRawValue(const Common::Render::TimelineInteractiveElement& el)
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
/// @param effect Timing 类型。
/// @return 稳定的 BPM、SV、Jump 或 HS 短标签。
/// @details 短标签用于窄表格列，未知值回退通用 Timing 文本。
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

/// @brief 获取 Timing 类型在表格搜索属性数组中的索引。
/// @param effect Timing 类型。
/// @return 与四个搜索复选框一致的零基索引。
/// @details 映射顺序必须与翻译键和控件 ID 数组保持一致。
/// @param effect Timing 类型。
/// @return 对应搜索属性索引。
std::size_t getTimingTableSearchEffectIndex(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 判断表格行数值是否精确匹配搜索值。
/// @param value 行显示值。
/// @param searchValue 已解析搜索值。
/// @return 在相对/绝对混合容差内相等时返回 true。
/// @details 容差吸收显示到 double 的往返误差，但不进行模糊区间搜索。
/// @param value 表格行显示值。
/// @param expected 用户输入的搜索值。
/// @return 在浮点比例容差内相等时返回 true。
bool timingTableSearchValueEquals(double value, double expected)
{
    if ( !std::isfinite(value) || !std::isfinite(expected) ) {
        return false;
    }
    const double scale = std::max({ 1.0, std::abs(value), std::abs(expected) });
    return std::abs(value - expected) <= 1e-9 * scale;
}

/// @brief 获取 Timeline UI 中展示用的类型颜色
/// @param effect Timing 类型。
/// @return 与画布 marker 语义一致的 ImVec4 颜色。
/// @details 颜色只用于文字与高亮，不影响存储类型或筛选逻辑。
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
/// @param snapshot 当前时间线快照。
/// @return 能稳定区分活动谱面的字符串视图。
/// @details key 引用快照内部字符串，仅在当前调用与比较期间有效。
std::string_view getTimingPointsTableBeatmapKey(
    const Common::Render::RenderSnapshot& snapshot)
{
    if ( !snapshot.beatmapPathKey.empty() ) {
        return snapshot.beatmapPathKey;
    }
    return snapshot.beatmapName;
}

/// @brief 判定 Timeline 表格当前取得的快照是否属于活动谱面。
///
/// @details 状态判定：
/// - 无活动 Session 返回 Close。
/// - Logo 占位会话返回 Close。
/// - Session 正在切换谱面且快照为空时返回 AwaitingSnapshot。
/// - 快照存在但无谱面时根据活动上下文判断等待或关闭。
/// - 快照 beatmap key 与活动谱面不匹配时等待新代际。
/// - key 匹配且快照有效时返回 Ready。
/// - 判定只比较身份，不准备或消费快照。
/// - 调用方在 session mutex 保护下取得活动会话指针。
/// @param activeSession 当前活动会话观察指针。
/// @param snapshot Timeline 当前快照观察指针。
/// @return 无活动谱面时关闭，快照尚未追上时等待，身份一致时可绘制。
/// @warning UI 热路径：每个打开的表格每帧调用，不复制共享所有权。
TimelineTableSnapshotStatus getTimelineTableSnapshotStatus(
    const Logic::BeatmapSession*          activeSession,
    const Common::Render::RenderSnapshot* snapshot)
{
    const auto* activeBeatmap =
        activeSession ? activeSession->getContext().currentBeatmap.get()
                      : nullptr;
    return resolveTimelineTableSnapshotStatus(
        activeBeatmap != nullptr,
        reinterpret_cast<std::uintptr_t>(activeBeatmap),
        snapshot != nullptr,
        snapshot && snapshot->hasBeatmap,
        snapshot ? snapshot->beatmapInstanceId : 0);
}

/// @brief 从当前 Session 收集完整 Timing 列表，供表格窗口编辑使用。
///
/// @details 收集规则：
/// - 短暂锁定活动 Session 的递归互斥量。
/// - 无活动 Session 或 BeatMap 时返回空列表。
/// - 遍历 Timeline registry 的有效组件视图。
/// - 每个实体生成独立表格元素。
/// - 时间、effect、value 和 metadata 从组件按值复制。
/// - 不依赖 RenderSnapshot 的视口裁剪列表。
/// - 元素按时间升序稳定排序。
/// - 同时间元素按 effect 顺序排序。
/// - 相同时间和类型仍保留各自实体行。
/// - 释放锁后 ImGui 只处理值语义副本。
/// - 本函数不修改 registry 或选择集合。
/// - 完整列表只在表格窗口打开时构建。
std::vector<Common::Render::TimelineInteractiveElement>
collectTimelineElements()
{
    std::vector<Common::Render::TimelineInteractiveElement> elements;
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) return elements;

    auto& registry = session->getContext().timelineRegistry;
    auto  view     = registry.view<const Logic::TimelineComponent>();
    elements.reserve(view.size());

    for ( auto entity : view ) {
        const auto& tc = view.get<const Logic::TimelineComponent>(entity);
        Common::Render::TimelineInteractiveElement el;
        el.time = tc.m_timestamp;
        el.y    = 0.0f;

        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            el.effects   = Common::Render::SCROLL_EFFECT_BPM;
            el.bpmEntity = entity;
            el.bpmValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::SCROLL ) {
            el.effects      = Common::Render::SCROLL_EFFECT_SCROLL;
            el.scrollEntity = entity;
            el.scrollValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::JUMP ) {
            el.effects    = Common::Render::SCROLL_EFFECT_JUMP;
            el.jumpEntity = entity;
            el.jumpValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::HS ) {
            el.effects  = Common::Render::SCROLL_EFFECT_HS;
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

/// @brief 将时间点表格当前选中行写入编辑器级 Timeline 剪贴板。
///
/// @details 复制规则：
/// - 只收集实体位于选择集合中的表格元素。
/// - 空选择或无有效目标返回 false。
/// - 目标按表格稳定顺序写入剪贴板。
/// - 第一项时间作为相对秒锚点。
/// - 第一项 beat 作为相对拍位锚点。
/// - 每项保存 TimelineComponent 的 effect、value 和 metadata。
/// - relativeTime 保存原始秒间隔。
/// - relativeBeat 保存目标 BPM 时间线下的节奏间隔。
/// - hasBeatPosition 标记该条目可按 beat 粘贴。
/// - 剪贴板按当前 SessionContext 隔离。
/// - 成功写入后同步导出到系统剪贴板桥接格式。
/// - 本函数不删除原 Timing，Cut 由调用方在成功后执行。
/// - 本函数不改变当前行选择。
/// @param elements 已按时间排序的完整 Timing 行。
/// @param selectedEntities 当前选中的 Timing 实体集合。
/// @param beatTimeline 当前谱面的连续拍位时间线。
/// @param fallbackBpm 无 BPM 锚点时使用的回退 BPM。
/// @return 至少复制一行时返回 true。
/// @warning UI 快捷键低频路径：仅在复制或剪切时短暂持有 Session 锁，
/// 遍历完整表格行并复制选中 Timing 元数据。
bool copyTimingTableSelectionToClipboard(
    const std::vector<Common::Render::TimelineInteractiveElement>& elements,
    const std::unordered_set<entt::entity>& selectedEntities,
    const TimingTableBeatTimeline& beatTimeline, double fallbackBpm)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) {
        return false;
    }

    auto& registry = session->getContext().timelineRegistry;
    std::vector<Logic::TimelineClipboardItem> clipboard;
    clipboard.reserve(selectedEntities.size());
    for ( const auto& element : elements ) {
        const entt::entity entity = getElementEntity(element);
        if ( !selectedEntities.contains(entity) || !registry.valid(entity) ||
             !registry.all_of<Logic::TimelineComponent>(entity) ) {
            continue;
        }

        Logic::TimelineClipboardItem entry;
        entry.timeline = registry.get<const Logic::TimelineComponent>(entity);
        clipboard.push_back(std::move(entry));
    }
    if ( clipboard.empty() ) {
        return false;
    }

    const double anchorTime = clipboard.front().timeline.m_timestamp;
    const double anchorBeat =
        timingTableTimeToBeat(beatTimeline, anchorTime, fallbackBpm);
    for ( auto& entry : clipboard ) {
        entry.relativeTime = entry.timeline.m_timestamp - anchorTime;
        entry.relativeBeat =
            timingTableTimeToBeat(
                beatTimeline, entry.timeline.m_timestamp, fallbackBpm) -
            anchorBeat;
        entry.hasBeatPosition = true;
    }

    engine.setTimelineClipboard(
        std::move(clipboard), &session->getContext(), false);
    return true;
}

/// @brief 读取当前活跃谱面的动画时间。
/// @param fallbackTime 快照时间不可用时的回退值。
/// @return 活动 Session timelineTime 的有限值，否则返回回退。
/// @details 读取时短暂持有 session mutex，不在锁内绘制或发布命令。
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

/// @brief 查找可见行中最靠近目标时间的 Timing 行索引。
/// @details visibleIndices 保持原 elements 时间顺序，因此可直接 lower_bound；
/// 目标位于范围外时返回首尾行，位于两行之间时比较绝对时间差，
/// 等距优先前一行以保持重复定位稳定。
/// @param elements 已按时间排序的 Timing 元素列表。
/// @param visibleIndices 当前筛选后可见行对应的原始索引。
/// @param targetTime 目标判定线时间，单位秒。
/// @return 命中的可见行索引；无可用行时返回 -1。
int findNearestTimelineElementIndex(
    const std::vector<Common::Render::TimelineInteractiveElement>& elements,
    const std::vector<std::size_t>& visibleIndices, double targetTime)
{
    if ( visibleIndices.empty() || !std::isfinite(targetTime) ) {
        return -1;
    }

    auto next = std::lower_bound(visibleIndices.begin(),
                                 visibleIndices.end(),
                                 targetTime,
                                 [&](std::size_t elementIndex, double time) {
                                     return elements[elementIndex].time < time;
                                 });
    if ( next == visibleIndices.begin() ) {
        return 0;
    }
    if ( next == visibleIndices.end() ) {
        return static_cast<int>(visibleIndices.size() - 1U);
    }

    const auto prev      = std::prev(next);
    const auto prevDelta = std::abs(elements[*prev].time - targetTime);
    const auto nextDelta = std::abs(elements[*next].time - targetTime);
    return static_cast<int>((prevDelta <= nextDelta ? prev : next) -
                            visibleIndices.begin());
}

/// @brief 将存储值转换成编辑器显示值
/// @details 当前所有 Timing 类型显示值与存储值相同；保留独立入口用于未来
/// 单位换算，并确保表格、弹窗和搜索统一使用同一语义。
/// @warning UI 热路径：表格绘制时逐行调用，只做常量时间数值归一化。
double getDisplayValue(::MMM::TimingEffect, double rawValue,
                       entt::entity = entt::null)
{
    return rawValue;
}

/// @brief 将编辑器显示值转换成存储值
/// @details 当前为恒等转换；提交路径仍必须经过该入口，避免未来单位扩展时
/// 行内编辑与创建/修改弹窗出现不一致。
/// @warning UI 热路径：用户提交编辑值时调用，不应访问文件系统或执行重型同步。
double getStoredValue(::MMM::TimingEffect, double displayValue,
                      entt::entity = entt::null)
{
    return displayValue;
}

/// @brief 判断 Timeline 编辑值是否满足界面提交约束。
/// @details 所有类型要求有限；BPM 额外禁止负数，零值按现有产品语义允许，
/// 最终 BPM 规范化和边界处理仍由逻辑层公共入口负责。
/// @param effect Timeline 类型。
/// @param value 待提交的显示值。
/// @return 数值有限，且 BPM 不小于零时返回 true。
/// @warning UI 热路径：仅执行常量时间数值判断，不得增加状态查询。
bool isValidTimingEditorValue(::MMM::TimingEffect effect, double value)
{
    // 其他特效沿用既有有符号值语义，只有 BPM 禁止负数。
    return std::isfinite(value) &&
           (effect != ::MMM::TimingEffect::BPM || value >= 0.0);
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
/// @details 优先读取活动谱面 preference_bpm，并通过统一 BPM 规范化；
/// 无会话或无谱面时使用全局默认 BPM，返回值始终可安全作为除法分子。
double getKeepSpeedReferenceBpm()
{
    double refBpm = ::MMM::DEFAULT_NORMALIZED_BPM;
    if ( auto session = Logic::EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            refBpm = ::MMM::normalizeBpmValue(
                beatmap->m_baseMapMetadata.preference_bpm);
        }
    }
    return refBpm;
}

/// @brief 根据新 BPM 计算保持画布下落速度所需的流速存储值。
/// @details 目标 Scroll 等于参考 BPM 除以规范化新 BPM，使 BPM 与 Scroll
/// 乘积保持参考速度；结果异常或过小则回退单位流速。
double getKeepSpeedScrollValue(double bpm)
{
    double refBpm      = getKeepSpeedReferenceBpm();
    double safeBpm     = ::MMM::normalizeBpmValue(bpm, refBpm);
    double scrollSpeed = refBpm / safeBpm;
    return scrollSpeed > 1e-6 ? scrollSpeed : 1.0;
}

/// @brief 创建与新 BPM 同时间点的保持速度流速事件。
/// @details 只计算最终 Scroll 参数并发布创建命令；实体关联由快照回流后的
/// keep-speed binding 按时间和最新实体 ID 完成。
void createKeepSpeedScrollEvent(double time, double bpm)
{
    double finalScrollValue = getKeepSpeedScrollValue(bpm);
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
            time, ::MMM::TimingEffect::SCROLL, finalScrollValue }));
}

/// @brief 绘制按偏好格式显示、仍可编辑原始秒值的时间输入控件。
///
/// @details 显示语义：
/// - Seconds 偏好直接显示 `InputDouble`。
/// - 其它偏好显示格式化时间按钮。
/// - 按钮文本通过公共 `formatCanvasTime` 生成。
/// - 按钮可见标签与 `##id` 内部标识组合。
/// - 悬停按钮显示相同格式的 tooltip。
/// - 点击按钮打开以传入 id 命名的轻量 popup。
/// - popup 内始终编辑原始秒值，不解析格式化文本。
/// - 秒输入普通步进为 0.001，快速步进为 0.01。
/// - 输入显示四位小数但内部保留 double 精度。
/// - 返回值只表示本帧数值实际变化。
/// - 本 helper 不钳制负时间或总时长。
/// - 调用方负责最终业务范围和提交时机。
/// - 所有可见按钮使用统一 FeedbackButton。
/// - popup 状态由 ImGui ID 隔离，多处时间字段不会冲突。
/// @return 文本或步进按钮在当前帧改变秒值时返回 true。
bool drawTimeEditor(const char* id, double& value,
                    const Common::Render::RenderSnapshot* snapshot)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Seconds ) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        return ImGui::InputDouble(id, &value, 0.001, 0.01, "%.4f");
    }

    std::string label =
        MMM::UI::Utils::formatCanvasTime(value, snapshot) + "##" + id;
    if ( ::MMM::UI::FeedbackButton(label.c_str(), ImVec2(-FLT_MIN, 0.0f)) ) {
        ImGui::OpenPopup(id);
    }
    if ( ImGui::IsItemHovered() ) {
        const auto timeText = MMM::UI::Utils::formatCanvasTime(value, snapshot);
        ImGui::SetTooltip("%s", timeText.c_str());
    }

    bool changed = false;
    if ( ImGui::BeginPopup(id) ) {
        ImGui::SetNextItemWidth(180.0f);
        changed = ImGui::InputDouble("##Seconds", &value, 0.001, 0.01, "%.4f");
        ImGui::EndPopup();
    }
    return changed;
}

/// @brief 按表格相同的拍号与分拍位语义编辑弹窗秒时间。
///
/// @details 基准与拟合：
/// - 无快照时不绘制拍位控件。
/// - modal 首次出现时构建一次 BPM 连续 beat 时间线。
/// - 同时冻结规范化 fallback BPM。
/// - modal 打开期间不重复排序 BPM 分段。
/// - 冻结避免编辑 BPM 参数时定位基准随草稿反馈。
/// - 当前秒时间先换算为连续 beat。
/// - 连续 beat 再拟合为整数拍号和有限分母分数。
/// - 拟合误差通过反算秒时间获得。
/// - 误差以毫秒显示但不自动改写原时间。
///
/// @details 输入与提交：
/// - 拍号使用整数输入并支持普通/快速步进。
/// - 分拍位接受 `numerator/denominator` 或小数。
/// - 分拍文本只在结束编辑时解析。
/// - 非法分数保持原时间不变。
/// - 任一合法字段提交后合成为连续 beat。
/// - 连续 beat 通过冻结时间线反算秒时间。
/// - 非有限反算结果被忽略。
/// - 有限结果最小钳制到零。
/// - 只有显式字段编辑才写回 `time` 引用。
/// - 因此只打开弹窗不会因拟合近似改变时间。
/// - tooltip 同时说明输入格式与当前拟合误差。
/// - helper 不发布逻辑命令，外层 Apply/Create 再提交。
/// @param time 弹窗草稿时间；仅显式编辑拍位时写回，避免拟合改变原时间。
/// @param snapshot 当前谱面快照。
/// @warning UI 热路径：仅在弹窗首次显示时构建 BPM 换算基准；打开期间复用
/// 基准，避免每帧排序，也避免编辑 BPM 自身时定位基准随草稿变化。
void drawPopupBeatPositionEditor(double&                               time,
                                 const Common::Render::RenderSnapshot* snapshot)
{
    if ( !snapshot ) return;

    /// @brief 当前模态弹窗打开时捕获的谱面节奏定位基准。
    static TimingTableBeatTimeline timeline;
    /// @brief 与定位基准一同捕获的缺省 BPM。
    static double fallbackBpm = 120.0;
    if ( ImGui::IsWindowAppearing() ) {
        timeline    = buildTimingTableBeatTimeline(*snapshot);
        fallbackBpm = timingTableFallbackBpm(*snapshot);
    }

    const auto fit = fitTimingTableFractionWithError(
        timingTableTimeToBeat(timeline, time, fallbackBpm),
        time,
        timeline,
        fallbackBpm);
    int    beatIndex = fit.beatIndex;
    double fraction  = fit.fraction;
    ImGui::TextUnformatted("拍号");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool beatChanged = ImGui::InputInt("##PopupBeat", &beatIndex, 1, 4);
    ImGui::TextUnformatted("分拍位");
    const bool fractionChanged =
        drawTimingTableFractionInput("##PopupBeatFraction", fit, fraction);
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("支持分数（如 1/4）或小数；拟合误差 %.3f ms",
                          fit.errorMs);
    }
    if ( beatChanged || fractionChanged ) {
        const double requestedTime = timingTableBeatToTime(
            timeline, static_cast<double>(beatIndex) + fraction, fallbackBpm);
        if ( std::isfinite(requestedTime) ) {
            time = std::max(0.0, requestedTime);
        }
    }
}

/// @brief 绘制占满弹窗内容区宽度的双精度输入框。
/// @details 先把下一控件宽度设为剩余内容区，再绘制 InputDouble；只有控件
/// 结束编辑的帧返回 true，调用方可据此避免逐键发布逻辑更新。
/// @warning UI 热路径：仅写入 ImGui 下一控件宽度并绘制输入框。
bool drawFullWidthInputDouble(const char* id, double& value, double step,
                              double stepFast, const char* format)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputDouble(id, &value, step, stepFast, format);
    return ImGui::IsItemDeactivatedAfterEdit();
}

/// @brief 计算横向按钮行的等宽按钮宽度。
/// @details 从剩余内容宽度扣除按钮间距后等分，并以 minWidth 为下限；
/// buttonCount 非正时直接返回下限，避免除零。
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
/// @details 创建命令尚未回流实体 ID，因此先以时间建立临时 binding，清空
/// 两类实体并请求首次识别后聚焦 BPM 行。
void TimelineCanvas::beginKeepSpeedBinding(double time)
{
    m_keepSpeedBindingActive       = true;
    m_keepSpeedBindingTime         = time;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = true;
}

/// @brief 刷新当前“保持画布速度”联动关联的实体。
///
/// @details 关联规则：
/// - binding 未激活时立即返回。
/// - 只检查与创建时间在微秒容差内相同的元素。
/// - BPM effect 提供 BPM 实体候选。
/// - Scroll effect 提供联动流速实体候选。
/// - null 实体不会覆盖已有候选。
/// - 首个有效实体直接成为候选。
/// - 同类型多个实体选择 integral ID 更大的最新项。
/// - 该规则匹配创建命令通常追加新实体的顺序。
/// - 已找到实体可跨帧保留，直到 binding 结束。
/// - 本函数不修改 Timing 参数或选择集合。
/// - 完整元素来自当前表格绑定的同一谱面。
/// - 谱面切换会先关闭窗口并结束 binding。
void TimelineCanvas::refreshKeepSpeedBinding(
    const std::vector<Common::Render::TimelineInteractiveElement>& elements)
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

        if ( el.effects & Common::Render::SCROLL_EFFECT_BPM ) {
            m_keepSpeedBindingBpmEntity =
                chooseNewest(m_keepSpeedBindingBpmEntity, el.bpmEntity);
        }
        if ( el.effects & Common::Render::SCROLL_EFFECT_SCROLL ) {
            m_keepSpeedBindingScrollEntity =
                chooseNewest(m_keepSpeedBindingScrollEntity, el.scrollEntity);
        }
    }
}

/// @brief 判断表格行是否属于当前临时联动。
/// @details 只有 binding 激活、实体非空且等于已识别 BPM 或 Scroll 实体时
/// 返回 true，用于行高亮和限制联动编辑范围。
bool TimelineCanvas::isKeepSpeedBindingEntity(entt::entity entity) const
{
    return m_keepSpeedBindingActive && entity != entt::null &&
           (entity == m_keepSpeedBindingBpmEntity ||
            entity == m_keepSpeedBindingScrollEntity);
}

/// @brief 使用编辑中的 BPM 值刷新联动 Scroll 值。
/// @details 只有 binding 激活且 Scroll 实体已识别时发布更新；时间固定为
/// 创建时间，参数通过统一保持速度公式计算，UI 不直接修改 registry。
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
/// @details 同时清除激活标志、时间、BPM 实体、Scroll 实体和首次聚焦请求；
/// 重复调用安全，不会发布额外命令。
void TimelineCanvas::finishKeepSpeedBinding()
{
    m_keepSpeedBindingActive       = false;
    m_keepSpeedBindingTime         = -1.0;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = false;
}

/// @brief 渲染 Timeline 时间点编辑弹窗。
///
/// @details 弹窗状态：
/// - `m_isPopupOpen` 是编辑 modal 的唯一打开状态。
/// - 打开请求通过统一反馈入口转为 ImGui popup。
/// - modal 使用 DPI 缩放后的固定首选宽度。
/// - 居中、padding 和圆角由 `CenteredModalPopupScope` 统一管理。
/// - `m_editingEntity` 标识要更新或删除的 Timing。
/// - `m_editTime` 和 `m_editValue` 是本地草稿。
/// - `m_editType` 只用于选择可见字段和 TimingEffect。
/// - 弹窗关闭后不保留未提交草稿到谱面。
/// - 播放中保留阅读内容但禁用全部编辑操作。
/// - 取消按钮在禁用范围外，播放中仍可关闭弹窗。
///
/// @details 时间编辑：
/// - 秒格式偏好直接显示双精度输入框。
/// - 其它格式显示格式化按钮并在子 popup 中编辑原始秒。
/// - 拍号与分拍位编辑器始终与秒时间并列提供。
/// - 弹窗首次出现时冻结 BPM/beat 换算基准。
/// - 编辑 BPM 自身时不会让定位基准随草稿值逐帧改变。
/// - 仅用户明确提交拍位字段时才反算并覆盖秒时间。
/// - 拟合误差只作为 tooltip 提示，不自动改变原时间。
/// - 反算时间最小钳制为零。
/// - 所有时间草稿保持 double 精度。
///
/// @details 参数编辑：
/// - BPM 步进为 0.1，快速步进为 1.0。
/// - Jump 以毫秒语义展示，步进为 1 和 10。
/// - HS 与 Scroll 步进为 0.01 和 0.1。
/// - Scroll 额外展示存储语义提示。
/// - BPM 可选择保持偏好 BPM 对应的画布速度。
/// - 该选项只在编辑 BPM 时可见。
/// - 非有限参数不能提交。
/// - 负 BPM 不能提交。
/// - 零 BPM 按现有界面契约仍允许进入逻辑规范化流程。
/// - 其它 TimingEffect 保留有符号值语义。
///
/// @details 提交语义：
/// - Apply、Delete、Cancel 三按钮按内容区等宽排列。
/// - Apply 先把显示值转换为存储值。
/// - 普通更新发布 `CmdUpdateTimelineEvent`。
/// - 保持速度 BPM 更新发布组合命令 `CmdUpdateBpmWithKeepSpeedSv`。
/// - 组合命令携带 BPM 实体、新时间、新 BPM 和目标 Scroll 值。
/// - Scroll 值按偏好 BPM 除以规范化新 BPM 计算。
/// - Delete 发布 `CmdDeleteTimelineEvent`。
/// - Apply 和 Delete 发布后立即关闭 modal。
/// - Cancel 不发布数据命令。
/// - UI 不直接修改 Timeline registry。
/// - 逻辑线程负责实体校验、联动更新和撤销历史。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventEditorPopup()
{
    if ( m_isPopupOpen ) {
        // 反馈入口既打开 popup，也统一播放 modal 打开音效。
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

        const bool editingDisabled =
            m_currentSnapshot && m_currentSnapshot->isPlaying;
        // 播放中只禁用内容和提交/删除，取消保持可达。
        if ( editingDisabled ) {
            ImGui::BeginDisabled();
        }

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##Time", m_editTime, m_currentSnapshot);
        drawPopupBeatPositionEditor(m_editTime, m_currentSnapshot);

        ::MMM::TimingEffect editEffect =
            (m_editType == "BPM")    ? ::MMM::TimingEffect::BPM
            : (m_editType == "Jump") ? ::MMM::TimingEffect::JUMP
            : (m_editType == "HS")   ? ::MMM::TimingEffect::HS
                                     : ::MMM::TimingEffect::SCROLL;
        // 可见字符串只作为兼容映射，内部提交统一使用强类型 effect。

        if ( editEffect == ::MMM::TimingEffect::BPM ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            drawFullWidthInputDouble("##Value", m_editValue, 0.1, 1.0, "%.4f");
            ImGui::Spacing();
            ::MMM::UI::FeedbackCheckbox(
                TR("ui.timeline.event_editor.keep_preferred_bpm_speed_sv")
                    .data(),
                &m_keepSpeedOnBpmEdit);
            if ( ImGui::IsItemHovered() ) {
                // tooltip 解释保持速度会联动同时间 Scroll，而不是改变音频速度。
                ImGui::SetTooltip(
                    "%s",
                    TR("ui.timeline.event_editor.keep_preferred_bpm_speed_sv_"
                       "tooltip")
                        .data());
            }
        } else if ( editEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble("##Value", m_editValue, 1.0, 10.0, "%.4f");
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
        // BPM 为负数时禁用应用，零值仍按用户要求作为合法输入。
        const bool editValueValid =
            isValidTimingEditorValue(editEffect, m_editValue);
        // 无效草稿只禁用 Apply，Delete 和 Cancel 仍应可用。
        if ( !editValueValid ) ImGui::BeginDisabled();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.apply").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            double finalValue =
                getStoredValue(editEffect, m_editValue, m_editingEntity);

            if ( editEffect == ::MMM::TimingEffect::BPM &&
                 m_keepSpeedOnBpmEdit ) {
                // 组合命令在逻辑线程原子处理 BPM 与联动 Scroll 更新。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateBpmWithKeepSpeedSv{
                        .bpmEntity   = m_editingEntity,
                        .newTime     = m_editTime,
                        .newBpm      = finalValue,
                        .scrollValue = getKeepSpeedScrollValue(m_editValue),
                    }));
            } else {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                        m_editingEntity, m_editTime, finalValue }));
            }
            ImGui::CloseCurrentPopup();
            // 本地打开标志与 ImGui popup 同帧关闭，避免下一帧重新打开。
            m_isPopupOpen = false;
        }
        if ( !editValueValid ) ImGui::EndDisabled();

        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_editor.delete").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ m_editingEntity }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        if ( editingDisabled ) {
            ImGui::EndDisabled();
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
///
/// @details 创建位置：
/// - 弹窗保存鼠标原始时间、吸附时间和当前播放时间三类来源。
/// - `m_createPosType == 0` 表示鼠标点击位置。
/// - 点击位置在发生吸附时优先使用吸附时间。
/// - `m_createPosType == 1` 表示当前播放头时间。
/// - 切换单选项时立即刷新 `m_createTimeManual` 草稿。
/// - 用户仍可通过秒输入或拍位输入覆盖草稿。
/// - 拍位换算基准在弹窗首次出现时冻结。
/// - 最终创建时间保持 double 精度。
/// - UI 不在提交前创建任何 Timing entity。
///
/// @details 创建类型：
/// - 普通模式显示 BPM、Scroll、Jump、HS 四个单选项。
/// - 专业模式的类型已由点击泳道确定，因此隐藏类型选择。
/// - 类型索引零映射 BPM。
/// - 类型索引一映射 Scroll。
/// - 类型索引二映射 Jump。
/// - 类型索引三映射 HS。
/// - 未知索引安全回退 Scroll。
/// - 切换类型时重置为该类型默认参数。
/// - 单选项根据剩余宽度自动同行或换行。
/// - 宽度计算包含圆形控件、文字、间距和 frame padding。
///
/// @details 参数与校验：
/// - BPM 默认值为 120。
/// - Scroll 默认值为单位倍率 1。
/// - Jump 默认值为 1000 毫秒。
/// - HS 默认值为单位倍率 1。
/// - 输入步长与编辑弹窗保持一致。
/// - BPM 可选“保持画布速度”联动创建 Scroll。
/// - 非有限值不能创建。
/// - 负 BPM 不能创建。
/// - 其它效果保留有符号参数语义。
/// - 无效草稿只禁用创建按钮，取消始终可用。
///
/// @details 创建提交：
/// - 显示值先通过 `getStoredValue` 转换。
/// - 单个事件发布 `CmdCreateTimelineEvent`。
/// - 记录最近创建时间和效果用于表格短时高亮。
/// - 高亮截止时间使用 ImGui 单调时间加三秒。
/// - BPM 保持速度时额外创建同时间 Scroll 事件。
/// - 联动 Scroll 值按偏好 BPM 除以新 BPM 计算。
/// - 随后开启临时 keep-speed binding 追踪新实体。
/// - 创建命令发布后立即关闭 modal。
/// - Cancel 不发布任何创建命令。
/// - 逻辑快照回流后表格负责识别新实体并完成联动绑定。
/// - UI 不等待命令完成，也不直接访问 registry 创建对象。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventCreationPopup()
{
    if ( m_isCreatePopupOpen ) {
        // 每帧保持 OpenPopup 请求直到 modal 成功进入，兼容首次布局延迟。
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

        const bool editingDisabled =
            m_currentSnapshot && m_currentSnapshot->isPlaying;
        if ( editingDisabled ) {
            ImGui::BeginDisabled();
        }

        // 自动计算下一项 RadioButton 宽度并在空间充足时在同行显示的辅助函数
        auto getRadioButtonWidth = [](const char* label) -> float {
            // 预估宽度用于决定 SameLine，不创建隐藏测量控件。
            ImGuiStyle& style      = ImGui::GetStyle();
            float       circleSize = ImGui::GetFrameHeight();
            float       textWidth  = ImGui::CalcTextSize(label).x;
            return circleSize + style.ItemSpacing.x + textWidth +
                   style.FramePadding.x * 2.0f;
        };

        auto wrapToNextLineIfNoSpace = [&](float nextItemWidth) {
            // 当前行容纳得下才 SameLine，否则让 ImGui 自然换到下一行。
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
            // 选择点击来源时恢复最初捕获的原始或吸附候选。
            m_createTimeManual =
                m_isTimeSnapped ? m_createTimeSnapped : m_createTimeRaw;
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth(posCurrentLabel.c_str()));

        if ( ::MMM::UI::FeedbackRadioButton(
                 posCurrentLabel.c_str(), &m_createPosType, 1) ) {
            // 当前时间在切换时取快照值，之后允许用户手动继续编辑。
            m_createTimeManual = m_currentSnapshot->currentTime;
        }

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##CreateTime", m_createTimeManual, m_currentSnapshot);
        drawPopupBeatPositionEditor(m_createTimeManual, m_currentSnapshot);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool professionalMode =
            Config::AppConfig::instance().getEditorSettings().professionalMode;
        if ( !professionalMode ) {
            // 专业模式类型由泳道决定，避免弹窗类型与点击泳道不一致。
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
                "##BPMValue", m_createValue, 0.1, 1.0, "%.4f");
            ImGui::Spacing();
            ::MMM::UI::FeedbackCheckbox(
                TR("ui.timeline.event_creator.keep_speed").data(),
                &m_keepSpeedOnBpmChange);
        } else if ( createEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble(
                "##JumpValue", m_createValue, 1.0, 10.0, "%.4f");
        } else if ( createEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            drawFullWidthInputDouble(
                "##HSValue", m_createValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            drawFullWidthInputDouble(
                "##ScrollValue", m_createValue, 0.01, 0.1, "%.4f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float actionButtonWidth =
            calcButtonRowWidth(2, std::floor(100.0f * dpiScale));
        // 创建弹窗与修改弹窗共享同一约束，避免入口行为不一致。
        const bool createValueValid =
            isValidTimingEditorValue(createEffect, m_createValue);
        // 校验与编辑弹窗共用同一 helper，两个入口保持一致。
        if ( !createValueValid ) ImGui::BeginDisabled();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.timeline.event_creator.create").data(),
                 ImVec2(actionButtonWidth, 0)) ) {
            const auto type       = createEffect;
            double     finalValue = getStoredValue(type, m_createValue);

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    m_createTimeManual, type, finalValue }));
            // 最近创建标识只用于 UI 高亮，不作为实体身份或命令结果。
            m_lastCreatedTimingTime   = m_createTimeManual;
            m_lastCreatedTimingEffect = type;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;

            if ( type == ::MMM::TimingEffect::BPM && m_keepSpeedOnBpmChange ) {
                // 两条创建命令异步回流，binding 以时间和最新实体 ID 重新关联。
                createKeepSpeedScrollEvent(m_createTimeManual, m_createValue);
                beginKeepSpeedBinding(m_createTimeManual);
            }

            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }
        if ( !createValueValid ) ImGui::EndDisabled();

        if ( editingDisabled ) {
            ImGui::EndDisabled();
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
///
/// @details 独立窗口生命周期：
/// - 打开状态存储在 `TimelineAuxiliaryWindowState`。
/// - 主 Timeline 窗口隐藏不影响本窗口绘制。
/// - 本窗口需要独立时间线快照准备条件。
/// - 关闭时清除恢复和聚焦请求。
/// - 关闭时清除绑定谱面键。
/// - 关闭时清除行选择锚点。
/// - 关闭时终止行拖选状态。
/// - 关闭时清除拖选基线集合。
/// - 关闭时结束 keep-speed 临时联动。
/// - 标题翻译可变，`###TimingPointsTableWindow` 保持固定内部 ID。
/// - 首次窗口尺寸为 820x450 逻辑像素。
/// - 样式参数按当前窗口 DPI 缩放。
/// - 圆角、内边距和元素间距沿用编辑器美学设置。
/// - 窗口位置恢复和可达性检查使用公共辅助窗口 helper。
/// - popup 暂时夺焦时保留此前聚焦可达状态。
///
/// @details 快照归属：
/// - 读取活动 Session 条目时短暂持有 session mutex。
/// - Logo 占位会话不能打开时间点表格。
/// - 无活动 Session 时立即关闭窗口。
/// - `getTimelineTableSnapshotStatus` 区分关闭、等待和就绪。
/// - 请求早于首帧快照时绘制同步占位窗口。
/// - 等待状态不访问空 `m_currentSnapshot`。
/// - 就绪快照必须提供非空 beatmap key。
/// - 首次就绪时把 key 绑定到窗口。
/// - 后续帧必须与绑定 key 完全一致。
/// - 活动谱面切换时关闭旧表格，而不展示跨谱面数据。
/// - key 比较同时检查长度和字节内容。
/// - 表格内容来自当前活动 Session 的完整 Timing 列表。
/// - RenderSnapshot 只提供谱面身份、播放和 BPM 投影数据。
/// - 表格数据准备不依赖主时间线是否渲染。
///
/// @details 播放与编辑边界：
/// - 播放中表格保持可见但整体编辑区域禁用。
/// - 播放开始时终止行拖选状态。
/// - 播放开始时清除拖选锚点和基线。
/// - 播放开始时结束 keep-speed 临时联动。
/// - 停止播放后表格从新快照恢复编辑。
/// - UI 不直接修改 Timeline registry。
/// - 所有新增、更新和删除均发布逻辑命令。
/// - 行内提交只在控件结束编辑时触发。
/// - 弹窗、搜索和列菜单不阻塞逻辑线程。
/// - 会话锁不覆盖 ImGui 表格绘制。
///
/// @details 数据与拍位：
/// - 每帧收集当前谱面的完整 Timing 元素列表。
/// - 元素按时间及类型稳定排序。
/// - BPM 快照构建连续 beat 换算时间线。
/// - fallback BPM 使用统一规范化语义。
/// - 每行秒时间可转换为拍号、分拍位和拟合误差。
/// - 拍号是连续 beat 的整数部分。
/// - 分拍位以有限分母分数或小数编辑。
/// - 用户提交拍位后反算真实秒时间。
/// - 时间最小钳制到零。
/// - Malody metadata 的 beat 同步更新为最终实际拍位。
/// - 未编辑拍位时不因拟合近似改变原秒时间。
/// - 拟合误差超过阈值时显示警告。
/// - BPM 行参数编辑使用统一合法范围语义。
/// - 其它效果保留原有参数符号语义。
///
/// @details 选择模型：
/// - `m_selectedTimingEntities` 与时间线画布共享选择集合。
/// - 单击行选择对应 Timing 实体。
/// - Ctrl/Command 单击切换单个实体。
/// - Shift 单击以选择锚点建立连续范围。
/// - 行拖动可连续扩展选择范围。
/// - 拖选开始时冻结原选择集。
/// - 带修饰键拖选把范围并入基线。
/// - 普通拖选以当前范围替换选择。
/// - 选择锚点若已不在选择集中则重新规整。
/// - 单选集合可自动成为新的锚点。
/// - 快照删除实体后选择集合由公共清理流程更新。
/// - 点击行内输入控件不应误触发行选择。
/// - 拖选释放后清除瞬态基线。
///
/// @details 顶层工具栏：
/// - “添加 BPM”在活动 Session 时间创建默认 120 BPM。
/// - “保持速度”决定新增 BPM 是否联动创建 Scroll。
/// - “添加流速”创建默认 1.0 Scroll。
/// - “添加 Jump”创建默认 1000 Jump。
/// - “添加 HS”创建默认 1.0 HS。
/// - 新建动作记录时间、类型和三秒高亮截止时间。
/// - 定位判定线在元素为空时禁用。
/// - 定位请求保存目标 Session 时间。
/// - 下一次表格布局完成后滚动到最邻近可见行。
/// - 工具栏按钮使用统一 FeedbackButton。
/// - keep-speed BPM 会开启临时实体绑定追踪。
///
/// @details 搜索与筛选：
/// - 可单独启用只显示 BPM。
/// - BPM、SV、Jump、HS 可组合类型过滤。
/// - 未选择任何类型时表示不过滤类型。
/// - 数值搜索使用短文本缓冲区。
/// - 搜索解析支持科学计数格式。
/// - 非空但非法的数值查询显示错误提示。
/// - 非法数值查询不匹配任何行。
/// - 数值匹配使用精确语义 helper 和浮点容差。
/// - 可见行索引引用完整 elements 数组。
/// - 每次过滤条件变化后重建可见索引。
/// - 清除筛选重置 BPM 开关、类型数组和文本缓冲。
/// - 无任何筛选时清除按钮禁用。
/// - 结果计数基于当前可见行数。
/// - 搜索替换只作用于匹配后的行集合。
///
/// @details 批量操作与剪贴板：
/// - 批量工具折叠在可展开 TreeNode 内。
/// - 复制只收集当前选择且仍存在的行。
/// - Cut 在复制成功后发布删除命令。
/// - Paste 先从系统桥接导入编辑器剪贴板。
/// - 粘贴锚点优先使用选择锚点行时间。
/// - 没有锚点时使用任一选中行时间。
/// - 无选择时使用活动 Session 当前时间。
/// - 粘贴时间最小钳制为零。
/// - 秒/beat 粘贴基础遵守编辑器偏好。
/// - 包含 BPM 的剪贴板按相对秒粘贴，避免新 BPM 尚未应用时错拍。
/// - 粘贴后清除旧表格选择锚点。
/// - 删除选中行复用时间线选择删除入口。
/// - 批量替换发布独立或批量逻辑命令，不直接写 registry。
///
/// @details 列与滚动：
/// - 表格固定声明七列。
/// - 各列拥有独立最小宽度。
/// - 用户列显隐状态通过 ImGui table settings 持久化。
/// - 右键表头菜单可以显示或隐藏可选列。
/// - 列状态写入 `IsUserEnabledNextFrame`，下一帧安全生效。
/// - 当前禁用列不绘制其行内控件。
/// - 水平空间不足时表格内部滚动，不压缩到不可编辑宽度。
/// - 垂直滚动使用独立加宽滚动条。
/// - grab 具有最小尺寸，长表格仍可操作。
/// - 定位判定线只在过滤后可见行中查找最近项。
/// - 二分查找依赖 visibleIndices 保持原时间顺序。
/// - 等距时优先前一行保持稳定。
/// - 新建高亮和定位滚动是独立状态。
///
/// @details keep-speed 联动：
/// - 新增 BPM 与 Scroll 命令异步产生两个新实体。
/// - binding 以创建时间匹配同时间事件。
/// - 同类型多个候选时选择实体 ID 最新者。
/// - BPM 和 Scroll 实体都识别后可联动行内编辑。
/// - 编辑 BPM 时即时发布对应 Scroll 参数更新。
/// - Scroll 值按偏好 BPM 除以规范新 BPM 计算。
/// - 关闭窗口、播放或联动结束时清除绑定。
/// - 临时绑定只影响本次成对创建，不吸附历史同时间事件。
/// - 绑定实体仍通过普通逻辑更新命令修改。
///
/// @details 性能与安全：
/// - 常规帧不访问文件系统。
/// - 文本解析不使用异常机制。
/// - 固定输入缓冲区始终保留 NUL 结尾。
/// - 表格 ImGuiID 包含实体 ID，跨行控件不冲突。
/// - 字符串视图只在其源缓冲生命周期内使用。
/// - 只在需要 metadata 时短暂锁定 Session。
/// - 不在持锁期间发布或等待逻辑命令。
/// - 每帧排序范围仅为当前谱面 Timing 列表。
/// - 关闭与等待快照路径尽早返回。
/// - 所有 PushStyleVar、BeginDisabled、BeginTable 和 Child 都成对结束。
///
/// @details 表格行标识：
/// - 每行以 Timing entity integral 值构造稳定 ImGui ID。
/// - effect 类型作为同时间多行的可见语义标签。
/// - 行颜色与时间线 marker 使用相同类型配色。
/// - 最近创建的时间与类型在三秒窗口内高亮。
/// - keep-speed binding 的 BPM 与 Scroll 行使用联动提示。
/// - 选中行背景独立于最近创建高亮。
/// - hover 行只改变当前帧视觉反馈。
/// - 行身份不依赖当前排序后的可见索引。
/// - 筛选隐藏再恢复后仍能匹配原实体选择。
/// - null entity 行不会进入可编辑表格。
///
/// @details 时间列编辑：
/// - 秒时间列显示公共格式化文本。
/// - 编辑提交保持原始 double 秒值。
/// - 时间提交最小钳制到零。
/// - 拍号列展示连续 beat 的整数部分。
/// - 分拍列展示拟合后的分数。
/// - 分拍误差超过三毫秒时显示警告色。
/// - 拍号或分拍提交共用 `publishTimingBeatPositionUpdate`。
/// - 更新命令同时保留当前参数值。
/// - Malody beat metadata 按最终钳制时间重新生成。
/// - BPM 时间线在当前表格帧内复用。
/// - 行时间更新后等待逻辑快照回流再重排。
/// - UI 不在当前容器中原地移动行。
///
/// @details 参数列编辑：
/// - 参数列显示 `getDisplayValue` 转换后的值。
/// - 提交时通过 `getStoredValue` 恢复存储语义。
/// - BPM 行使用 0.1 和 1.0 步进。
/// - Jump 行使用适合毫秒的 1 和 10 步进。
/// - Scroll 与 HS 使用 0.01 和 0.1 步进。
/// - 非有限参数提交被拒绝。
/// - 负 BPM 提交被拒绝。
/// - 其它效果允许负参数。
/// - keep-speed 绑定 BPM 改变时联动更新 Scroll 行。
/// - 普通 BPM 行不会隐式修改相邻 Scroll。
/// - 参数更新保持行原时间和 metadata。
/// - 行内编辑结束才发布更新命令。
///
/// @details 删除与上下文操作：
/// - 每行删除按钮只删除对应实体。
/// - 删除已选实体时同步从本地选择集合移除。
/// - 删除选择锚点时清除锚点。
/// - 删除 keep-speed 绑定实体时结束临时联动。
/// - Delete 快捷键删除当前多选集合。
/// - Ctrl/Command+C 复制当前选择。
/// - Ctrl/Command+X 在复制成功后删除选择。
/// - Ctrl/Command+V 使用解析出的表格锚点粘贴。
/// - 快捷键在文本输入活动时由 ImGui 捕获。
/// - 右键表头菜单只管理列，不修改行。
/// - 行操作按钮全部使用统一 FeedbackButton。
///
/// @details 筛选替换：
/// - 替换参数输入与搜索参数输入分离存储。
/// - 只有有效数值搜索时才允许数值替换。
/// - 替换只遍历当前 visibleElementIndices。
/// - effect 过滤决定哪些类型进入替换集合。
/// - 只显示 BPM 开关进一步限制集合。
/// - 替换值仍经过各 effect 的提交校验。
/// - 不合法目标类型值不会发布更新。
/// - 每个更新保留原时间与 metadata。
/// - 批量时间位移对所有匹配行应用同一增量。
/// - 位移结果最小钳制为零。
/// - 操作后由快照回流更新过滤结果和排序。
/// - 搜索字符串本身不写入项目配置。
///
/// @details 局部子窗口：
/// - 工具栏和筛选区位于滚动表格之外。
/// - 表格主体使用独立 Child 控制可用高度。
/// - 横向最小总宽由七列最小宽度求和。
/// - 内容宽度不足时只在 Child 内水平滚动。
/// - 加宽 scrollbar 只作用于表格 Child 样式。
/// - 垂直 grab 尺寸具有可操作下限。
/// - 表头保持可见并提供列菜单。
/// - 行滚动不移动外部创建和批量工具。
/// - 定位判定线请求在 BeginTable 后执行。
/// - 目标行通过最近时间二分查找得到。
/// - `SetScrollHereY` 只影响当前表格滚动容器。
/// - 过滤为空时定位请求安全清除。
///
/// @details 帧末清理：
/// - 用户关闭标题栏后调用统一 close lambda。
/// - `opened == false` 仍必须调用 ImGui::End。
/// - 播放禁用范围在表格内容结束后成对恢复。
/// - 六项窗口样式在函数退出前成对 PopStyleVar。
/// - 单次恢复和聚焦请求在 Begin 后清除。
/// - 拖选鼠标释放后清除基线集合。
/// - 新建高亮超过截止时间后自然失效。
/// - 绑定实体失效时由刷新与窗口生命周期清理。
/// - 快照等待路径不残留半初始化 ImGui table。
/// - 任一提前返回都不会遗留 session mutex。
///
/// @details 七列职责：
/// - 选择列提供行选择、范围选择和拖选的交互区域。
/// - 时间列展示格式化秒时间并允许精确数值编辑。
/// - 拍号列展示并编辑连续 beat 的整数部分。
/// - 分拍位列展示分数并接受分数或小数输入。
/// - 类型列展示 BPM、SV、Jump 或 HS 标签及语义色。
/// - 数值列展示并编辑对应 Timing 参数。
/// - 操作列提供单行删除等动作。
/// - 每列是否绘制必须先检查当前帧 IsEnabled。
/// - 隐藏列不创建行内 ImGui 控件。
/// - 固定列索引与最小宽度数组一一对应。
/// - 表头翻译不参与内部列 ID。
/// - 用户显隐选择由 header context menu 修改。
///
/// @details 行点击判定：
/// - 行背景 hit zone 覆盖当前可见列组成的整行宽度。
/// - 输入框和按钮 active 时不启动行拖选。
/// - 左键按下时记录拖选锚点实体。
/// - 首次跨行移动后标记拖选确实发生。
/// - 未移动的按下释放按普通单击处理。
/// - 拖选范围按完整 elements 顺序确定。
/// - 筛选隐藏行不进入可见范围选择。
/// - Shift 范围以最近有效选择锚点为起点。
/// - 锚点不在可见集合时退化为当前行。
/// - Ctrl/Command 修饰状态在按下时决定基线合并语义。
/// - 鼠标移出 Child 后已开始拖选仍能在释放时结束。
/// - 播放开始会取消未完成拖选而不提交数据修改。
///
/// @details 快捷键焦点：
/// - 只有表格窗口或其子 popup 可达聚焦时响应表格快捷键。
/// - 文本输入控件活动时复制、粘贴和删除由输入控件优先处理。
/// - 列菜单打开时保持窗口激活但不触发行快捷键。
/// - 标题栏关闭按钮不穿透到表格行。
/// - 时间线主窗口焦点不替代表格窗口焦点。
/// - 从帮助菜单激活窗口时聚焦请求只执行一次。
/// - 已聚焦且可达时再次激活可以关闭窗口。
/// - 脱离屏幕的窗口再次激活会先恢复再聚焦。
/// - Dock 内隐藏标签视为不可达并请求恢复。
/// - popup 关闭后的下一帧重新计算真实焦点状态。
///
/// @details 命令一致性：
/// - 快速新增与创建弹窗使用相同默认 Timing 参数。
/// - 行内编辑与编辑弹窗使用相同显示/存储转换入口。
/// - 拍位编辑与创建弹窗使用相同分数解析器。
/// - BPM 规范化与其它项目入口使用相同公共函数。
/// - 有限越界 BPM 不回退 120，而是钳制到最近边界。
/// - 非有限 BPM 按公共回退语义处理。
/// - 创建、更新、删除都通过 EventBus 进入逻辑线程。
/// - UI 不预测新实体 ID。
/// - UI 不假设命令一定成功，最终以新快照为准。
/// - 最近创建高亮按时间和 effect 匹配，不作为成功证明。
/// - keep-speed binding 通过回流实体重新确认联动对象。
/// - 复制的 metadata 按值保存，粘贴时完整恢复。
///
/// @details 边界数据：
/// - 空 Timing 列表仍显示创建工具栏和空表格状态。
/// - 空筛选结果不会访问 visibleIndices 首尾。
/// - 非有限活动时间回退到快照当前时间。
/// - 非有限搜索值显示错误且不匹配行。
/// - 非有限编辑草稿禁用提交。
/// - 负创建时间在提交路径钳制到零。
/// - 空 beat 时间线使用 fallback BPM 线性换算。
/// - 空 metadata 仍可生成只含 Malody beat 的结果。
/// - 已删除实体 metadata 读取返回空值语义对象。
/// - 当前列指针为空时列 helper 返回 false。
/// - 列索引越界时显隐 helper 安全返回或忽略。
/// - 输入缓冲截断始终保证 NUL 终止。
/// - 表格滚动位置由 ImGui 窗口状态维护，不写入谱面。
/// - 搜索过滤状态属于当前 TimelineCanvas 实例。
/// - 多谱面实例不会共享搜索文本或列选择锚点。
/// - 时间点表格关闭不会关闭 Timeline 主窗口。
/// - Timeline 主窗口关闭也不会强制关闭本表格。
/// - 表格窗口与批注表窗口不共享快照消费条件。
/// - 表格窗口打开即可触发独立快照准备。
/// - 快照准备完成前用户关闭请求立即生效。
/// - 所有用户可见按钮均经过统一反馈控件。
/// - 表格没有自定义后台线程或轮询计时器。
/// - UI 帧内创建的局部向量在函数返回后释放。
/// - 成员 scratch 容器只保存需要跨帧的交互状态。
/// @warning UI 热路径：打开时每帧执行；快照尚未就绪时只绘制同步提示。
void TimelineCanvas::renderTimingPointsTableWindow()
{
    if ( !m_auxiliaryWindowState.timingPointsTableOpen ) {
        // 关闭状态主动清空所有窗口局部锁存，下一次打开从干净状态开始。
        m_shouldRecoverTableWindow         = false;
        m_shouldFocusTableWindow           = false;
        m_isTableWindowFocusedAndReachable = false;
        m_tableBeatmapKey.clear();
        m_tableSelectionAnchorEntity = entt::null;
        m_isTableRowDragSelecting    = false;
        m_tableRowDragAnchorEntity   = entt::null;
        m_tableRowDragBaseSelection.clear();
        m_hasTableRowDragSelectionMoved = false;
        finishKeepSpeedBinding();
        return;
    }

    auto closeTableWindow = [this]() {
        // 所有异常归属和用户关闭路径复用同一完整清理序列。
        m_auxiliaryWindowState.timingPointsTableOpen = false;
        m_shouldRecoverTableWindow                   = false;
        m_shouldFocusTableWindow                     = false;
        m_isTableWindowFocusedAndReachable           = false;
        m_tableBeatmapKey.clear();
        m_tableScrollToCurrentTimePending = false;
        m_tableSelectionAnchorEntity      = entt::null;
        m_isTableRowDragSelecting         = false;
        m_tableRowDragAnchorEntity        = entt::null;
        m_tableRowDragBaseSelection.clear();
        m_hasTableRowDragSelectionMoved = false;
        finishKeepSpeedBinding();
    };

    auto&                       engine = Logic::EditorEngine::instance();
    TimelineTableSnapshotStatus snapshotStatus;
    {
        // 会话锁保证读取 currentBeatmap 地址令牌时不会与谱面切换并发。
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        const int32_t activeIndex = engine.getActiveSessionIndex();
        const auto*   activeEntry = engine.getSessionEntry(activeIndex);
        if ( !activeEntry || activeEntry->isLogoPlaceholder ||
             !activeEntry->session ) {
            // 没有真实活动谱面会话时表格没有合法数据所有者。
            closeTableWindow();
            return;
        }
        snapshotStatus = getTimelineTableSnapshotStatus(
            activeEntry->session.get(), m_currentSnapshot);
    }
    if ( snapshotStatus == TimelineTableSnapshotStatus::Close ) {
        closeTableWindow();
        return;
    }
    if ( snapshotStatus == TimelineTableSnapshotStatus::AwaitingSnapshot ) {
        // 主窗口隐藏启动时也允许先显示占位窗口，等待独立准备快照到达。
        bool& windowOpen = m_auxiliaryWindowState.timingPointsTableOpen;
        renderTimingTableSyncingWindow(windowOpen,
                                       m_shouldRecoverTableWindow,
                                       m_shouldFocusTableWindow,
                                       m_isTableWindowFocusedAndReachable);
        return;
    }
    const std::string_view currentBeatmapKey =
        getTimingPointsTableBeatmapKey(*m_currentSnapshot);
    if ( currentBeatmapKey.empty() ) {
        closeTableWindow();
        return;
    }
    if ( m_tableBeatmapKey.empty() ) {
        // 首个就绪快照建立窗口与谱面的身份绑定。
        m_tableBeatmapKey.assign(currentBeatmapKey.data(),
                                 currentBeatmapKey.size());
    } else if ( m_tableBeatmapKey.size() != currentBeatmapKey.size() ||
                !std::equal(m_tableBeatmapKey.begin(),
                            m_tableBeatmapKey.end(),
                            currentBeatmapKey.begin()) ) {
        // 谱面切换后关闭旧窗口，避免上一谱面的行选择泄漏到新谱面。
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
    if ( m_shouldFocusTableWindow ) {
        ImGui::SetNextWindowFocus();
    }

    std::string windowTitle =
        TR("ui.timeline.timing_points_table.title").toString() +
        "###TimingPointsTableWindow";
    bool&      windowOpen = m_auxiliaryWindowState.timingPointsTableOpen;
    const bool wasOpenBeforeBegin = windowOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &windowOpen);
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin,
                                                &windowOpen);
    if ( windowOpen ) {
        recoverCurrentAuxiliaryWindow(m_shouldRecoverTableWindow, dpiScale);
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool reachable = isCurrentAuxiliaryWindowReachable(dpiScale);
        const bool popupOpen = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        m_isTableWindowFocusedAndReachable =
            resolveTimelineTableWindowFocusedAndReachable(
                m_isTableWindowFocusedAndReachable,
                reachable,
                focused,
                popupOpen);
    } else {
        m_isTableWindowFocusedAndReachable = false;
    }
    m_shouldRecoverTableWindow = false;
    m_shouldFocusTableWindow   = false;
    if ( opened ) {
        const bool editingDisabled = m_currentSnapshot->isPlaying;
        if ( editingDisabled ) {
            // 播放开始会终止无法继续的拖选与临时 BPM/Scroll 联动。
            m_isTableRowDragSelecting       = false;
            m_tableRowDragAnchorEntity      = entt::null;
            m_hasTableRowDragSelectionMoved = false;
            m_tableRowDragBaseSelection.clear();
            finishKeepSpeedBinding();
            ImGui::BeginDisabled();
        }

        auto elements = collectTimelineElements();
        // 完整表格元素独立从 Session 收集，不受当前视口裁剪范围限制。
        const auto beatTimeline =
            buildTimingTableBeatTimeline(*m_currentSnapshot);
        const double tableFallbackBpm =
            timingTableFallbackBpm(*m_currentSnapshot);
        if ( m_tableSelectionAnchorEntity != entt::null &&
             !m_selectedTimingEntities.contains(
                 m_tableSelectionAnchorEntity) ) {
            m_tableSelectionAnchorEntity =
                m_selectedTimingEntities.size() == 1
                    ? *m_selectedTimingEntities.begin()
                    : entt::null;
        }
        refreshKeepSpeedBinding(elements);
        const double tableCurrentTime =
            getActiveSessionTimelineTime(m_currentSnapshot->currentTime);
        const auto copyTableSelection = [&](bool cut) {
            // 只有成功写入剪贴板后才执行 cut 删除，失败不破坏原选择。
            if ( !copyTimingTableSelectionToClipboard(elements,
                                                      m_selectedTimingEntities,
                                                      beatTimeline,
                                                      tableFallbackBpm) ) {
                return;
            }
            if ( cut ) {
                deleteSelectedTimingEvents();
                m_tableSelectionAnchorEntity = entt::null;
            }
        };
        const auto resolveTablePasteAnchor = [&]() {
            // 粘贴锚点优先选择显式 anchor，其次任一选择，最后当前时间。
            const auto anchor = std::find_if(
                elements.begin(), elements.end(), [&](const auto& element) {
                    return getElementEntity(element) ==
                           m_tableSelectionAnchorEntity;
                });
            if ( anchor != elements.end() ) {
                return anchor->time;
            }
            const auto selected = std::find_if(
                elements.begin(), elements.end(), [&](const auto& element) {
                    return m_selectedTimingEntities.contains(
                        getElementEntity(element));
                });
            return selected != elements.end() ? selected->time
                                              : tableCurrentTime;
        };
        const auto pasteTableSelection = [&]() {
            // 先同步系统剪贴板到编辑器级格式，再按解析出的锚点批量粘贴。
            ::MMM::UI::ClipboardBridge::importEditorClipboardFromSystem();
            pasteTimingClipboard(std::max(0.0, resolveTablePasteAnchor()));
            m_tableSelectionAnchorEntity = entt::null;
        };

        // 顶层工具栏
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加 BPM") ) {
            // 工具栏快速新增使用活动 Session 时间，而非快照视觉偏移时间。
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
                // 联动创建后按时间追踪异步回流的最新 BPM 与 Scroll 实体。
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

        std::vector<std::size_t> visibleElementIndices;
        visibleElementIndices.reserve(elements.size());
        std::string_view      searchValueText;
        bool                  hasSearchValueText = false;
        std::optional<double> parsedSearchValue;
        bool                  hasValidSearchValue          = false;
        const auto            rebuildVisibleElementIndices = [&]() {
            // 搜索文本在本次重建期间以 string_view 引用固定成员缓冲。
            searchValueText =
                trimTimingTableAsciiWhitespace(m_tableSearchValueBuffer.data());
            hasSearchValueText = !searchValueText.empty();
            parsedSearchValue  = hasSearchValueText
                                                ? parseTimingTableDouble(searchValueText)
                                                : std::nullopt;
            hasValidSearchValue =
                parsedSearchValue && std::isfinite(*parsedSearchValue);
            const bool hasEffectSearchFilter =
                std::any_of(m_tableSearchEffectFilters.begin(),
                            m_tableSearchEffectFilters.end(),
                            [](bool enabled) { return enabled; });
            // 没有任何类型开关时解释为全部类型，而不是空结果。

            visibleElementIndices.clear();
            for ( std::size_t elementIndex = 0; elementIndex < elements.size();
                  ++elementIndex ) {
                const auto effect = getElementEffect(elements[elementIndex]);
                if ( m_tableOnlyShowBpm &&
                     effect != ::MMM::TimingEffect::BPM ) {
                    continue;
                }
                if ( hasEffectSearchFilter &&
                     !m_tableSearchEffectFilters
                         [getTimingTableSearchEffectIndex(effect)] ) {
                    continue;
                }
                if ( hasSearchValueText &&
                     (!hasValidSearchValue ||
                      !timingTableSearchValueEquals(
                          getDisplayValue(
                              effect,
                              getElementRawValue(elements[elementIndex]),
                              getElementEntity(elements[elementIndex])),
                          *parsedSearchValue)) ) {
                    // 非法数值查询和不匹配行都不进入可见索引。
                    continue;
                }
                visibleElementIndices.push_back(elementIndex);
            }
        };

        // 批量修改工具
        const std::string bulkToolsLabel =
            fmt::format("{}###TimingTableBulkTools",
                        TR("ui.timeline.timing_points_table.bulk_tools"));
        if ( ::MMM::UI::FeedbackTreeNode(
                 bulkToolsLabel.c_str(),
                 ImGuiTreeNodeFlags_SpanAvailWidth |
                     ImGuiTreeNodeFlags_FramePadding) ) {
            const std::string onlyBpmLabel = fmt::format(
                "{}###TimingTableOnlyBpm",
                TR("ui.timeline.timing_points_table.filter.only_bpm"));
            ::MMM::UI::FeedbackCheckbox(onlyBpmLabel.c_str(),
                                        &m_tableOnlyShowBpm);
            ImGui::SameLine();
            const bool hadTimingTableFilter =
                m_tableOnlyShowBpm ||
                std::any_of(m_tableSearchEffectFilters.begin(),
                            m_tableSearchEffectFilters.end(),
                            [](bool enabled) { return enabled; }) ||
                !trimTimingTableAsciiWhitespace(m_tableSearchValueBuffer.data())
                     .empty();
            const std::string clearFilterLabel =
                fmt::format("{}###TimingTableClearFilter",
                            TR("ui.timeline.timing_points_table.filter.clear"));
            if ( !hadTimingTableFilter ) {
                ImGui::BeginDisabled();
            }
            if ( ::MMM::UI::FeedbackButton(clearFilterLabel.c_str()) ) {
                m_tableOnlyShowBpm = false;
                m_tableSearchEffectFilters.fill(false);
                m_tableSearchValueBuffer.fill('\0');
                m_tableSearchReplacementValue = 0.0;
            }
            if ( !hadTimingTableFilter ) {
                ImGui::EndDisabled();
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(
                TR("ui.timeline.timing_points_table.search.attributes").data());
            const std::array<const char*, TIMING_TABLE_SEARCH_EFFECT_COUNT>
                searchEffectTranslationKeys{
                    "ui.timeline.timing_points_table.search.effect.bpm",
                    "ui.timeline.timing_points_table.search.effect.sv",
                    "ui.timeline.timing_points_table.search.effect.jump",
                    "ui.timeline.timing_points_table.search.effect.hs"
                };
            const std::array<const char*, TIMING_TABLE_SEARCH_EFFECT_COUNT>
                searchEffectIds{ "TimingTableSearchBpm",
                                 "TimingTableSearchSv",
                                 "TimingTableSearchJump",
                                 "TimingTableSearchHs" };
            for ( std::size_t filterIndex = 0;
                  filterIndex < TIMING_TABLE_SEARCH_EFFECT_COUNT;
                  ++filterIndex ) {
                ImGui::SameLine();
                const std::string filterLabel =
                    fmt::format("{}###{}",
                                TR(searchEffectTranslationKeys[filterIndex]),
                                searchEffectIds[filterIndex]);
                ::MMM::UI::FeedbackCheckbox(
                    filterLabel.c_str(),
                    &m_tableSearchEffectFilters[filterIndex]);
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(
                TR("ui.timeline.timing_points_table.search.value").data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f * dpiScale);
            ImGui::InputTextWithHint(
                "###TimingTableSearchValue",
                TR("ui.timeline.timing_points_table.search.value_hint").data(),
                m_tableSearchValueBuffer.data(),
                m_tableSearchValueBuffer.size(),
                ImGuiInputTextFlags_CharsScientific);

            rebuildVisibleElementIndices();

            ImGui::SameLine();
            if ( hasSearchValueText && !hasValidSearchValue ) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                   "%s",
                                   TR("ui.timeline.timing_points_table."
                                      "search.invalid_value")
                                       .data());
            } else {
                ImGui::TextUnformatted(TR_FMT("ui.timeline.timing_points_"
                                              "table.search.result_count",
                                              visibleElementIndices.size(),
                                              elements.size())
                                           .c_str());
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("ui.timeline.timing_points_table."
                                      "search.replacement_value")
                                       .data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f * dpiScale);
            ImGui::InputDouble("###TimingTableSearchReplacement",
                               &m_tableSearchReplacementValue,
                               0.01,
                               0.1,
                               "%.4f");
            const bool replacementTouchesBpm = std::any_of(
                visibleElementIndices.begin(),
                visibleElementIndices.end(),
                [&](std::size_t elementIndex) {
                    return getElementEffect(elements[elementIndex]) ==
                           ::MMM::TimingEffect::BPM;
                });
            // 批量替换 BPM 时仅排除负数，零值与单项编辑保持一致。
            const bool replacementValueValid =
                std::isfinite(m_tableSearchReplacementValue) &&
                (!replacementTouchesBpm ||
                 m_tableSearchReplacementValue >= 0.0);
            const bool canReplaceSearchResults =
                hasValidSearchValue && !visibleElementIndices.empty() &&
                replacementValueValid;
            ImGui::SameLine();
            if ( !canReplaceSearchResults ) {
                ImGui::BeginDisabled();
            }
            const std::string replaceLabel =
                TR_FMT("ui.timeline.timing_points_table.search.replace",
                       visibleElementIndices.size()) +
                "###TimingTableSearchReplace";
            if ( ::MMM::UI::FeedbackButton(replaceLabel.c_str()) ) {
                Logic::CmdUpdateTimelineEvents command;
                command.events.reserve(visibleElementIndices.size());
                for ( std::size_t elementIndex : visibleElementIndices ) {
                    const auto& element = elements[elementIndex];
                    const auto  effect  = getElementEffect(element);
                    const auto  entity  = getElementEntity(element);
                    command.events.push_back(
                        { entity,
                          element.time,
                          getStoredValue(
                              effect, m_tableSearchReplacementValue, entity) });
                }
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(std::move(command)));
            }
            if ( !canReplaceSearchResults ) {
                ImGui::EndDisabled();
            }
            if ( replacementTouchesBpm && !replacementValueValid ) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                   "%s",
                                   TR("ui.timeline.timing_points_table.search."
                                      "invalid_bpm_value")
                                       .data());
            }

            // 表格选择与基础 Excel 式剪贴板操作
            const bool hasTableSelection = !m_selectedTimingEntities.empty();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("已选择: %zu", m_selectedTimingEntities.size());
            ImGui::SameLine();
            if ( !hasTableSelection ) {
                ImGui::BeginDisabled();
            }
            if ( ::MMM::UI::FeedbackButton("复制##TimingTableCopy") ) {
                copyTableSelection(false);
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("剪切##TimingTableCut") ) {
                copyTableSelection(true);
            }
            if ( !hasTableSelection ) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("粘贴##TimingTablePaste") ) {
                pasteTableSelection();
            }
            ImGui::SameLine();
            if ( !hasTableSelection ) {
                ImGui::BeginDisabled();
            }
            if ( ::MMM::UI::FeedbackButton("删除##TimingTableDelete") ) {
                deleteSelectedTimingEvents();
                m_tableSelectionAnchorEntity = entt::null;
            }
            if ( !hasTableSelection ) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Ctrl+A / C / X / V");

            ImGui::Separator();

            static double         bulkOffsetValue   = 0.0;
            static double         bulkScaleValue    = 1.0;
            constexpr const char* BULK_OFFSET_LABEL = "批量时间偏移 (秒):";
            constexpr const char* BULK_SCALE_LABEL  = "批量流速缩放倍率:";
            const float           bulkTransformLabelWidth =
                std::max(ImGui::CalcTextSize(BULK_OFFSET_LABEL).x,
                         ImGui::CalcTextSize(BULK_SCALE_LABEL).x);
            const ImGuiStyle& style                   = ImGui::GetStyle();
            const float       bulkTransformValueWidth = std::max(
                160.0f * dpiScale,
                ImGui::CalcTextSize("-000000.0000").x +
                    style.FramePadding.x * 2.0f +
                    (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x) *
                        2.0f);
            if ( ImGui::BeginTable("###TimingTableBulkTransformLayout",
                                   3,
                                   ImGuiTableFlags_SizingFixedFit |
                                       ImGuiTableFlags_NoSavedSettings |
                                       ImGuiTableFlags_NoPadOuterX) ) {
                ImGui::TableSetupColumn("###TimingTableBulkTransformLabel",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        bulkTransformLabelWidth);
                ImGui::TableSetupColumn("###TimingTableBulkTransformValue",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        bulkTransformValueWidth);
                ImGui::TableSetupColumn("###TimingTableBulkTransformAction",
                                        ImGuiTableColumnFlags_WidthFixed);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(BULK_OFFSET_LABEL);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputDouble(
                    "##BulkOffsetInput", &bulkOffsetValue, 0.001, 0.01, "%.4f");
                ImGui::TableSetColumnIndex(2);
                if ( ::MMM::UI::FeedbackButton("应用时间偏移") &&
                     std::abs(bulkOffsetValue) > 1e-6 ) {
                    for ( const auto& el : elements ) {
                        entt::entity ent    = getElementEntity(el);
                        double       rawVal = getElementRawValue(el);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time + bulkOffsetValue, rawVal }));
                    }
                    bulkOffsetValue = 0.0;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(BULK_SCALE_LABEL);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputDouble(
                    "##BulkScaleInput", &bulkScaleValue, 0.01, 0.1, "%.4f");
                ImGui::TableSetColumnIndex(2);
                if ( ::MMM::UI::FeedbackButton("应用流速缩放") &&
                     std::abs(bulkScaleValue - 1.0) > 1e-6 ) {
                    for ( const auto& el : elements ) {
                        if ( el.effects &
                             Common::Render::SCROLL_EFFECT_SCROLL ) {
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

                ImGui::EndTable();
            }

            ImGui::TreePop();
        } else {
            rebuildVisibleElementIndices();
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
        ImVec2 timingTableMin;
        ImVec2 timingTableMax;
        float  timingTableBlankStartY = 0.0f;
        bool   hasTimingTableRect     = false;
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
                timingTableMin                   = table->OuterRect.Min;
                timingTableMax                   = table->OuterRect.Max;
                hasTimingTableRect               = true;
            }
            ImGui::TableHeadersRow();
            renderTimingTableHeaderContextMenu();
            timingTableBlankStartY = ImGui::GetCursorScreenPos().y;

            const int scrollTargetIndex =
                m_tableScrollToCurrentTimePending
                    ? findNearestTimelineElementIndex(elements,
                                                      visibleElementIndices,
                                                      m_tableScrollTargetTime)
                    : -1;
            if ( m_tableScrollToCurrentTimePending && scrollTargetIndex < 0 ) {
                m_tableScrollToCurrentTimePending = false;
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visibleElementIndices.size()));
            if ( scrollTargetIndex >= 0 ) {
                clipper.IncludeItemByIndex(scrollTargetIndex);
            }
            while ( clipper.Step() ) {
                for ( int visibleIndex = clipper.DisplayStart;
                      visibleIndex < clipper.DisplayEnd;
                      ++visibleIndex ) {
                    const int idx = static_cast<int>(
                        visibleElementIndices[static_cast<std::size_t>(
                            visibleIndex)]);
                    const auto&         el         = elements[idx];
                    int                 displayIdx = idx + 1;
                    ::MMM::TimingEffect effect     = getElementEffect(el);
                    entt::entity        ent        = getElementEntity(el);
                    const bool          rowSelected =
                        m_selectedTimingEntities.contains(ent);
                    const bool isKeepSpeedBindingRow =
                        isKeepSpeedBindingEntity(ent);
                    bool isRecentlyCreated =
                        (ImGui::GetTime() <=
                         m_lastCreatedTimingHighlightUntil) &&
                        (effect == m_lastCreatedTimingEffect) &&
                        (std::abs(el.time - m_lastCreatedTimingTime) <= 1e-6);

                    ImGui::TableNextRow();
                    if ( m_tableScrollToCurrentTimePending &&
                         visibleIndex == scrollTargetIndex ) {
                        ImGui::SetScrollHereY(0.5f);
                        m_tableScrollToCurrentTimePending = false;
                    }
                    if ( rowSelected ) {
                        ImVec4 selectedRowColor =
                            ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
                        selectedRowColor.w =
                            std::max(selectedRowColor.w, 0.72f);
                        ImGui::TableSetBgColor(
                            ImGuiTableBgTarget_RowBg0,
                            ImGui::ColorConvertFloat4ToU32(selectedRowColor));
                    } else if ( isKeepSpeedBindingRow ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(180, 225, 255, 115));
                    } else if ( isRecentlyCreated ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(255, 245, 170, 95));
                    }

                    // 第 0 列：序号
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 rowNumberCellMin = ImGui::GetCursorScreenPos();
                    const float  rowNumberCellWidth =
                        ImGui::GetContentRegionAvail().x;
                    const std::string rowLabel =
                        fmt::format("#{}###TimingTableRow_{}",
                                    displayIdx,
                                    static_cast<std::uint32_t>(ent));
                    if ( ::MMM::UI::FeedbackSelectable(
                             rowLabel.c_str(),
                             rowSelected,
                             ImGuiSelectableFlags_SpanAllColumns |
                                 ImGuiSelectableFlags_AllowOverlap,
                             ImVec2(0.0f, ImGui::GetFrameHeight())) ) {
                        if ( !m_hasTableRowDragSelectionMoved ) {
                            const ImGuiIO& io     = ImGui::GetIO();
                            const auto     anchor = std::find_if(
                                visibleElementIndices.begin(),
                                visibleElementIndices.end(),
                                [&](std::size_t elementIndex) {
                                    return getElementEntity(
                                               elements[elementIndex]) ==
                                           m_tableSelectionAnchorEntity;
                                });
                            if ( io.KeyShift &&
                                 anchor != visibleElementIndices.end() ) {
                                if ( !io.KeyCtrl ) {
                                    m_selectedTimingEntities.clear();
                                }
                                const int anchorIndex =
                                    static_cast<int>(std::distance(
                                        visibleElementIndices.begin(), anchor));
                                const int first = std::clamp(
                                    std::min(anchorIndex, visibleIndex),
                                    0,
                                    static_cast<int>(
                                        visibleElementIndices.size()) -
                                        1);
                                const int last = std::clamp(
                                    std::max(anchorIndex, visibleIndex),
                                    0,
                                    static_cast<int>(
                                        visibleElementIndices.size()) -
                                        1);
                                for ( int selectedIndex = first;
                                      selectedIndex <= last;
                                      ++selectedIndex ) {
                                    m_selectedTimingEntities.insert(
                                        getElementEntity(
                                            elements
                                                [visibleElementIndices
                                                     [static_cast<std::size_t>(
                                                         selectedIndex)]]));
                                }
                            } else if ( io.KeyCtrl ) {
                                if ( rowSelected ) {
                                    m_selectedTimingEntities.erase(ent);
                                } else {
                                    m_selectedTimingEntities.insert(ent);
                                }
                                m_tableSelectionAnchorEntity = ent;
                            } else {
                                m_selectedTimingEntities.clear();
                                m_selectedTimingEntities.insert(ent);
                                m_tableSelectionAnchorEntity = ent;
                            }
                        }
                    }
                    const ImVec2 rowNumberItemMin = ImGui::GetItemRectMin();
                    const ImVec2 rowNumberItemMax = ImGui::GetItemRectMax();
                    if ( rowSelected ) {
                        const float accentWidth =
                            std::max(3.0f, std::floor(3.0f * dpiScale));
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            rowNumberItemMin,
                            ImVec2(rowNumberItemMin.x + accentWidth,
                                   rowNumberItemMax.y),
                            ImGui::GetColorU32(ImGuiCol_CheckMark));
                    }
                    const bool rowNumberHovered = ImGui::IsMouseHoveringRect(
                        ImVec2(rowNumberCellMin.x, rowNumberItemMin.y),
                        ImVec2(rowNumberCellMin.x + rowNumberCellWidth,
                               rowNumberItemMax.y));
                    if ( !editingDisabled && rowNumberHovered &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
                        const ImGuiIO& io               = ImGui::GetIO();
                        m_isTableRowDragSelecting       = true;
                        m_tableRowDragAnchorEntity      = ent;
                        m_tableSelectionAnchorEntity    = ent;
                        m_hasTableRowDragSelectionMoved = false;
                        m_tableRowDragBaseSelection =
                            io.KeyCtrl ? m_selectedTimingEntities
                                       : std::unordered_set<entt::entity>{};
                    }
                    const float mouseY = ImGui::GetIO().MousePos.y;
                    const bool  rowDragTargetHovered =
                        mouseY >= rowNumberItemMin.y &&
                        mouseY < rowNumberItemMax.y;
                    if ( m_isTableRowDragSelecting && rowDragTargetHovered &&
                         ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                         ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ) {
                        const auto dragAnchor = std::find_if(
                            visibleElementIndices.begin(),
                            visibleElementIndices.end(),
                            [&](std::size_t elementIndex) {
                                return getElementEntity(
                                           elements[elementIndex]) ==
                                       m_tableRowDragAnchorEntity;
                            });
                        if ( dragAnchor != visibleElementIndices.end() ) {
                            m_hasTableRowDragSelectionMoved = true;
                            m_selectedTimingEntities =
                                m_tableRowDragBaseSelection;
                            const int anchorIndex =
                                static_cast<int>(std::distance(
                                    visibleElementIndices.begin(), dragAnchor));
                            const int first =
                                std::min(anchorIndex, visibleIndex);
                            const int last =
                                std::max(anchorIndex, visibleIndex);
                            for ( int selectedIndex = first;
                                  selectedIndex <= last;
                                  ++selectedIndex ) {
                                m_selectedTimingEntities.insert(
                                    getElementEntity(
                                        elements[visibleElementIndices
                                                     [static_cast<std::size_t>(
                                                         selectedIndex)]]));
                            }
                        }
                    }

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
                    // 单元值下一帧会从快照重建，步进按钮必须在变化当帧提交。
                    const bool beatIndexChanged =
                        ImGui::InputInt(beatId.c_str(), &beatIndexValue, 1, 4);
                    if ( beatIndexChanged ) {
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
                    // InputDouble 的返回值同时覆盖文本编辑和步进按钮。
                    const bool displayValueChanged = ImGui::InputDouble(
                        vId.c_str(),
                        &vVal,
                        effect == ::MMM::TimingEffect::BPM ? 0.1 : 0.01,
                        effect == ::MMM::TimingEffect::BPM ? 1.0 : 0.1,
                        "%.4f");
                    if ( isBoundScroll ) {
                        ImGui::EndDisabled();
                        if ( ImGui::IsItemHovered(
                                 ImGuiHoveredFlags_AllowWhenDisabled) ) {
                            ImGui::SetTooltip(
                                "保持画布速度联动中，修改 BPM 后自动刷新");
                        }
                    }
                    // 表格输入在发布命令前过滤负
                    // BPM，防止非法值短暂写入快照。
                    const bool displayValueValid =
                        isValidTimingEditorValue(effect, vVal);
                    if ( displayValueChanged && !isBoundScroll &&
                         displayValueValid ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                        if ( isBoundBpm ) {
                            updateKeepSpeedBindingScroll(vVal);
                        }
                    }
                    if ( isBoundBpm && ImGui::IsItemDeactivated() ) {
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
                        m_selectedTimingEntities.erase(ent);
                        m_tableSelectionAnchorEntity = entt::null;
                    }
                    if ( const ImGuiTable* table = ImGui::GetCurrentTable() ) {
                        timingTableBlankStartY =
                            std::max(timingTableBlankStartY, table->RowPosY2);
                    }
                }
            }
            if ( m_isTableRowDragSelecting &&
                 !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
                m_isTableRowDragSelecting       = false;
                m_tableRowDragAnchorEntity      = entt::null;
                m_hasTableRowDragSelectionMoved = false;
                m_tableRowDragBaseSelection.clear();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);

        const ImVec2 mousePosition = ImGui::GetMousePos();
        const bool   mouseInsideTimingTable =
            hasTimingTableRect && mousePosition.x >= timingTableMin.x &&
            mousePosition.x < timingTableMax.x &&
            mousePosition.y >= timingTableMin.y &&
            mousePosition.y < timingTableMax.y;
        const bool mouseInsideTimingTableBlank =
            mouseInsideTimingTable && mousePosition.y >= timingTableBlankStartY;
        const ImVec2 windowPosition        = ImGui::GetWindowPos();
        const ImVec2 windowContentMinLocal = ImGui::GetWindowContentRegionMin();
        const ImVec2 windowContentMaxLocal = ImGui::GetWindowContentRegionMax();
        const ImVec2 windowContentMin(
            windowPosition.x + windowContentMinLocal.x,
            windowPosition.y + windowContentMinLocal.y);
        const ImVec2 windowContentMax(
            windowPosition.x + windowContentMaxLocal.x,
            windowPosition.y + windowContentMaxLocal.y);
        const bool mouseInsideWindowContent =
            mousePosition.x >= windowContentMin.x &&
            mousePosition.x < windowContentMax.x &&
            mousePosition.y >= windowContentMin.y &&
            mousePosition.y < windowContentMax.y;
        const bool mouseInsideOutsideTableBlank =
            !mouseInsideTimingTable && mouseInsideWindowContent &&
            ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            !ImGui::IsAnyItemHovered();
        if ( ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             (mouseInsideTimingTableBlank || mouseInsideOutsideTableBlank) ) {
            m_selectedTimingEntities.clear();
            m_tableSelectionAnchorEntity = entt::null;
        }

        const bool tableShortcutFocused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImGuiIO& io = ImGui::GetIO();
        if ( !editingDisabled && tableShortcutFocused &&
             !ImGui::IsAnyItemActive() && !io.WantTextInput ) {
            const bool ctrlOnly =
                io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper;
            const bool noModifier =
                !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper;
            if ( ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
                m_selectedTimingEntities.clear();
                for ( std::size_t elementIndex : visibleElementIndices ) {
                    m_selectedTimingEntities.insert(
                        getElementEntity(elements[elementIndex]));
                }
                m_tableSelectionAnchorEntity =
                    visibleElementIndices.empty()
                        ? entt::null
                        : getElementEntity(
                              elements[visibleElementIndices.front()]);
            } else if ( ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_C, false) ) {
                copyTableSelection(false);
            } else if ( ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_X, false) ) {
                copyTableSelection(true);
            } else if ( ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
                pasteTableSelection();
            } else if ( noModifier &&
                        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) ) {
                deleteSelectedTimingEvents();
                m_tableSelectionAnchorEntity = entt::null;
            } else if ( ImGui::IsKeyPressed(ImGuiKey_Escape, false) ) {
                m_selectedTimingEntities.clear();
                m_tableSelectionAnchorEntity = entt::null;
            }
        }

        if ( editingDisabled ) {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::Canvas
