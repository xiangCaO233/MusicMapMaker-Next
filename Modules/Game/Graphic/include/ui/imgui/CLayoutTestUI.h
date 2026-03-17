#pragma once

#include "ui/IUIView.h"

namespace MMM::Graphic::UI
{

class CLayoutTestUI : public IUIView
{
public:
    CLayoutTestUI();
    CLayoutTestUI(CLayoutTestUI&&)                 = default;
    CLayoutTestUI(const CLayoutTestUI&)            = default;
    CLayoutTestUI& operator=(CLayoutTestUI&&)      = default;
    CLayoutTestUI& operator=(const CLayoutTestUI&) = default;
    ~CLayoutTestUI();

    void update() override;

private:
};


}  // namespace MMM::Graphic::UI
