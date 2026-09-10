#include "canvas/AnnotationTableData.h"

#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace MMM::Canvas
{
/// @brief 从活动会话的版本化缓存增量刷新批注表数据。
/// @return 当前数据状态以及表格行是否被新快照替换。
///
/// 批注表拥有独立于时间线窗口的缓存。这里仅借用活动会话已经准备好的
/// 批注渲染缓存和滚动缓存，不要求任一辅助窗口处于打开状态。版本号没有
/// 变化时保留现有容器，避免 UI 轮询期间反复复制正文和重新分配字符串。
///
/// @warning 该函数会短暂持有会话递归锁。调用方必须按低频刷新间隔调用，
/// 不得把它放进每个表格行或每个画布物件的绘制循环。
AnnotationTableDataRefreshResult AnnotationTableData::refresh()
{
    // 活动会话指针及其上下文都受同一把锁保护。锁内完成版本检查和复制，
    // 防止逻辑线程在生成新缓存时留下跨版本的行数据与 Timing 上下文。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    const auto*                           activeEntry =
        engine.getSessionEntry(engine.getActiveSessionIndex());
    // Logo 占位会话没有可供批注表消费的谱面；Close 让窗口统一走关闭流程。
    if ( !activeEntry || activeEntry->isLogoPlaceholder ||
         !activeEntry->session ) {
        return { AnnotationTableDataStatus::Close, false };
    }

    const auto& context = activeEntry->session->getContext();
    // 会话切换过程中 currentBeatmap 可能短暂为空，此时不能沿用旧项目数据。
    if ( !context.currentBeatmap ) {
        return { AnnotationTableDataStatus::Close, false };
    }

    // 地址只作为当前进程内的实例令牌，不会持久化也不会解引用。即使两个
    // 谱面版本号相同，只要实例发生切换，也必须丢弃上一谱面的全部缓存。
    const auto beatmapInstanceId =
        reinterpret_cast<std::uintptr_t>(context.currentBeatmap.get());
    if ( beatmapInstanceId != m_beatmapInstanceId ) {
        reset();
        m_beatmapInstanceId = beatmapInstanceId;
    }

    // 脏标记表示逻辑侧正在等待重建批注缓存。此时保留旧行并返回 Pending，
    // 避免窗口把半成品当成空数据，从而清除用户当前选中的批注。
    if ( context.isAnnotationRenderCacheDirty ) {
        return { AnnotationTableDataStatus::Pending, false };
    }

    bool rowsChanged = false;
    if ( context.annotationRenderCacheRevision != m_annotationRevision ) {
        // 先在临时容器中构造完整结果，再一次性交换。这样旧数据要么完整
        // 保留，要么被完整替换，窗口不会观察到逐项追加的中间状态。
        // 表格数据刻意复制必要字段：它不持有 ECS 实体、谱面注解对象或
        // 渲染缓存元素的引用，因此会话下一次重建缓存后仍可安全绘制本帧。
        std::vector<AnnotationTableRow> refreshedRows;
        refreshedRows.reserve(context.currentBeatmap->m_annotations.size());
        // 渲染缓存已经按实际时间组织 marker；同一时间戳下的多个批注仍要
        // 各自成为独立表格行，不能把正文或目标信息折叠到 marker 层级。
        for ( const auto& marker : context.annotationRenderCache ) {
            for ( const auto& item : marker.items ) {
                refreshedRows.push_back({
                    .timestamp     = marker.timestamp,
                    .id            = item.id,
                    .targetKind    = item.targetKind,
                    .track         = item.track,
                    .targetMissing = item.targetMissing,
                    .author        = item.author,
                    .content       = item.content,
                });
            }
        }
        // 交换保持刷新前后边界清晰，同时让旧字符串随临时容器统一释放。
        m_rows.swap(refreshedRows);
        m_annotationRevision = context.annotationRenderCacheRevision;
        rowsChanged          = true;
    }

    // 分拍数不属于 ScrollCache 的 Timing 修订号，因此单独参与刷新判断。
    const int beatDivisor =
        std::max(1, context.lastConfig.settings.beatDivisor);
    const auto* scrollCache =
        context.timelineRegistry.ctx().find<Logic::System::ScrollCache>();
    // 脏的滚动缓存可能含有旧分段，宁可暂时沿用上次格式上下文，也不能
    // 让表格时间文本与画布当前的 BPM 分段出现瞬时错配。
    if ( scrollCache && !scrollCache->isDirty &&
         (scrollCache->getRevision() != m_timingRevision ||
          beatDivisor != m_beatDivisor) ) {
        UI::Utils::CanvasTimeFormatContext refreshedContext;
        // 时间格式上下文只保存显示所需的轻量 BPM 点，不复制滚动曲线、
        // SV 倍率或时间线实体，从数据结构上维持与时间线表窗口的解耦。
        refreshedContext.beatDivisor = beatDivisor;
        const auto& segments         = scrollCache->getSegments();
        refreshedContext.bpmPoints.reserve(segments.size());
        // 只有真实 BPM 实体能定义时间格式锚点；纯 SV 分段沿用此前 BPM，
        // 若把它们也加入会在同一拍点产生虚假的节奏切换。
        for ( const auto& segment : segments ) {
            if ( segment.bpmEntity == entt::null ) continue;
            refreshedContext.bpmPoints.push_back(
                { segment.time, ::MMM::normalizeBpmValue(segment.bpmValue) });
        }
        // ScrollCache 面向滚动计算，不把排序与同时间去重作为公开契约；
        // 时间格式器则要求稳定递增锚点，因此在复制边界显式规范化。
        std::sort(refreshedContext.bpmPoints.begin(),
                  refreshedContext.bpmPoints.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.time < rhs.time;
                  });
        // 浮点时间允许微小构建误差。保留排序后的第一项，使同一时刻只
        // 参与一次拍号换算，避免表格文本在两个近似锚点之间抖动。
        refreshedContext.bpmPoints.erase(
            std::unique(refreshedContext.bpmPoints.begin(),
                        refreshedContext.bpmPoints.end(),
                        [](const auto& lhs, const auto& rhs) {
                            return std::abs(lhs.time - rhs.time) < 1e-6;
                        }),
            refreshedContext.bpmPoints.end());
        // 完整生成后再发布上下文，保证 UI 读取到的 beatDivisor 与锚点同代。
        m_timeFormatContext = std::move(refreshedContext);
        m_timingRevision    = scrollCache->getRevision();
        m_beatDivisor       = beatDivisor;
    }

    // 批注行 Ready 与 Timing 是否存在无关：没有滚动缓存时仍可用秒数显示，
    // 待缓存准备好后再通过独立修订号补齐拍号格式上下文。
    return { AnnotationTableDataStatus::Ready, rowsChanged };
}

/// @brief 解除当前谱面绑定并恢复首次刷新哨兵值。
///
/// 修订号使用最大值而不是零，因为合法缓存的首个修订号可能就是零；这样
/// reset 后首次遇到任意正常版本都会执行完整复制。
void AnnotationTableData::reset()
{
    // 先清除身份与版本，再释放数据，确保之后的 refresh 不会错误复用
    // 上一谱面的行或时间格式上下文。
    m_beatmapInstanceId  = 0U;
    m_annotationRevision = std::numeric_limits<std::uint64_t>::max();
    m_timingRevision     = std::numeric_limits<std::uint64_t>::max();
    m_beatDivisor        = 4;
    m_rows.clear();
    // 保留一个有效的默认分拍数，让无 BPM 点的空上下文仍可安全格式化。
    m_timeFormatContext.bpmPoints.clear();
    m_timeFormatContext.beatDivisor = 4;
}

}  // namespace MMM::Canvas
