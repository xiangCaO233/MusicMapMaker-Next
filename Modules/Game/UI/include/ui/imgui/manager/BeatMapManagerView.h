#pragma once

#include "ui/ISubView.h"

namespace MMM::UI
{

class BeatMapManagerView : public ISubView
{
public:
    BeatMapManagerView(const std::string& subViewName) : ISubView(subViewName)
    {
    }
    BeatMapManagerView(BeatMapManagerView&&)                 = default;
    BeatMapManagerView(const BeatMapManagerView&)            = default;
    BeatMapManagerView& operator=(BeatMapManagerView&&)      = delete;
    BeatMapManagerView& operator=(const BeatMapManagerView&) = delete;
    ~BeatMapManagerView() override                           = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
};

}  // namespace MMM::UI
