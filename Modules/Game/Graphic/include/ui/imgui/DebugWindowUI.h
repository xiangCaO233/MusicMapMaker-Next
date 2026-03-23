#pragma once

#include "ui/IUIView.h"

namespace MMM::Graphic::UI
{

class DebugWindowUI : public IUIView
{
public:
    DebugWindowUI(const std::string& name);
    DebugWindowUI(DebugWindowUI&&)                 = default;
    DebugWindowUI(const DebugWindowUI&)            = default;
    DebugWindowUI& operator=(DebugWindowUI&&)      = delete;
    DebugWindowUI& operator=(const DebugWindowUI&) = delete;
    ~DebugWindowUI() override                      = default;

    void update() override;

private:
    static float slideValue1;
    static float slideValue2;
    static float slideValue3;
    static float slideValue4;
};

}  // namespace MMM::Graphic::UI
