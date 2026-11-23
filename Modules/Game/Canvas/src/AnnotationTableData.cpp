#include "canvas/AnnotationTableData.h"

#include "logic/AnnotationTableSnapshot.h"
#include "ui/utils/CanvasTimeFormatContext.h"

#include <utility>
#include <vector>

namespace MMM::Canvas
{

/// @brief 消费活动会话的最新完整发布，行与时间格式随各自版本更新。
/// @return Close 表示没有谱面；Pending 表示新谱面的第一份数据还在逻辑侧准备。
/// @note 切换期间回收 UI 自持的快照，其他读者可以继续使用同一代数据。
/// @note 播放标记与协作许可共同决定按钮可用性，修改命令仍由目标会话复核。
/// @warning UI 辅助窗口低频轮询。持有共享句柄跨过逻辑发布边界，无会话锁等待；
/// 仅新 Timing 才遍历 BPM，普通刷新不遍历或复制批注行。
AnnotationTableDataRefreshResult AnnotationTableData::refresh()
{
    auto active = Logic::readActiveAnnotationTable();
    // 此时注册表已撤回活动谱面，包括关闭最后一个标签和切到 Logo。
    // 不保留上次实例身份，否则下一次打开批注表可能复用旧草稿。
    if ( !active ) {
        reset();
        return { AnnotationTableDataStatus::Close, false };
    }
    // 切换到尚未发布的谱面时先撤回旧行和编辑许可。旧行不能用于新会话的命令。
    if ( active->cameraId != m_cameraId ) {
        reset();
        m_cameraId = std::move(active->cameraId);
    }
    // 在逻辑线程处理请求之前，UI 可以绘制加载状态，不能把它当成空表关闭。
    if ( !active->snapshot )
        return { AnnotationTableDataStatus::Pending, false };

    const auto& snapshot = *active->snapshot;
    // 非空发布带零标识，表示逻辑侧确认没有载入谱面。
    if ( !snapshot.instanceId ) {
        reset();
        return { AnnotationTableDataStatus::Close, false };
    }
    // 同一画布也会重新载入；比较独立实例身份，不能只靠缓存修订号。
    if ( snapshot.instanceId != m_beatmapInstanceId ) {
        m_snapshot.reset();
        // 同一个停靠标签可以重新载入其他谱面，旧 BPM 不能作为新数据的缺省值。
        // 此处只丢弃本窗口缓存；导出或其他读者持有的快照继续有效。
        m_timeFormatContext = {};
        m_beatmapInstanceId = snapshot.instanceId;
    }
    m_writable = !snapshot.playing && !active->offlineReadOnly;
    // 按共享容器身份比较，播放标志变化的发布不会使平台重算整表。
    const bool rowsChanged = !m_snapshot || snapshot.rows != m_snapshot->rows;
    const bool timingChanged =
        !m_snapshot || snapshot.timing != m_snapshot->timing;
    // 空 Timing 代表当前谱面尚无滚动缓存，只在载入新实例时撤回旧锚点。
    if ( timingChanged && snapshot.timing ) {
        // 时间格式器的上下文由 UI 持有；只在逻辑发布新锚点时转换类型。
        auto& context       = m_timeFormatContext;
        context.beatDivisor = snapshot.timing->beatDivisor;
        context.bpmPoints.clear();
        context.bpmPoints.reserve(snapshot.timing->bpmPoints.size());
        for ( const auto& point : snapshot.timing->bpmPoints )
            context.bpmPoints.push_back({ point.time, point.bpm });
    }
    // 行引用的有效期与窗口自己的快照绑定；旧数据在本次提交后才释放。
    // 单独的显示修订号保证 BPM 变化也能刷新派生文本。
    m_snapshot = std::move(active->snapshot);
    if ( rowsChanged || timingChanged ) ++m_displayRevision;
    return { AnnotationTableDataStatus::Ready, rowsChanged };
}

/// @brief 返回快照拥有的行，等待新会话发布时返回共享空容器。
/// @return 可借用到下次数据刷新或 reset；跨版本定位应保存行的稳定 ID。
/// @warning 窗口绘制只读取已有句柄，禁止在这里查询逻辑状态。
const std::vector<AnnotationTableRow>& AnnotationTableData::rows() const
{
    // 等待时返回不可变空容器，平台呈现不必为未发布状态构造临时集合。
    static const std::vector<AnnotationTableRow> empty;
    return m_snapshot && m_snapshot->rows ? *m_snapshot->rows : empty;
}

/// @brief 撤回当前表格数据，游戏会话及其他读者的生命周期彼此独立。
/// @post 下一次成功发布会重新建立行、时间格式和新实例的编辑许可。
void AnnotationTableData::reset()
{
    // UI 缓存的代数也推进，确保触控派生文本不会复用旧行号。
    ++m_displayRevision;
    // 权限必须和实例同时清空。窗口重开期间不会沿用旧谱面的可写状态。
    m_writable          = false;
    m_beatmapInstanceId = 0;
    m_cameraId.clear();
    m_snapshot.reset();
    m_timeFormatContext = {};
}

}  // namespace MMM::Canvas
