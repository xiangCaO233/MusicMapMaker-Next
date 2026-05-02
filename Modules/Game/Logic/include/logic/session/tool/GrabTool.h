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
};

}  // namespace MMM::Logic
