#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <string>

namespace MMM
{
namespace UI
{
class UIManager;
class IUIView;
}  // namespace UI
namespace Event
{
/// @brief 为 UI 层事件提供来源管理器与视图名称。
/// @note 事件仅观察 UIManager；发布者必须保证分发期间对象仍然存活。
struct UIEvent : public BaseEvent {
    /// @brief 接收或产生此次变更的 UI 管理器观察指针。
    /// @warning 该指针不拥有对象，也不得跨越 UIManager 生命周期保存。
    const UI::UIManager* uiManager;

    /// @brief 产生事件或需要变更的 UI 视图稳定名称。
    /// @note 名称只承担路由标识，不用于推断视图对象的生命周期。
    std::string sourceUiName;
};
}  // namespace Event
}  // namespace MMM

// 注册基础事件关系，使 UI 事件参与统一的时间戳与分发处理。
// 派生 UI 事件只需注册自己的直接父类，事件总线会继续展开该关系。
// 这避免各事件重复声明 BaseEvent，同时保留按层级订阅的能力。
EVENT_REGISTER_PARENTS(UIEvent, BaseEvent);
