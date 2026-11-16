#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include "mmm/timing/BpmNormalization.h"

#include <cmath>
#include <cstddef>

namespace MMM::UI
{
/// @brief 将一个段落的编辑转换为保持连续拍位的后续联动。
/// @param index 被明确编辑的段落，之前的段落保持不变。
/// @param timestamp 新段起点，单位秒，已由调用入口执行原有吸附。
/// @param bpm 新 BPM；后续段保留自身 BPM，不统一改成这个值。
/// @return 有效且实际改变数据时为真；越界时整次候选不写入。
/// @warning 仅实际编辑时遍历后续段；复用一次手势历史快照，不逐帧复制列表、
/// 分配临时段落、排序、读取文件或等待其他线程。
bool BpmMeasurementToolView::applyTimingSegmentEdit(std::size_t index,
                                                    double      timestamp,
                                                    double      bpm)
{
    // 所有 UI 入口共享有限值门禁，不让非有限候选进入拍差乘除。
    // 校验只读工具自身状态，不读取音频文件或等待播放线程。
    if ( index >= m_timingSegments.size() || !std::isfinite(timestamp) ||
         !std::isfinite(bpm) || bpm <= 0 )
        return false;
    bpm          = ::MMM::normalizeBpmValue(bpm);
    auto& edited = m_timingSegments[index];
    // 静止按下或重复事件不重算后缀，避免浮点尾差制造空撤销记录。
    // 此判定只比较用户明确编辑的值，已有有效后缀已经由上一次提交维护。
    if ( edited.timestampSeconds == timestamp && edited.bpm == bpm ) {
        m_followEditRejected = false;
        return false;
    }
    // 关闭联动时维持旧行为，归一化和排序仍由各原入口完成。
    if ( !m_followSubsequentSegments ) {
        const bool changed =
            edited.timestampSeconds != timestamp || edited.bpm != bpm;
        edited = { timestamp, bpm };
        syncPrimaryTimingFieldsFromSegments();
        return changed;
    }

    // 冻结发生在首个修改之前；已有鼠标或触控手势会直接复用其快照。
    // 后续候选永远从这个起点重算，往返拖动不累计相位误差。
    beginMeasurementHistoryGesture();
    // 音轨切换与导入已有独立清理边界，快照不会从另一首音频沿用。
    // 借用仅持续到本次同步调用返回，不持有外部对象或可变 ECS 引用。
    const auto& original = m_measurementGestureBefore->timingSegments;
    const auto  reject   = [&]() {
        m_followEditRejected = true;
        return false;
    };
    // 增删段落不属于位置或拍长调整，不能把旧索引套到新拓扑。
    if ( original.size() != m_timingSegments.size() ) return reject();
    constexpr double MIN_GAP  = 1e-6;
    const double     duration = playbackCanvasDuration();
    // 首段允许向音频零点前延伸一拍，与既有首拍控件语义一致。
    // 中间段不得越过前段；之后各段则由完整拍间隔推导。
    const double minimum =
        index == 0 ? -(60.0 / bpm)
                   : m_timingSegments[index - 1].timestampSeconds + MIN_GAP;
    if ( !std::isfinite(duration) || timestamp < minimum ||
         timestamp > duration )
        return reject();

    // 在实际秒差上检查，避免较大时间加上微小间隔后发生浮点舍入。
    // 只比较 timestamp 与 previous+epsilon 可能放行实际小于去重阈值的间隔。
    if ( index > 0 &&
         timestamp - m_timingSegments[index - 1].timestampSeconds <= MIN_GAP )
        return reject();

    /// @brief 保留完整拍间隔，包括拍内百分比，不作整拍或分拍量化。
    /// @warning 输入手势热路径仅计算标量；分母来自已验证的正 BPM。
    const auto nextTime = [&](std::size_t next, double previousTime) {
        const auto& previous = original[next - 1];
        // 旧秒差乘旧 BPM 得到连续拍差，绝不能对拍差 round、floor 或按分拍量化。
        // 段落的 BPM 各自保留，只有紧随编辑段的区间改用候选 BPM。
        const double beatSpan =
            (original[next].timestampSeconds - previous.timestampSeconds) *
            (previous.bpm / 60.0);
        const double newBpm =
            next == index + 1 ? bpm : m_timingSegments[next - 1].bpm;
        // 往返拖回原值时精确恢复原时间戳，不让乘除往返积累几个 ULP。
        // 后续原 BPM 未改变时，这个快速路径还能逐段恢复整条原始后缀。
        if ( previousTime == previous.timestampSeconds &&
             newBpm == previous.bpm )
            return original[next].timestampSeconds;
        return previousTime + beatSpan * (60.0 / newBpm);
    };
    // 先验证整个后缀，再做第二遍写入：不能先改前半截，末尾越界才失败。
    // 各段单独钳制到音频末尾会使段落重合并被归一化删除，因此整次拒绝。
    double candidate = timestamp;
    for ( std::size_t next = index + 1; next < original.size(); ++next ) {
        const double value = nextTime(next, candidate);
        // 最小间距与现有归一化的去重阈值一致，防止后续排序静默吞掉段落。
        // 音频末端只作为合法性边界，不把多个起点逐项钳在同一个末端。
        if ( !std::isfinite(value) || value - candidate <= MIN_GAP ||
             value > duration )
            return reject();
        candidate = value;
    }
    m_followEditRejected = false;
    const bool changed =
        edited.timestampSeconds != timestamp || edited.bpm != bpm;
    edited    = { timestamp, bpm };
    candidate = timestamp;
    for ( std::size_t next = index + 1; next < original.size(); ++next ) {
        // 原始间隔仍从历史快照读取，先前写入的新时间不会反过来改变拍位基准。
        // 只写时间戳，后续 BPM、节点数量和时间顺序保持原身份。
        candidate                               = nextTime(next, candidate);
        m_timingSegments[next].timestampSeconds = candidate;
    }
    // 只同步工具草稿和兼容字段，实际写谱面仍由用户点击“应用”完成。
    // 撤销记录沿用整次手势的前后快照，一次撤销恢复全部联动段。
    syncPrimaryTimingFieldsFromSegments();
    return changed;
}
}  // namespace MMM::UI
