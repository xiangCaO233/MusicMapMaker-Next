#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/tool/IEditTool.h"
#include <unordered_map>

namespace MMM::Logic
{

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
        // 可以存储更多初始信息，比如 Transform
    };
    std::unordered_map<entt::entity, InitialState> m_initialStates;

    /// @brief 记录当前是否为折线内部子段拖拽模式
    bool m_isPolylineSubDrag{ false };

    /// @brief 当前拖拽手势是否已经应用过目标格点。
    bool m_hasLastAppliedDragTarget{ false };

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
};

}  // namespace MMM::Logic
