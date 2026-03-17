#pragma once

namespace MMM::Graphic::UI
{
class IUIView
{
public:
    virtual ~IUIView() = default;

    /// @brief 更新ui
    virtual void update() = 0;

    /// @brief 是否可渲染
    /// @return 默认不可再渲染
    virtual bool renderable() { return false; }
};
}  // namespace MMM::Graphic::UI
