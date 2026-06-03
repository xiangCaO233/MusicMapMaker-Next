#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/ActionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Logic
{

/// @brief 获取当前会话可用于轨道镜像的轨道数量。
int getMirrorTrackCount(const SessionContext& ctx)
{
    if ( ctx.currentBeatmap &&
         ctx.currentBeatmap->m_baseMapMetadata.track_count > 0 ) {
        return ctx.currentBeatmap->m_baseMapMetadata.track_count;
    }
    return ctx.trackCount;
}

/// @brief 对单个 NoteComponent 应用轨道镜像变换。
void mirrorNoteComponent(NoteComponent& note, int trackCount)
{
    if ( trackCount <= 0 ) return;

    note.m_trackIndex = (trackCount - 1) - note.m_trackIndex;
    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        note.m_dtrack = -note.m_dtrack;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( auto& sub : note.m_subNotes ) {
            sub.trackIndex = (trackCount - 1) - sub.trackIndex;
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                sub.dtrack = -sub.dtrack;
            }
        }
    }
}

/// @brief 将调色盘命令颜色转换为 NoteColorOverrides。
NoteColorOverrides makeNoteColorOverrides(
    const std::array<glm::vec4, NOTE_COLOR_SLOT_COUNT>& colors)
{
    NoteColorOverrides overrides;
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(overrides, slot, colors[i]);
    }
    return overrides;
}

/// @brief 将折线子物件点击目标解析到父折线实体。
entt::entity resolveNoteColorTargetEntity(SessionContext& ctx,
                                          entt::entity    entity)
{
    if ( entity == entt::null || !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        return entt::null;
    }

    const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
    if ( note.m_isSubNote && note.m_parentPolyline != entt::null &&
         ctx.noteRegistry.valid(note.m_parentPolyline) &&
         ctx.noteRegistry.all_of<NoteComponent>(note.m_parentPolyline) ) {
        return note.m_parentPolyline;
    }
    return entity;
}

/// @brief 判断两个可选颜色是否完全相同。
bool isSameOptionalColor(const std::optional<glm::vec4>& lhs,
                         const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    if ( !lhs.has_value() ) return true;
    return lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
           lhs->a == rhs->a;
}

/// @brief 判断两个音符配色覆写缓存是否完全相同。
bool isSameNoteColorOverrides(const NoteColorOverrides& lhs,
                              const NoteColorOverrides& rhs)
{
    return isSameOptionalColor(lhs.tap, rhs.tap) &&
           isSameOptionalColor(lhs.head, rhs.head) &&
           isSameOptionalColor(lhs.hold, rhs.hold) &&
           isSameOptionalColor(lhs.end, rhs.end) &&
           isSameOptionalColor(lhs.flickArrow, rhs.flickArrow) &&
           isSameOptionalColor(lhs.node, rhs.node);
}

// --- Editing Handlers ---

void ActionController::handleCommand(const CmdUndo& cmd)
{
    m_ctx.actionStack.undo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdRedo& cmd)
{
    m_ctx.actionStack.redo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdCopy& cmd)
{
    m_ctx.clipboard.clear();
    auto view = m_ctx.noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            const auto& note = view.get<NoteComponent>(entity);
            m_ctx.clipboard.push_back({ note });
        }
    }
    EditorEngine::instance().setClipboard(m_ctx.clipboard, &m_ctx, false);
    XINFO("Copied {} items to clipboard", m_ctx.clipboard.size());
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.copied"),
                                          m_ctx.clipboard.size(),
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdCut& cmd)
{
    handleCommand(CmdCopy{});
    auto view = m_ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            ic.isCut = true;
        }
    }
    EditorEngine::instance().setClipboard(m_ctx.clipboard, &m_ctx, true);
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.cut"),
                                          m_ctx.clipboard.size(),
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdDeleteSelected& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            entries.push_back(
                { entity, view.get<NoteComponent>(entity), std::nullopt });
        }
    }

    // 如果没有任何选中的，但有悬停的，也删除悬停的 (符合习惯)
    if ( entries.empty() && m_ctx.hoveredEntity != entt::null ) {
        if ( m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(m_ctx.hoveredEntity) ) {
            entries.push_back(
                { m_ctx.hoveredEntity,
                  m_ctx.noteRegistry.get<NoteComponent>(m_ctx.hoveredEntity),
                  std::nullopt });
        }
    }

    // 收集所有被删除实体的 ID（用于后续查找子物件）
    std::unordered_set<entt::entity> deletedEntities;
    for ( const auto& entry : entries ) {
        deletedEntities.insert(entry.entity);
    }

    // 同时删除被删除折线下所有子物件实体，防止孤儿子实体残留
    for ( entt::entity parentEntity : deletedEntities ) {
        if ( m_ctx.noteRegistry.valid(parentEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(parentEntity) ) {
            const auto& nc =
                m_ctx.noteRegistry.get<NoteComponent>(parentEntity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                 !nc.m_subNotes.empty() ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == parentEntity ) {
                        entries.push_back({ subEnt, subNC, std::nullopt });
                    }
                }
            }
        }
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                        "Delete Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Deleted {} selected/hovered items", count);
    }
}

