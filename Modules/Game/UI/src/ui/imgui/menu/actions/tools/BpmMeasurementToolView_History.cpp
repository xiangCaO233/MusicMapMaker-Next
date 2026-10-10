#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include "audio/AudioManager.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 工具本地历史上限，防止长时间编辑无限保存段落列表副本。
constexpr std::size_t MAX_MEASUREMENT_HISTORY = 128;
}  // namespace

/// @brief 捕获本帧工具可撤销状态及当前播放头位置。
/// @return 与当前选中音轨绑定的状态副本。
/// @warning 仅用户输入边界调用；段落列表复制不得放入每帧渲染路径。
/// @note 播放头只供明确 seek 的历史项使用，普通播放推进不形成操作。
/// @details 快照只包含此工具能够独立恢复的测量状态，章节和谱面应用
/// 选项不属于本地历史。音轨 ID 是恢复时的身份门禁。
/// 段落列表保留全部顺序，以便多段测量撤销后不丢失后续段。
BpmMeasurementToolView::MeasurementHistorySnapshot
BpmMeasurementToolView::captureMeasurementHistorySnapshot() const
{
    MeasurementHistorySnapshot snapshot;
    snapshot.audioTrackId = m_selectedAudioTrackId;
    // 段落可能在拖动期间连续变化；只在手势边界复制列表。
    snapshot.timingSegments    = m_timingSegments;
    snapshot.bpm               = m_bpm;
    snapshot.beatLengthSeconds = m_beatLengthSeconds;
    snapshot.firstBeatTime     = m_firstBeatTime;
    snapshot.markerWidthMs     = m_markerWidthMs;
    snapshot.beatDivisor       = m_beatDivisor;
    snapshot.zoomSeconds       = m_zoomSeconds;
    snapshot.viewCenter        = m_viewCenter;
    snapshot.playbackSpeed     = m_playbackSpeed;
    // 播放头持续推进，只有明确 seek 的操作才比较此时间。
    // 同轨与独立试听分别读取自身播放头；未选轨时零值仅作占位。
    const auto&  audio = Audio::AudioManager::instance();
    const double audioTime =
        m_playbackRoute == BpmPlaybackRoute::Audition
            ? audio.getAuditionCurrentTime()
        : m_playbackRoute == BpmPlaybackRoute::SynchronizedWithEditor
            ? audio.getCurrentTime()
            : 0.0;
    snapshot.seekCanvasTime = audioTime + playbackVisualOffset();
    // 记录画布时间，恢复时交给同域的 seekPlaybackToCanvasTime。
    return snapshot;
}

/// @brief 开始一次用户输入事务，连续拖动只保存首次状态。
/// @warning 低频输入边界：捕获会复制段落列表，不得在每帧空路径调用。
/// @note 同一手势中重复 begin 保留最早状态，拖动只形成一个撤销点。
/// 历史恢复期间忽略 begin，防止偏好同步或播放命令递归记录。
void BpmMeasurementToolView::beginMeasurementHistoryGesture()
{
    if ( m_restoringMeasurementHistory || m_measurementGestureBefore ) return;
    m_measurementGestureBefore = captureMeasurementHistorySnapshot();
    // 新手势不能继承上一轮的 seek 目标与视野变化标识。
    m_measurementHistorySeekChanged = false;
    m_measurementHistoryViewChanged = false;
    m_measurementHistorySeekCanvasTime.reset();
}

