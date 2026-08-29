#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/tool/IEditTool.h"
#include "mmm/project/AudioResource.h"
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace MMM::Logic
{

/// @brief 将满足规则的自动采样转换为玩家 Tap。
/// @param sample 待转换的自动采样。
/// @param targetTrack 玩家区目标轨道。
/// @param resource 已解析的项目音频资源。
/// @return offset 为零且为 Effect 或空资源草稿时返回转换结果。
[[nodiscard]] std::optional<NoteComponent> makePlayerNoteFromSample(
    const SampleComponent& sample, std::int32_t targetTrack,
    const ::MMM::AudioResource* resource);

/// @brief 将满足规则的玩家 Tap 转换为自动采样。
/// @param note 待转换的玩家物件。
/// @param targetTrack BGM 区统一绝对轨道。
/// @param resource 已解析的项目音频资源。
/// @return 未绑定音频或绑定 Effect 的普通 Tap 返回转换结果。
[[nodiscard]] std::optional<SampleComponent> makeAudioSampleFromPlayerNote(
    const NoteComponent& note, std::uint32_t targetTrack,
    const ::MMM::AudioResource* resource);

/// @brief 抓取工具 (MoveTool)，负责移动选中的音符。
class GrabTool : public IEditTool
{
public:
    void handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd) override;
    void handleUpdateDrag(SessionContext&      ctx,
                          const CmdUpdateDrag& cmd) override;
    void handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd) override;

private:
    struct InitialState {
        NoteComponent note;
        /// @brief 拖动开始时是否处于选中状态。
        bool selected{ false };
    };

    /// @brief 自动采样拖动开始时的完整状态。
    struct SampleInitialState {
        SampleComponent sample;
        /// @brief 拖动开始时是否处于选中状态。
        bool selected{ false };
    };

    std::unordered_map<entt::entity, InitialState> m_initialStates;
    /// @brief 当前手势涉及的自动采样初始状态。
    std::unordered_map<entt::entity, SampleInitialState> m_initialSampleStates;

    /// @brief 记录当前是否为折线内部子段拖拽模式
    bool m_isPolylineSubDrag{ false };

    /// @brief 当前手势是否已进入统一轨道移动或跨区转换模式。
    bool m_usesUnifiedObjectDrag{ false };

    /// @brief 当前手势是否只编辑自动采样的实际触发 offset。
    bool m_isSampleOffsetDrag{ false };

    /// @brief 当前拖拽手势是否已经应用过目标格点。
    bool m_hasLastAppliedDragTarget{ false };

    /// @brief 拖拽开始时的持久化草稿轨道数量。
    std::int32_t m_initialDraftTrackCount{ 0 };

    /// @brief 当前手势是否触发过草稿追加轨扩展。
    bool m_expandedDraftTracks{ false };

    /// @brief 当前拖拽手势上一次应用的目标时间。
    double m_lastAppliedDragTargetTime{ 0.0 };

    /// @brief 当前拖拽手势上一次应用的目标轨道。
    int m_lastAppliedDragTargetTrack{ 0 };

    /// @brief 将父折线的 m_subNotes 数据同步到所有子物件实体
    void syncPolylineSubEntities(SessionContext& ctx, entt::entity parent,
                                 const NoteComponent& note);

    /// @brief 尝试在折线子段拖拽结束时执行合并操作 (dtrack==0 或 duration==0)
    /// @return true 如果执行了合并并已提交 Action
    bool tryPolylineSubDragMerge(SessionContext& ctx);

    /// @brief 处理统一玩家/BGM 轨道中的整物件拖动。
    /// @param ctx 会话上下文。
    /// @param cmd 当前拖动位置。
    /// @return 已处理本次更新时返回 true。
    bool handleUnifiedDragUpdate(SessionContext& ctx, const CmdUpdateDrag& cmd);

    /// @brief 结束统一轨道拖动并原子提交移动或跨区转换。
    /// @param ctx 会话上下文。
    void finishUnifiedDrag(SessionContext& ctx);
};

}  // namespace MMM::Logic
