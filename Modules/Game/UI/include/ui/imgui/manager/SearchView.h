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

private:
    char m_searchBuffer[256] = "";
};

}  // namespace MMM::UI
