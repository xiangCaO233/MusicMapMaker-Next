#pragma once

#include "event/ui/UIEvent.h"
#include <cstdint>
#include <memory>

namespace MMM
{

namespace Event
{

enum Operate : uint32_t {
    INSERT = 1,
    REMOVE = 2,
    UPDATE = 3,
};

struct UIManagerModifyEvent : public UIEvent {
    ///@brief 变更行为
    const Operate operate;

    ///@brief 更新或插入的资源
    std::unique_ptr<UI::IUIView> ui_resource{ nullptr };
};

}  // namespace Event

}  // namespace MMM

// 注册parent关系
EVENT_REGISTER_PARENTS(UIManagerModifyEvent, UIEvent);
