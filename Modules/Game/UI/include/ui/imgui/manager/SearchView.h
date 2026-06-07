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
    SearchView(const std::string& name);
    ~SearchView() override = default;

    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取搜索面板中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 搜索面板最小内容尺寸。
    ImVec2 getMinContentSize(float dpiScale) const override;

private:
    char m_searchBuffer[256] = "";
};

}  // namespace MMM::UI
