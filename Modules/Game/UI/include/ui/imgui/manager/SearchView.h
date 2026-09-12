#pragma once

#include "ui/ISubView.h"
#include <string>

namespace MMM::UI
{

/**
 * @brief 搜索视图
 * 提供对谱面物件、标记点、音频事件的全局搜索与快速跳转功能。
 */
class SearchView : public ISubView
{
public:
    /// @brief 创建具有稳定子视图名称的搜索面板。
    /// @param name 侧栏管理器用于识别面板的名称。
    SearchView(const std::string& name);
    /// @brief 按基础子视图生命周期销毁搜索面板。
    ~SearchView() override = default;

    /// @brief 绘制搜索输入及匹配结果区域。
    /// @param layoutContext 当前布局上下文。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：面板可见时每帧调用，禁止文件系统扫描。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取搜索面板中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 搜索面板最小内容尺寸。
    ImVec2 getMinContentSize(float dpiScale) const override;

private:
    /// @brief 固定容量的即时搜索文本缓冲，包含结尾空字符空间。
    char m_searchBuffer[256] = "";
};

}  // namespace MMM::UI
