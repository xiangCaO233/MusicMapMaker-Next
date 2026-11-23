#include "ui/imgui/manager/SearchView.h"
#include "config/skin/translation/Translation.h"
#include <cmath>
#include <imgui.h>

/// @file SearchView.cpp
/// @brief 全局搜索子视图的最小布局测量与占位界面实现。
/// @note 索引服务接入前只展示功能状态，侧栏仍可恢复用户保存的布局。

namespace MMM::UI
{

/// @brief 创建具名搜索子视图。
/// @param name 侧栏管理器注册该视图时使用的稳定名称。
SearchView::SearchView(const std::string& name) : ISubView(name) {}

namespace
{

/// @brief 全局搜索尚未开放时，布局测量和渲染共用的翻译键。
constexpr char SEARCH_UNAVAILABLE[] = "ui.search.global_unavailable";

}  // namespace

/// @brief 获取搜索状态提示所需的最小内容尺寸。
/// @param dpiScale 当前窗口内容缩放，由 ImGui 当前字体和样式体现。
/// @return 可容纳一行本地化提示文本的最小尺寸。
/// @warning UI 热路径：布局查询可能每帧多次调用，只执行字体测量。
ImVec2 SearchView::getMinContentSize(float /*dpiScale*/) const
{
    const ImVec2 textSize = ImGui::CalcTextSize(TR(SEARCH_UNAVAILABLE).data());
    // 向上取整避免停靠窗口在分数像素边界反复调整。
    return ImVec2(std::ceil(textSize.x),
                  std::ceil(ImGui::GetTextLineHeightWithSpacing()));
}

/// @brief 显示全局搜索的可用状态。
/// @param layoutContext 当前布局上下文。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI 热路径：面板可见时每帧调用，只提交状态提示。
void SearchView::onUpdate(LayoutContext& /*layoutContext*/,
                          UIManager* /*sourceManager*/)
{
    // 索引服务未接入时没有可执行的查询，避免输入后显示误导性的空结果。
    ImGui::TextDisabled("%s", TR(SEARCH_UNAVAILABLE).data());
}

}  // namespace MMM::UI
