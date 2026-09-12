#include "ui/imgui/manager/SearchView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

/// @file SearchView.cpp
/// @brief 全局搜索子视图的最小布局测量与占位界面实现。
/// @note 当前搜索执行逻辑尚未接入，本文件只维护输入缓冲和稳定布局。

namespace MMM::UI
{

/// @brief 创建具名搜索子视图。
/// @param name 侧栏管理器注册该视图时使用的稳定名称。
SearchView::SearchView(const std::string& name) : ISubView(name) {}

/// @brief 获取搜索面板中不可再换行控件所需的最小内容尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 可容纳输入提示、空结果文本和两行控件的最小尺寸。
/// @warning UI 热路径：布局查询可能每帧多次调用，只执行常量级字体测量。
ImVec2 SearchView::getMinContentSize(float dpiScale) const
{
    // 不允许低于 1 的缩放压缩固定输入区补偿量。
    const float scale = std::max(1.0f, dpiScale);
    // 输入提示和空结果中较宽者决定文本下限。
    const float inputText =
        ImGui::CalcTextSize(TR("title.search_manager").data()).x;
    const float emptyText =
        ImGui::CalcTextSize(TR("ui.search.no_results").data()).x;
    // 为输入框保留两侧 padding 和固定的光标/清除操作余量。
    const float inputPad =
        ImGui::GetStyle().FramePadding.x * 2.0f + std::floor(48.0f * scale);
    // 各项向上取整，避免分数像素导致停靠窗口反复抖动。
    const float minWidth = std::ceil(std::max(inputText + inputPad, emptyText));
    const float minHeight =
        std::ceil(ImGui::GetFrameHeightWithSpacing() * 2.0f +
                  ImGui::GetTextLineHeightWithSpacing());
    return ImVec2(minWidth, minHeight);
}

/// @brief 绘制搜索输入框和当前空结果状态。
/// @param layoutContext 当前布局上下文，预留给搜索结果列表布局。
/// @param sourceManager 当前 UI 管理器，预留给结果跳转。
/// @warning UI 热路径：面板可见时每帧调用，不得执行文件系统扫描。
void SearchView::onUpdate(LayoutContext& layoutContext,
                          UIManager*     sourceManager)
{
    // 输入框占满当前可用宽度，并使用稳定隐藏标签保存编辑状态。
    ImGui::SetNextItemWidth(-1);
    if ( ImGui::InputTextWithHint("##GlobalSearch",
                                  TR("title.search_manager").data(),
                                  m_searchBuffer,
                                  sizeof(m_searchBuffer)) ) {
        // TODO: 在索引服务接入后，仅提交查询文本，不在 UI 帧遍历谱面。
    }

    // 分隔输入区与结果区，空结果也保持一致结构。
    ImGui::Separator();

    // 搜索后端接入前始终显示弱化的空结果占位文本。
    ImGui::TextDisabled("%s", TR("ui.search.no_results").data());
}

}  // namespace MMM::UI
