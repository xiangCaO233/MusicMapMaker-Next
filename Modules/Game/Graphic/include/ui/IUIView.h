#pragma once

namespace MMM::Graphic::UI
{
class IUIView
{
public:
    virtual ~IUIView()    = default;
    virtual void update() = 0;
};
}  // namespace MMM::Graphic::UI