void ActionController::handleCommand(const CmdMirrorSelected& cmd)
{
    if ( !m_ctx.currentBeatmap ) return;
    int trackCount = getMirrorTrackCount(m_ctx);

    std::vector<BatchNoteAction::Entry> entries;
    std::unordered_set<entt::entity>    toMirror;

    // 1. 收集所有选中的物件
    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            toMirror.insert(entity);

            // 如果是 Polyline，收集其所有子物件实体
            const auto& nc = view.get<NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        toMirror.insert(subEnt);
                    }
                }
            }
        }
    }

    // 2. 执行镜像逻辑
    for ( auto entity : toMirror ) {
        if ( !m_ctx.noteRegistry.valid(entity) ||
             !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
            continue;

        const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
        auto        newNote = oldNote;

        mirrorNoteComponent(newNote, trackCount);

        entries.push_back({ entity, oldNote, newNote });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                        "Mirror Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Mirrored {} items (including sub-notes)", count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.edit.mirror"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

void ActionController::handleCommand(const CmdApplyNoteColorToSelection& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote ) continue;

        auto newNote = oldNote;
        setNoteColorOverride(newNote, cmd.slot, cmd.color);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto actionName =
        cmd.color.has_value() ? "Set Note Color" : "Clear Note Color";
    auto action =
        std::make_unique<BatchNoteAction>(std::move(entries), actionName);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage =
        cmd.color.has_value() ? "Note color applied" : "Note color cleared";
}

void ActionController::handleCommand(const CmdApplyNotePaletteToSelection& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;
    auto colors = makeNoteColorOverrides(cmd.colors);

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote ) continue;

        auto newNote = oldNote;
        applyNoteColorOverrides(newNote, colors);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

void ActionController::handleCommand(const CmdApplyBrushPaletteToEntity& cmd)
{
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto& colors = m_ctx.brushState.customColors;
    if ( !hasAnyNoteColorOverride(colors) ) return;

    const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    auto        newNote = oldNote;
    applyNoteColorOverrides(newNote, colors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

void ActionController::handleCommand(const CmdClearNoteColorOverrides& cmd)
{
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto&        oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    auto               newNote = oldNote;
    NoteColorOverrides emptyColors;
    applyNoteColorOverrides(newNote, emptyColors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Clear Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette cleared";
}

void ActionController::handleCommand(const CmdPaste& cmd)
{
    auto clipboard = EditorEngine::instance().getClipboard();
    if ( clipboard.empty() ) {
        clipboard = m_ctx.clipboard;
    }
    if ( clipboard.empty() ) return;

    // 计算基准点 (目前取所有选中音符的最小时间)
    double minTime = clipboard[0].note.m_timestamp;
    for ( const auto& item : clipboard ) {
        minTime = std::min(minTime, item.note.m_timestamp);
    }

    std::vector<BatchNoteAction::Entry> entries;
    /// @brief 本次粘贴预先分配的新实体 ID 列表，用于动作执行后选中新物件。
    std::vector<entt::entity> pastedEntities;
    pastedEntities.reserve(clipboard.size());
    /// @brief 是否在粘贴完成后只保留新粘贴物件为选中状态。
    const bool selectPastedObjects = cmd.m_selectPastedObjects;

    // 1. 如果之前有 Cut，需要删除那些 Cut 的物件
    auto view       = m_ctx.noteRegistry.view<InteractionComponent>();
    bool isLocalCut = EditorEngine::instance().isClipboardCutFrom(&m_ctx);
    if ( isLocalCut ) {
        for ( auto entity : view ) {
            auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
            if ( ic.isCut ) {
                if ( !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    continue;
                }

                auto oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
                entries.push_back({ entity, oldNote, std::nullopt });

                // 同时删除 Polyline 的子物件实体
                if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                     !oldNote.m_subNotes.empty() ) {
                    for ( auto subEnt :
                          m_ctx.noteRegistry.view<NoteComponent>() ) {
                        const auto& subNC =
                            m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == entity ) {
                            entries.push_back({ subEnt, subNC, std::nullopt });
                        }
                    }
                }
            }
        }
    } else {
        EditorEngine::instance().consumeCrossSessionCutClipboard(&m_ctx);
        for ( auto entity : view ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
        }
    }

    // 2. 粘贴到当前视觉时间 (判定线)
    double pasteTime = m_ctx.visualTime;

    // 尝试获取鼠标悬停处的时间作为基准 (如果有)
    // 注意：这里为了简化直接使用视觉时间。如果需要鼠标对齐，需要 UI 传入坐标。

    double timeOffset = pasteTime - minTime;

    int mirrorTrackCount = cmd.m_mirrored ? getMirrorTrackCount(m_ctx) : 0;
    for ( const auto& item : clipboard ) {
        auto newNote        = item.note;
        newNote.m_timestamp = item.note.m_timestamp + timeOffset;

        // 折线物件：同步偏移所有子物件的时间戳
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( auto& sub : newNote.m_subNotes ) {
                sub.timestamp += timeOffset;
            }
        }

        if ( cmd.m_mirrored ) {
            mirrorNoteComponent(newNote, mirrorTrackCount);
        }

        /// @brief 为新粘贴物件预分配实体，避免执行后再从撤销栈动作反查实体。
        entt::entity pastedEntity = m_ctx.noteRegistry.create();
        pastedEntities.push_back(pastedEntity);
        entries.push_back({ pastedEntity, std::nullopt, newNote });
    }

    auto action = std::make_unique<BatchNoteAction>(
        std::move(entries), cmd.m_mirrored ? "Mirror Paste" : "Paste");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

    if ( selectPastedObjects ) {
        m_ctx.isSelecting         = false;
        m_ctx.hasMarqueeSelection = false;
        m_ctx.marqueeIsAdditive   = false;
        m_ctx.marqueeBoxes.clear();
        // 先清空所有旧选择，再只选中本次粘贴创建出的实体。
        for ( auto entity : m_ctx.noteRegistry.view<InteractionComponent>() ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                false;
        }

        for ( auto entity : pastedEntities ) {
            if ( !m_ctx.noteRegistry.valid(entity) ||
                 !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) )
                continue;

            m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                true;
        }

        m_ctx.isTransformDirty = true;
    }

    // 清除剪切状态
    for ( auto entity : view ) {
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
    }
    if ( isLocalCut ) {
        EditorEngine::instance().markCutClipboardConsumed();
    }
}

