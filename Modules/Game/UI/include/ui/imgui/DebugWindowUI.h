#pragma once
#include "ui/IUIView.h"

namespace MMM::UI
{
/**
 * @brief 调试窗口，用于显示渲染引擎的内部状态（如 Glow Mask）
 */
class DebugWindowUI : public IUIView
{
public:
    DebugWindowUI(const std::string& name);
    ~DebugWindowUI() override = default;

    void update(UIManager* sourceManager) override;
};
}  // namespace MMM::UI
