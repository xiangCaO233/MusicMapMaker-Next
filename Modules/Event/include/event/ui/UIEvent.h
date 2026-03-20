#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <string>

namespace MMM
{
namespace Graphic::UI
{
class UIManager;
class IUIView;
}  // namespace Graphic::UI
namespace Event
{
struct UIEvent : public BaseEvent {
    ///@brief 需要发生变更的ui管理器常指针
    const Graphic::UI::UIManager* uiManager;

    ///@brief 需要变更的ui名
    std::string sourceUiName;
};
}  // namespace Event
}  // namespace MMM

// 注册parent关系
EVENT_REGISTER_PARENTS(UIEvent, BaseEvent);
