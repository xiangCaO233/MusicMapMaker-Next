#pragma once

#include "common/LogicCommands.h"
#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <cstdint>
#include <string>

namespace MMM::Event
{
/// @brief 新建谱面交互已经到达的业务阶段。
/// @details 阶段只单向描述已经发生的事实，不作为 UI 向导状态机的控制命令。
enum class BeatmapCreateInteractionStage : std::uint8_t {
    WizardOpened,      ///< 新建谱面向导已经显示。
    TemplateSelected,  ///< 已选择可用的打开谱面作为模板。
    AudioSelected,     ///< 已选择满足创建条件的主音频。
    TimingMeasured,    ///< BPM 测量结果已经回填向导。
    Completed          ///< 谱面文件创建成功且已建立编辑会话。
};

/// @brief 新建谱面向导的阶段事件，与具体演练文案和步骤 ID 无关。
/// @note 入口随 UI 命令传到逻辑线程，失败或取消不会发布 Completed。
/// @note AudioSelected 可以重复发布，订阅方应按幂等业务事实处理。
/// @note m_beatmapPath 只用于完成结果诊断，不能代替谱面对象或会话身份。
struct BeatmapCreateInteractionEvent : BaseEvent {
    /// @brief 发起向导的菜单或快捷键入口。
    Logic::BeatmapCreateOrigin m_origin{ Logic::BeatmapCreateOrigin::Unknown };

    /// @brief 当前已经实际完成的交互阶段。
    BeatmapCreateInteractionStage m_stage{
        BeatmapCreateInteractionStage::WizardOpened
    };

    /// @brief 当前阶段是否属于从已有模板创建的路径。
    /// @note WizardOpened 发生在用户选择来源前，因此该阶段固定为 false。
    bool m_fromTemplate{ false };

    /// @brief 完成阶段的新谱面项目内路径，其他阶段为空。
    std::string m_beatmapPath;
};
}  // namespace MMM::Event

// 注册基础事件关系，使统一 EventBus 能按时间戳分发新建谱面阶段。
EVENT_REGISTER_PARENTS(MMM::Event::BeatmapCreateInteractionEvent,
                       MMM::Event::BaseEvent);
