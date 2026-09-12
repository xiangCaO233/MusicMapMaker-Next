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
    /// @brief 创建渲染调试窗口。
    /// @param name UIManager 使用的稳定视图名称。
    DebugWindowUI(const std::string& name);
    /// @brief 使用基础视图析构流程释放布局上下文。
    ~DebugWindowUI() override = default;

    /// @brief 绘制活动画布 Glow Mask 纹理及诊断状态。
    /// @param sourceManager 用于按活动相机查找可渲染视图的 UI 管理器。
    /// @warning UI 热路径：窗口可见时每帧读取现有描述符，不创建 GPU 资源。
    void update(UIManager* sourceManager) override;
};
}  // namespace MMM::UI