/// @brief 结束用户输入事务并只在有效变化时追加一条历史。
/// @param seekChanged 是否显式提交 seek。
/// @param viewChanged 是否显式修改视图。
/// @param explicitSeekCanvasTime 异步 seek 的画布目标，缺省时使用提交时播放头。
/// @warning 低频输入边界：只有操作结束时复制结果并更新固定长度历史栈。
/// @details 先验证音轨身份，再比较参数、显式视野和显式 seek。
/// 非 seek 操作忽略播放推进，非视野操作忽略播放自动跟随。
/// 空操作保留 redo 分支；真实变化才追加 undo 并清空分支。
/// CmdSeek 异步执行时以后端尚未确认的用户目标作为 after 时间。
void BpmMeasurementToolView::finishMeasurementHistoryGesture(
    bool seekChanged, bool viewChanged,
    std::optional<double> explicitSeekCanvasTime)
{
    if ( m_restoringMeasurementHistory || !m_measurementGestureBefore ) return;
    seekChanged = seekChanged || m_measurementHistorySeekChanged;
    // 主视图在一个窗口手势内聚合多个控件的副作用标识。
    viewChanged = viewChanged || m_measurementHistoryViewChanged;
    if ( !explicitSeekCanvasTime ) {
        explicitSeekCanvasTime = m_measurementHistorySeekCanvasTime;
    }
    m_measurementHistorySeekChanged = false;
    m_measurementHistoryViewChanged = false;
    m_measurementHistorySeekCanvasTime.reset();
    MeasurementHistoryEntry entry;
    entry.before      = std::move(*m_measurementGestureBefore);
    entry.after       = captureMeasurementHistorySnapshot();
    entry.seekChanged = seekChanged;
    entry.viewChanged = viewChanged;
    m_measurementGestureBefore.reset();
    // 后续早退也不会把半完成的事务留给下一次输入。
    // 切换音轨会废弃整个历史，未完成事务绝不能跨身份提交。
    if ( entry.before.audioTrackId != entry.after.audioTrackId ) return;
    if ( explicitSeekCanvasTime ) {
        // CmdSeek 异步执行，提交时必须保存用户目标而非旧后端播放头。
        // 此值只由人工 seek 提供，自动播放不得覆盖它。
        entry.after.seekCanvasTime = *explicitSeekCanvasTime;
    }

    const auto sameSegments = [](const auto& lhs, const auto& rhs) {
        // 归一化后的列表逐段比较，避免重复点击产生空历史。
        if ( lhs.size() != rhs.size() ) return false;
        // 列表已归一化，逐段精确比较可避免重复点击同位置生成空历史。
        for ( std::size_t i = 0; i < lhs.size(); ++i ) {
            if ( lhs[i].timestampSeconds != rhs[i].timestampSeconds ||
                 lhs[i].bpm != rhs[i].bpm ) {
                return false;
            }
        }
        return true;
    };
    const bool measurementChanged =
        // 兼容字段与段落同时检查，防止旧控件编辑漏记。
        !sameSegments(entry.before.timingSegments,
                      entry.after.timingSegments) ||
        entry.before.bpm != entry.after.bpm ||
        entry.before.beatLengthSeconds != entry.after.beatLengthSeconds ||
        entry.before.firstBeatTime != entry.after.firstBeatTime ||
        entry.before.markerWidthMs != entry.after.markerWidthMs ||
        entry.before.beatDivisor != entry.after.beatDivisor ||
        entry.before.playbackSpeed != entry.after.playbackSpeed;
    const bool changedView =
        // 自动跟随改变中心，但只有用户视野操作才可撤销。
        viewChanged && (entry.before.viewCenter != entry.after.viewCenter ||
                        entry.before.zoomSeconds != entry.after.zoomSeconds);
    const bool changedSeek =
        // 微秒级噪声不作为新的用户跳转。
        seekChanged && std::isfinite(entry.after.seekCanvasTime) &&
        std::abs(entry.before.seekCanvasTime - entry.after.seekCanvasTime) >
            1e-6;
    if ( !measurementChanged && !changedView && !changedSeek ) return;
    // 无变化不截断已有重做操作序列。

    // 仅真实新操作截断 redo 分支；无变化手势不破坏已有重做历史。
    m_measurementRedoHistory.clear();
    if ( m_measurementUndoHistory.size() == MAX_MEASUREMENT_HISTORY ) {
        // 最旧快照超出可访问深度，先丢弃再追加。
        m_measurementUndoHistory.erase(m_measurementUndoHistory.begin());
    }
    m_measurementUndoHistory.push_back(std::move(entry));
}

/// @brief 清除当前音轨的所有工具历史和未完成输入事务。
/// @warning 低频音轨切换路径：不恢复旧音轨状态，也不触碰活动谱面。
/// @note 切轨前未完成的拖动直接取消，不能提交到新音轨历史。
/// 清理不是可撤销事务，redo 也不能跨音轨保留。
void BpmMeasurementToolView::clearMeasurementHistory()
{
    m_measurementGestureBefore.reset();
    m_measurementHistorySeekChanged = false;
    m_measurementHistoryViewChanged = false;
    m_measurementHistorySeekCanvasTime.reset();
    m_measurementUndoHistory.clear();
    m_measurementRedoHistory.clear();
}