// --- Timeline Handlers ---

void ActionController::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto newTl = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

void ActionController::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl  = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Delete, cmd.entity, oldTl, std::nullopt);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

void ActionController::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    TimelineComponent newTl{ cmd.time, cmd.type, cmd.value };
    auto              action = std::make_unique<TimelineAction>(
        TimelineAction::Type::Create, entt::null, std::nullopt, newTl);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdAlignSelectedToCommonBeats& cmd)
{
    if ( !m_ctx.currentBeatmap ) return;

    // Helper function to extract common beat divisors from the skin
    auto getCommonDivisorsFromSkin = []() -> std::vector<int> {
        return MMM::Config::SkinManager::instance().getCommonDivisors();
    };

    std::vector<int> commonDivisors = getCommonDivisorsFromSkin();

    // Gather BPM/Timing events
    auto tlView = m_ctx.timelineRegistry.view<const TimelineComponent>();
    std::vector<const TimelineComponent*> bpmEvents;
    for ( auto entity : tlView ) {
        const auto& tc = tlView.get<const TimelineComponent>(entity);
        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tc);
        }
    }
    std::stable_sort(
        bpmEvents.begin(), bpmEvents.end(), [](const auto* a, const auto* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    auto getAlignedTime = [&](double rawTime) -> double {
        if ( bpmEvents.empty() ) return rawTime;

        /// @brief 首个 BPM 前是否允许按绘制出的前置分拍线对齐。
        const bool allowBeforeFirstTiming =
            m_ctx.lastConfig.visual.drawBeatLinesBeforeFirstTiming;
        if ( rawTime < bpmEvents[0]->m_timestamp && !allowBeforeFirstTiming )
            return rawTime;

        double bestSnappedTime  = rawTime;
        double minWeightedError = std::numeric_limits<double>::max();

        // Find the timing event containing rawTime
        const TimelineComponent* currentBPM = nullptr;
        double                   bpmTime    = 0.0;
        double                   bpmVal     = 120.0;
        double nextBpmTime = std::numeric_limits<double>::infinity();

        if ( rawTime < bpmEvents.front()->m_timestamp ) {
            currentBPM  = bpmEvents.front();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = bpmEvents.size() > 1
                              ? bpmEvents[1]->m_timestamp
                              : std::numeric_limits<double>::infinity();
        } else {
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                double tBpm  = bpmEvents[i]->m_timestamp;
                double tNext = (i + 1 < bpmEvents.size())
                                   ? bpmEvents[i + 1]->m_timestamp
                                   : std::numeric_limits<double>::infinity();
                if ( rawTime >= tBpm && rawTime < tNext ) {
                    currentBPM  = bpmEvents[i];
                    bpmTime     = tBpm;
                    bpmVal      = currentBPM->m_value;
                    nextBpmTime = tNext;
                    break;
                }
            }
        }

        if ( !currentBPM ) {
            currentBPM  = bpmEvents.back();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = std::numeric_limits<double>::infinity();
        }

        double bVal = bpmVal;
        if ( bVal <= 0.0 ) {
            bVal = 120.0;
            if ( auto session = EditorEngine::instance().getActiveSession() ) {
                if ( auto beatmap = session->getContext().currentBeatmap ) {
                    if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                        bVal = beatmap->m_baseMapMetadata.preference_bpm;
                    }
                }
            }
        }

        double beatDuration = 60.0 / bVal;

        for ( int divisor : commonDivisors ) {
            if ( divisor <= 0 ) continue;
            double stepDuration    = beatDuration / divisor;
            double relativeTime    = rawTime - bpmTime;
            double stepCount       = std::round(relativeTime / stepDuration);
            double nearestStepTime = bpmTime + stepCount * stepDuration;

            if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;

            double error         = std::abs(rawTime - nearestStepTime);
            double weightedError = error * static_cast<double>(divisor);

            if ( weightedError < minWeightedError ) {
                minWeightedError = weightedError;
                bestSnappedTime  = nearestStepTime;
            }
        }

        return bestSnappedTime;
    };

    auto alignNote = [&](NoteComponent& nc) {
        double alignedStart = getAlignedTime(nc.m_timestamp);
        double alignedEnd   = getAlignedTime(nc.m_timestamp + nc.m_duration);
        nc.m_timestamp      = alignedStart;
        nc.m_duration       = std::max(0.0, alignedEnd - alignedStart);
    };

    std::unordered_set<entt::entity> toAlign;
    auto                             noteView =
        m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : noteView ) {
        const auto& ic = noteView.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            toAlign.insert(entity);
        }
    }

    // 闭包扩展：若 parent 在 toAlign 中，则其所有 subNotes 必须都在 toAlign
    // 中； 若任一 subNote 在 toAlign 中，则其 parent 及其所有 sibling subNotes
    // 必须都在 toAlign 中。
    bool expanded = true;
    while ( expanded ) {
        expanded = false;
        std::vector<entt::entity> currentToAlign(toAlign.begin(),
                                                 toAlign.end());
        for ( auto entity : currentToAlign ) {
            if ( !m_ctx.noteRegistry.valid(entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
                continue;

            const auto& nc = m_ctx.noteRegistry.get<NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        if ( toAlign.insert(subEnt).second ) {
                            expanded = true;
                        }
                    }
                }
            } else if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                if ( toAlign.insert(nc.m_parentPolyline).second ) {
                    expanded = true;
                }
            }
        }
    }

    if ( toAlign.empty() ) return;

    std::vector<BatchNoteAction::Entry>             entries;
    std::unordered_map<entt::entity, NoteComponent> originalNotes;
    for ( auto entity : toAlign ) {
        if ( m_ctx.noteRegistry.valid(entity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            originalNotes[entity] =
                m_ctx.noteRegistry.get<NoteComponent>(entity);
        }
    }

    std::unordered_map<entt::entity, NoteComponent> newNotes = originalNotes;

    // Align non-polyline notes and child subnotes
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type != ::MMM::NoteType::POLYLINE ) {
            bool shouldAlign = false;
            if ( m_ctx.noteRegistry.all_of<InteractionComponent>(entity) &&
                 m_ctx.noteRegistry.get<InteractionComponent>(entity)
                     .isSelected ) {
                shouldAlign = true;
            } else if ( newNote.m_isSubNote &&
                        newNote.m_parentPolyline != entt::null ) {
                if ( toAlign.count(newNote.m_parentPolyline) ) {
                    shouldAlign = true;
                }
            } else {
                shouldAlign = true;
            }

            if ( shouldAlign ) {
                alignNote(newNote);
            }
        }
    }

    // Sync subnotes within parent polylines
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            struct ChildInfo {
                entt::entity entity;
                double       timestamp;
                int          originalSubIndex;
            };
            std::vector<ChildInfo> children;
            for ( const auto& [otherEnt, otherNote] : newNotes ) {
                if ( otherNote.m_isSubNote &&
                     otherNote.m_parentPolyline == entity ) {
                    children.push_back({ otherEnt,
                                         otherNote.m_timestamp,
                                         otherNote.m_subIndex });
                }
            }

            std::stable_sort(
                children.begin(),
                children.end(),
                [](const ChildInfo& a, const ChildInfo& b) {
                    if ( std::abs(a.timestamp - b.timestamp) < 1e-9 ) {
                        return a.originalSubIndex < b.originalSubIndex;
                    }
                    return a.timestamp < b.timestamp;
                });

            std::vector<NoteComponent::SubNote> newSubNotesList;
            newSubNotesList.reserve(newNote.m_subNotes.size());

            for ( size_t i = 0; i < children.size(); ++i ) {
                entt::entity childEnt = children[i].entity;
                int          oldIdx   = children[i].originalSubIndex;

                NoteComponent::SubNote updatedSub = newNote.m_subNotes[oldIdx];
                updatedSub.timestamp = newNotes[childEnt].m_timestamp;
                updatedSub.duration  = newNotes[childEnt].m_duration;

                newSubNotesList.push_back(updatedSub);
                newNotes[childEnt].m_subIndex = static_cast<int>(i);
            }

            newNote.m_subNotes = std::move(newSubNotesList);

            if ( !newNote.m_subNotes.empty() ) {
                newNote.m_timestamp  = newNote.m_subNotes.front().timestamp;
                newNote.m_trackIndex = newNote.m_subNotes.front().trackIndex;
            }
        }
    }

    // Generate BatchNoteAction entries
    for ( auto entity : toAlign ) {
        entries.push_back({ entity, originalNotes[entity], newNotes[entity] });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                        "Align Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Aligned {} selected items to nearest common beat divisors",
              count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.tools.align_beats"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

}  // namespace MMM::Logic
