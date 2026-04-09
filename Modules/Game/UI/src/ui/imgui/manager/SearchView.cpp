#include "ui/imgui/manager/SearchView.h"
#include "config/skin/SkinConfig.h"
#include <imgui.h>

namespace MMM::UI
{

SearchView::SearchView(const std::string& name) : ISubView(name) {}

void SearchView::onUpdate(LayoutContext& layoutContext,
                          UIManager*     sourceManager)
{
    // 搜索栏
    ImGui::SetNextItemWidth(-1);
    if ( ImGui::InputTextWithHint("##GlobalSearch",
                                  TR("title.search_manager"),
                                  m_searchBuffer,
                                  sizeof(m_searchBuffer)) ) {
        // TODO: 实现搜索逻辑
    }

    ImGui::Separator();

    ImGui::TextDisabled("%s", TR("ui.search.no_results").data());
}

}  // namespace MMM::UI
