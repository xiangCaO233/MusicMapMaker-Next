#pragma once

#include "ui/ISubView.h"

namespace MMM::UI
{
class AudioManagerView : public ISubView
{
public:
    AudioManagerView(const std::string& subViewName) : ISubView(subViewName) {}
    AudioManagerView(AudioManagerView&&)                 = default;
    AudioManagerView(const AudioManagerView&)            = default;
    AudioManagerView& operator=(AudioManagerView&&)      = delete;
    AudioManagerView& operator=(const AudioManagerView&) = delete;
    ~AudioManagerView() override                         = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
};

}  // namespace MMM::UI
