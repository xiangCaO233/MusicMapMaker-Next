#include "ui/imgui/manager/SearchView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace MMM::UI
{

SearchView::SearchView(const std::string& name) : ISubView(name) {}

/// @brief 获取搜索面板中不可再换行控件所需的最小内容尺寸。
ImVec2 SearchView::getMinContentSize(float dpiScale) const
{
    const float scale     = std::max(1.0f, dpiScale);
    const float inputText = ImGui::CalcTextSize(TR("title.search_manager")).x;
    const float emptyText =
        ImGui::CalcTextSize(TR("ui.search.no_results").data()).x;
    const float inputPad =
        ImGui::GetStyle().FramePadding.x * 2.0f + std::floor(48.0f * scale);
    const float minWidth = std::ceil(std::max(inputText + inputPad, emptyText));
    const float minHeight =
        std::ceil(ImGui::GetFrameHeightWithSpacing() * 2.0f +
                  ImGui::GetTextLineHeightWithSpacing());
    return ImVec2(minWidth, minHeight);
}

void SearchView::onUpdate(LayoutContext& layoutContext,
                          UIManager*     sourceManager)
{
    // 搜索栏
    ImGui::SetNextItemWidth(-1);
    if ( ::MMM::UI::AnimatedInputTextWithHint("##GlobalSearch",
                                              TR("title.search_manager"),
                                              m_searchBuffer,
                                              sizeof(m_searchBuffer)) ) {
        // TODO: 实现搜索逻辑
    }

    ImGui::Separator();

    ImGui::TextDisabled("%s", TR("ui.search.no_results").data());
}

}  // namespace MMM::UI
