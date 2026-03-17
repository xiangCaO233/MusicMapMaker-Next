#pragma once

#include <cstdint>
#include <memory>
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

enum Operate : uint32_t {
    INSERT = 1,
    REMOVE = 2,
    UPDATE = 3,
};

struct UIManagerModifyEvent {
    ///@brief 需要发生变更的ui管理器常指针
    const Graphic::UI::UIManager* uiManager;

    ///@brief 需要变更的ui名
    const std::string uiName;

    ///@brief 变更行为
    const Operate operate;

    ///@brief 更新或插入的资源
    std::unique_ptr<Graphic::UI::IUIView> ui_resource{ nullptr };
};

}  // namespace Event

}  // namespace MMM
