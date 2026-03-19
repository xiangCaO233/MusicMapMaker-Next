#pragma once

#include "event/core/EventBus.h"
#include "event/input/translators/ImGuiTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/ui/iwindow/UIWindowKeyEvent.h"
#include "event/ui/iwindow/UIWindowMouseEvent.h"
#include <imgui.h>
#include <string>

namespace MMM::Graphic::UI
{
class IUIView
{
public:
    /// @brief ui名称
    const std::string m_name;

    IUIView(const std::string& name) : m_name(name) {}
    virtual ~IUIView() = default;

    /// @brief 更新ui
    virtual void update() = 0;

    /// @brief 是否可渲染
    /// @return 默认不可再渲染
    virtual bool renderable() { return false; }
};
}  // namespace MMM::Graphic::UI