/// @brief 将工具历史快照恢复为当前测量状态。
/// @param snapshot 目标快照，必须属于当前音轨。
/// @param seekChanged 是否恢复用户明确提交的 seek。
/// @param viewChanged 是否恢复用户明确修改的视图。
/// @warning 低频撤销重做路径：可发出播放命令，但不编辑谱面 Timing。
/// @details 先恢复本地参数，再通过工具既有倍速与 seek 路由发命令。
/// seek 会改变视图中心，显式视图状态必须在这些动作之后恢复。
/// 普通参数编辑不恢复旧视野，避免覆盖播放自动跟随的位置。
/// 状态恢复期间禁止新的历史事务，只更新内存偏好待低频保存。
void BpmMeasurementToolView::restoreMeasurementHistorySnapshot(
    const MeasurementHistorySnapshot& snapshot, bool seekChanged,
    bool viewChanged)
{
    if ( snapshot.audioTrackId != m_selectedAudioTrackId ) return;
    // 旧音轨快照不能应用到另一首音频的时间轴。
    // 完整还原段落和兼容字段，避免只还原首段使多段测量失配。
    m_restoringMeasurementHistory = true;
    m_timingSegments              = snapshot.timingSegments;
    // 三个兼容字段和段落来自同一次快照，保留原始精度。
    m_bpm               = snapshot.bpm;
    m_beatLengthSeconds = snapshot.beatLengthSeconds;
    m_firstBeatTime     = snapshot.firstBeatTime;
    m_markerWidthMs     = snapshot.markerWidthMs;
    m_beatDivisor       = snapshot.beatDivisor;
    // 普通参数编辑不应把播放自动跟随期间的视图移动撤销掉。
    if ( viewChanged || seekChanged ) {
        // 只有显式视图动作或 seek 才可回放旧中心位置。
        m_zoomSeconds = snapshot.zoomSeconds;
        m_viewCenter  = snapshot.viewCenter;
    }
    // 快照来自已提交 UI 状态；空分析列表也要原样恢复，不能生成新段。
    // 普通参数撤销不应重置拉伸器；倍率确实变化才同步播放后端。
    if ( m_playbackSpeed != snapshot.playbackSpeed ) {
        applyPlaybackSpeed(snapshot.playbackSpeed);
    }
    if ( seekChanged && std::isfinite(snapshot.seekCanvasTime) ) {
        // 跳转沿用原有同步或试听路由，不直写编辑器状态。
        seekPlaybackToCanvasTime(snapshot.seekCanvasTime);
    }
    // seek 会把视图中心移到播放头，显式视图状态须最后恢复。
    if ( viewChanged || seekChanged ) {
        // seek 自动居中后，以历史记录的视图状态作为最终结果。
        m_viewCenter  = snapshot.viewCenter;
        m_zoomSeconds = snapshot.zoomSeconds;
    }
    resetMetronomeScheduler(m_viewCenter);
    markUserPreferencesChanged(true, true);
    m_restoringMeasurementHistory = false;
}

/// @brief 查询是否存在当前音轨可撤销的测量操作。
/// @return 撤销栈非空时返回 true。
/// @warning UI 快捷键低频查询；只检查容器大小，不复制条目。
bool BpmMeasurementToolView::canUndoMeasurement() const
{
    return !m_measurementUndoHistory.empty();
}

/// @brief 查询是否存在当前音轨可重做的测量操作。
/// @return 重做栈非空时返回 true。
/// @warning UI 快捷键低频查询；只检查容器大小，不复制条目。
bool BpmMeasurementToolView::canRedoMeasurement() const
{
    return !m_measurementRedoHistory.empty();
}

/// @brief 撤销一次已提交的 BPM 工具输入操作。
/// @warning 用户快捷键路径：历史为空时保持无副作用，按键由上层消费。
/// @note 条目从 undo 移到 redo，不重新捕获正在推进的播放头。
/// 多次撤销重做往返仍使用原手势提交时的 seek 目标。
void BpmMeasurementToolView::undoMeasurementFromShortcut()
{
    if ( m_measurementUndoHistory.empty() ) return;
    auto entry = std::move(m_measurementUndoHistory.back());
    // 先弹出再恢复，恢复过程不会访问同一尾项。
    m_measurementUndoHistory.pop_back();
    restoreMeasurementHistorySnapshot(
        entry.before, entry.seekChanged, entry.viewChanged);
    m_measurementRedoHistory.push_back(std::move(entry));
}

/// @brief 重做一次已撤销的 BPM 工具输入操作。
/// @warning 用户快捷键路径：历史为空时保持无副作用，按键由上层消费。
/// @note 使用冻结的 after 快照，不重新执行鼠标输入或重新计算 BPM。
void BpmMeasurementToolView::redoMeasurementFromShortcut()
{
    if ( m_measurementRedoHistory.empty() ) return;
    auto entry = std::move(m_measurementRedoHistory.back());
    // 与撤销对称移动同一条目，保留多步顺序和副作用标识。
    m_measurementRedoHistory.pop_back();
    restoreMeasurementHistorySnapshot(
        entry.after, entry.seekChanged, entry.viewChanged);
    m_measurementUndoHistory.push_back(std::move(entry));
}

/// @brief 在视图中心插入继承当前位置 BPM 的新段落。
/// @warning 仅配置快捷键触发；归一化与历史提交都属于低频用户操作。
/// @note 新段继承视野中心所在段的 BPM，时间按画布时长钳制。
/// 面板按钮可共用此入口，同位置合并为原状态时不写空历史。
void BpmMeasurementToolView::addSegmentAtViewCenterFromShortcut()
{
    beginMeasurementHistoryGesture();
    // 先确保基准段存在，再从视图中心所在段继承 BPM。
    ensureTimingSegments();
    // 索引查找需要非空基准段，未分析状态也遵守此不变量。
    const std::size_t sourceIndex = findSegmentIndexForTime(m_viewCenter);
    const double      bpm         = m_timingSegments[sourceIndex].bpm;
    m_timingSegments.push_back(
        { std::clamp(m_viewCenter, 0.0, playbackCanvasDuration()), bpm });
    normalizeTimingSegments();
    // 节拍器依赖段边界，新增段后立即重建调度游标。
    resetMetronomeScheduler(m_viewCenter);
    finishMeasurementHistoryGesture();
}

}  // namespace MMM::UI
