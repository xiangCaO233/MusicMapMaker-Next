#pragma once

#include "ui/ISubView.h"
#include <string>

namespace MMM::UI
{

/**
 * @brief 全局搜索面板。
 * @details 索引服务接入前显示功能状态，并保留原有侧栏入口。
 */
class SearchView : public ISubView
{
public:
    /// @brief 创建具有稳定子视图名称的搜索面板。
    /// @param name 侧栏管理器用于识别面板的名称。
    SearchView(const std::string& name);
    /// @brief 按基础子视图生命周期销毁搜索面板。
    ~SearchView() override = default;

    /// @brief 绘制全局搜索的可用状态。
    /// @param layoutContext 当前布局上下文。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：面板可见时每帧调用，只提交状态提示。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取搜索面板中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 搜索面板最小内容尺寸。
    ImVec2 getMinContentSize(float dpiScale) const override;
};

}  // namespace MMM::UI
