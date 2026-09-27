#include "canvas/AnnotationTableData.h"

#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
/// @details 渲染缓存是批注表与主画布共享的版本化读模型。
/// 当批注目标跟随物件移动时，该缓存已经提供新的实际时间和轨道。
/// 导出所需的类型与终轨在相同锁区读取，避免另一次导出时遍历 ECS。
/// 复制完成后，批注表和导出器都只访问自持有的字符串和值字段。
/// 目标解析失败时保留正文，目标类型由导出层显示为已丢失。
/// 草稿区使用负轨道编号，导出层不会把它错误转成正轨道。
/// BGM 采样轨来自统一轨道坐标，导出时换算为 BGM 区局部编号。
/// 子折线物件带有父实体及子索引，不能直接读取父根的物件类型。
/// 缓存修订号与 Timing 修订号分别检查，减少未变化时的拷贝。
/// 所有会话引用都止于当前锁区，窗口绘制阶段不再解引用实体。
/// 导出附加元数据只在行版本变化时构造，不增加每帧绘制成本。
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
                AnnotationTableRow row{
                    .timestamp     = marker.timestamp,
                    .id            = item.id,
                    .targetKind    = item.targetKind,
                    .track         = item.track,
                    .exportTrack   = item.track,
                    .targetMissing = item.targetMissing,
                    .author        = item.author,
                    .content       = item.content,
                };
                // 缓存保存父折线实体和子段索引；在持锁的版本刷新边界读取
                // 目标类型与终轨，导出时不再访问可能变化的 Registry。
                // 同一实体可能是普通物件或折线根，子段类型需按索引覆盖。
                // 若索引无效则退回根类型，绝不越界访问子段数组。
                if ( item.targetKind ==
                         ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT &&
                     !item.targetMissing && item.targetEntity != entt::null ) {
                    const auto* note = context.noteRegistry
                                           .try_get<const Logic::NoteComponent>(
                                               item.targetEntity);
                    if ( note ) {
                        ::MMM::NoteType type   = note->m_type;
                        std::int32_t    dtrack = note->m_dtrack;
                        if ( item.targetSubIndex >= 0 &&
                             static_cast<std::size_t>(item.targetSubIndex) <
                                 note->m_subNotes.size() ) {
                            // 批注附着的是子段本身，其轨道和 Flick 位移由
                            // 渲染缓存的目标轨与子段定义共同决定。
                            const auto& sub =
                                note->m_subNotes[static_cast<std::size_t>(
                                    item.targetSubIndex)];
                            type                  = sub.type;
                            dtrack                = sub.dtrack;
                            row.isPolylineSubNote = true;
                        }
                        switch ( type ) {
                        case ::MMM::NoteType::NOTE:
                            row.objectType = AnnotationExportObjectType::Note;
                            break;
                        case ::MMM::NoteType::HOLD:
                            row.objectType = AnnotationExportObjectType::Hold;
                            break;
                        case ::MMM::NoteType::FLICK:
                            // dtrack 是相对起始轨的偏移，可以为负。
                            row.objectType = AnnotationExportObjectType::Flick;
                            row.exportEndTrack = row.exportTrack + dtrack;
                            break;
                        case ::MMM::NoteType::POLYLINE:
                            row.objectType =
                                AnnotationExportObjectType::Polyline;
                            // 整条折线以最终节点的落轨描述路径跨度。
                            // 最后节点仍可能是横向 Flick，终轨要包含其位移。
                            // 折线内部弯折无法用两端轨道完全表示；正文仍保留
                            // 原批注细节，目标描述只承担快速定位用途。
                            if ( !note->m_subNotes.empty() ) {
                                const auto& tail = note->m_subNotes.back();
                                row.exportEndTrack =
                                    tail.trackIndex +
                                    (tail.type == ::MMM::NoteType::FLICK
                                         ? tail.dtrack
                                         : 0);
                            }
                            break;
                        default: break;
                        }
                    }
                } else if ( item.targetKind ==
                                ::MMM::BeatmapAnnotationTargetKind::
                                    AUDIO_SAMPLE &&
                            item.track >= context.trackCount ) {
                    // 自动采样使用统一绝对轨道编号，导出改用 BGM 区局部编号。
                    // 负轨和主画布轨不进入此分支，避免跨区域编号混用。
                    row.exportTrack = item.track - context.trackCount;
                }
                refreshedRows.push_back(std::move(row));
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
