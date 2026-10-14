#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include <algorithm>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 工具本地 BPM 历史上限，限制长期编辑保存的段落副本。
constexpr std::size_t MAX_MEASUREMENT_HISTORY = 128;
}  // namespace

/// @brief 只捕获 BPM 段落与兼容测量字段，不捕获视野、播放头和显示偏好。
/// @return 与当前选中音轨绑定的测量数据副本。
/// @warning 仅用户输入边界复制段落列表，禁止每帧无操作时捕获。
BpmMeasurementToolView::MeasurementHistorySnapshot
BpmMeasurementToolView::captureMeasurementHistorySnapshot() const
{
    MeasurementHistorySnapshot snapshot;
    // 音轨身份保护历史，不能把另一首歌曲的 BPM 段落恢复到当前音轨。
    snapshot.audioTrackId   = m_selectedAudioTrackId;
    snapshot.timingSegments = m_timingSegments;
    // 三个字段与段落一同恢复，保持旧控件和多段测量的数据一致。
    snapshot.bpm               = m_bpm;
    snapshot.beatLengthSeconds = m_beatLengthSeconds;
    snapshot.firstBeatTime     = m_firstBeatTime;
    return snapshot;
}

/// @brief 连续操作只冻结一次 BPM 起点，鼠标拖动不会产生逐帧历史。
/// @warning 用户输入边界复制测量段落，不执行音频或文件操作。
void BpmMeasurementToolView::beginMeasurementHistoryGesture()
{
    // 撤销恢复和已有手势不能重入捕获，保留最早的用户编辑状态。
    if ( m_restoringMeasurementHistory || m_measurementGestureBefore ) return;
    m_measurementGestureBefore = captureMeasurementHistorySnapshot();
}

/// @brief 只有 BPM 增加、删除、数值或时间调整才生成撤销记录。
/// @warning 用户提交边界比较段落，不在每帧无输入路径遍历历史。
/// @note 导航、播放、倍速、显示宽度和分拍数都不构成测量数据修改。
void BpmMeasurementToolView::finishMeasurementHistoryGesture()
{
    if ( m_restoringMeasurementHistory || !m_measurementGestureBefore ) return;
    MeasurementHistoryEntry entry;
    entry.before = std::move(*m_measurementGestureBefore);
    entry.after  = captureMeasurementHistorySnapshot();
    // 提前消费事务，空操作或音轨切换早退后不能遗留半次编辑。
    m_measurementGestureBefore.reset();
    if ( entry.before.audioTrackId != entry.after.audioTrackId ) return;
    const auto sameSegments = [](const auto& lhs, const auto& rhs) {
        // 段落顺序已经归一化，逐项检查位置与 BPM，避免同位置点击生成空记录。
        if ( lhs.size() != rhs.size() ) return false;
        for ( std::size_t index = 0; index < lhs.size(); ++index ) {
            if ( lhs[index].timestampSeconds != rhs[index].timestampSeconds ||
                 lhs[index].bpm != rhs[index].bpm )
                return false;
        }
        return true;
    };
    // 段落首拍位置也是 BPM 编辑，调整首拍不能被当作普通播放跳转忽略。
    const bool changed =
        !sameSegments(entry.before.timingSegments,
                      entry.after.timingSegments) ||
        entry.before.bpm != entry.after.bpm ||
        entry.before.beatLengthSeconds != entry.after.beatLengthSeconds ||
        entry.before.firstBeatTime != entry.after.firstBeatTime;
    // 导航或无变化手势必须保留 redo，不能因为看了别处就失去可重做的 BPM。
    if ( !changed ) return;
    m_measurementRedoHistory.clear();
    if ( m_measurementUndoHistory.size() == MAX_MEASUREMENT_HISTORY ) {
        // 超出固定深度时仅丢弃最旧记录，不改变当前测量状态。
        m_measurementUndoHistory.erase(m_measurementUndoHistory.begin());
    }
    // 条目不含播放或导航参数，后续恢复不会产生额外音频传输命令。
    m_measurementUndoHistory.push_back(std::move(entry));
}

/// @brief 切换音轨时清空 BPM 历史与尚未结束的编辑手势。
/// @warning 低频身份切换路径，不恢复旧音轨或触碰当前播放。
void BpmMeasurementToolView::clearMeasurementHistory()
{
    m_measurementGestureBefore.reset();
    m_measurementUndoHistory.clear();
    m_measurementRedoHistory.clear();
}

/// @brief 恢复 BPM 段落，保留用户当前视野、播放位置、倍速与显示偏好。
/// @param snapshot 与当前音轨绑定的测量快照。
/// @warning 用户撤销路径只恢复工具数据，不发布 Seek 或播放速度命令。
void BpmMeasurementToolView::restoreMeasurementHistorySnapshot(
    const MeasurementHistorySnapshot& snapshot)
{
    // 失效音轨历史不能跨歌曲生效；视野导航始终属于用户当前上下文。
    if ( snapshot.audioTrackId != m_selectedAudioTrackId ) return;
    // 恢复期间只更改测量字段，不保存或覆盖显示偏好。
    m_restoringMeasurementHistory = true;
    m_timingSegments              = snapshot.timingSegments;
    m_bpm                         = snapshot.bpm;
    m_beatLengthSeconds           = snapshot.beatLengthSeconds;
    m_firstBeatTime               = snapshot.firstBeatTime;
    // BPM 改变后重建节拍调度基线，不把播放头跳回编辑前的位置。
    resetMetronomeScheduler(m_viewCenter);
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
/// 多次撤销重做始终使用冻结的 BPM 数据，播放头和视野不参与还原。
void BpmMeasurementToolView::undoMeasurementFromShortcut()
{
    if ( m_measurementUndoHistory.empty() ) return;
    auto entry = std::move(m_measurementUndoHistory.back());
    // 先弹出再恢复，恢复过程不会访问同一尾项。
    m_measurementUndoHistory.pop_back();
    restoreMeasurementHistorySnapshot(entry.before);
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
    restoreMeasurementHistorySnapshot(entry.after);
    // 条目不含播放或导航参数，后续恢复不会产生额外音频传输命令。
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
