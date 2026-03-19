#pragma once

#include "ui/IUIView.h"

namespace MMM::Graphic::UI
{

class CLayoutTestUI : public IUIView
{
public:
    CLayoutTestUI(const std::string& name) : IUIView(name) {}
    CLayoutTestUI(CLayoutTestUI&&)                 = delete;
    CLayoutTestUI(const CLayoutTestUI&)            = delete;
    CLayoutTestUI& operator=(CLayoutTestUI&&)      = delete;
    CLayoutTestUI& operator=(const CLayoutTestUI&) = delete;
    ~CLayoutTestUI() override                      = default;

    void update() override;

private:
};


}  // namespace MMM::Graphic::UI
