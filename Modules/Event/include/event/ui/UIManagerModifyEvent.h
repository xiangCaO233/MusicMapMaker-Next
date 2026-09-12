#pragma once

#include "event/ui/UIEvent.h"
#include <cstdint>
#include <memory>

namespace MMM
{

namespace Event
{

/// @brief UI 管理器支持的视图集合变更类型。
/// @note 显式数值属于事件协议的一部分，新增枚举值不得复用现有编号。
enum Operate : uint32_t {
    INSERT = 1,
    REMOVE = 2,
    UPDATE = 3,
};

/// @brief 请求 UIManager 插入、移除或更新一个视图。
/// @note 插入和更新通过独占指针转移视图所有权。
struct UIManagerModifyEvent : public UIEvent {
    /// @brief 本次请求执行的集合变更类型。
    const Operate operate;

    /// @brief 插入或更新时转交给 UIManager 的视图资源。
    /// @note 移除操作可保持为空，目标由继承的 sourceUiName 标识。
    std::unique_ptr<UI::IUIView> ui_resource{ nullptr };
};

}  // namespace Event

}  // namespace MMM

// 注册 UI 事件父类关系，供 UIManager 的通用订阅入口接收变更。
// 视图资源的实际接管时机由订阅者决定，事件只表达所有权转移意图。
EVENT_REGISTER_PARENTS(UIManagerModifyEvent, UIEvent);
