#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/manager/ProjectAudioToolSearch.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace MMM::UI
{
/// @brief 切换展示并结束方块布局手势，不清空当前音频画笔。
/// @param enabled 是否进入表格模式。
/// @warning 低频交互路径：仅实际切换时保存未完成的布局并清理手势。
/// @details 切换过程只改变展示，不重新导入或替换音频资源。
/// 方块中的缩放与滚动保存在原相机状态，返回时继续使用。
/// 批量选区属于布局手势，表格不继承该选区作为音频画笔。
/// 搜索词由两种模式共用，切换不会清空用户正在查找的资源。
/// 当前选中资源与音量仍保留，不发出清空画笔命令。
void ProjectAudioToolView::setTableView(bool enabled)
{
    // 同模式请求不得反复保存项目或重建筛选缓存。
    if ( m_tableView == enabled ) return;
    if ( m_draggingItem || m_resizingItem || m_batchDragging ) {
        // 切换前保存最后的可见位置，防止再次进入方块模式恢复旧布局。
        persistWorkspace();
    }
    // 切到表格前的数组可能因方块提升层级而变化，必须使行映射失效。
    // 这里只标脏，实际重建留到表格获得当前项目资源后执行。
    m_tableView      = enabled;
    m_tableRowsDirty = true;
    // 几何手势属于方块模式，不能在表格中松开鼠标后继续提交。
    m_draggingItem.reset();
    m_resizingItem.reset();
    m_batchDragging    = false;
    m_marqueeSelecting = false;
    m_batchDragEntries.clear();
    clearBatchSelection();
    // 清理未执行的定位请求，保留共享搜索词、选择与画布相机。
    m_searchFocusRequestId.clear();
    m_openVolumeEditorResourceId.clear();
}

/// @brief 显示项目音频表格；选择为画笔、试听和重命名复用原有入口。
/// @param dpiScale 当前显示缩放。
/// @warning UI 热路径：常态只处理可见行；资源和筛选变化时才遍历缓存。
/// @details 表格只是一种展示，不能修改方块位置、尺寸或叠层顺序。
/// 行索引只在资源缓存稳定时使用；重命名后的资源事件会使其失效。
/// 预览池与方块共用稳定资源 ID，切换不会重复创建另一个播放实例。
/// 用户选择与音量编辑才保存工作区，悬浮和滚动不产生文件操作。
/// @details 缓存约束：
/// - 行索引指向本视图的 Item 缓存，不保存 Project 资源容器内的指针。
/// - 资源事件、项目身份或 DPI 改变时才重新构建 Item。
/// - 搜索输入实际变化才重新生成行映射，获得焦点不触发重建。
/// - 模糊搜索复用文件名和资源 ID 评分规则，匹配时只保存索引。
/// - 空查询保留全部资源，不创建额外的搜索结果子窗口。
/// - 常态滚动只消费可见行，不对整个项目重新排序或重新扫描文件。
/// @details 交互约束：
/// - 文件名列承担选择入口，操作列承担已有重命名入口。
/// - 选择沿用画笔命令，并以不提升层级的方式激活 Item。
/// - 选择同一资源不重复自动试听，选择其他 Effect 仍尊重项目开关。
/// - 未选中的音频可独立试听，但不提供当前画笔音量编辑器。
/// - 只有音量实际变化时才提交新的画笔命令并保存工作区。
/// - 每行按钮使用资源稳定 ID，重名文件不能共享弹窗状态。
/// - 表格结束后外层再绘制重命名模态，不把模态留在滚动裁剪区内。
void ProjectAudioToolView::renderTableView(float dpiScale)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    // 外层已经检查项目，此处仍防御切换期间的调用，不保存跨帧项目指针。
    if ( !project ) return;
    const std::string projectRoot = Config::pathToUtf8(project->m_projectRoot);
    // 与方块一致地消费事件脏标记；项目切换不能复用旧行下标。
    // 初始默认排布仍按窗口宽度计算，不用表格内的横向内容宽度。
    if ( m_itemsDirty.exchange(false, std::memory_order_acq_rel) ||
         projectRoot != m_cachedProjectRoot ||
         std::abs(dpiScale - m_cachedDpiScale) > 1e-4F ) {
        rebuildItems(
            std::max(1.0F, ImGui::GetContentRegionAvail().x / dpiScale),
            dpiScale);
    }

    // 搜索输入与列表分离，纵向滚动不能把输入框带离视口。
    // Enter 只选择已有匹配行，不创建资源或修改筛选条件。
    ImGui::SetNextItemWidth(-1.0F);
    const bool submitted =
        ImGui::InputTextWithHint("##ProjectAudioToolSearch",
                                 m_translationCache.searchHint,
                                 m_searchBuffer.data(),
                                 m_searchBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll);
    if ( ImGui::IsItemEdited() ) {
        // 两种视图共享搜索词，另一模式下的评分结果也必须失效。
        m_tableRowsDirty     = true;
        m_searchResultsDirty = true;
    }
    if ( m_tableRowsDirty ) {
        m_tableRowsDirty = false;
        // 项目切换后旧下标不再有意义，先清空再从本轮缓存生成。
        // 索引容器保留容量，连续搜索不必每次重新分配同样大小的缓冲。
        m_tableRows.clear();
        const std::string_view query =
            ProjectAudioToolSearch::trimAsciiWhitespace(m_searchBuffer.data());
        // 只在输入或资源变化时评分，不在每帧或每个可见行重复模糊匹配。
        // 顺序沿用稳定资源缓存，选中行不会因提高 Z 序跳到列表末尾。
        for ( std::size_t i = 0; i < m_items.size(); ++i ) {
            const auto& item = m_items[i];
            if ( query.empty() ||
                 ProjectAudioToolSearch::scoreCandidate(item.label, query) ||
                 ProjectAudioToolSearch::scoreCandidate(item.audioResourceId,
                                                        query) ) {
                m_tableRows.push_back(i);
            }
        }
    }

    // 统一选择入口只改变画笔状态，表格明确禁止提升方块层级。
    const auto selectRow = [&](std::size_t index) {
        const bool changed =
            m_selectedAudioResourceId != m_items[index].audioResourceId;
        // 不移动 m_items 元素，正在绘制的行引用与后续行索引保持有效。
        // 画笔选择的持久化格式与方块模式一致，旧项目无需迁移。
        if ( auto active = activateItem(index, false) ) {
            if ( changed ) previewEffectSelection(m_items[*active]);
            persistWorkspace();
        }
    };
    if ( submitted && !m_tableRows.empty() ) selectRow(m_tableRows.front());
    // 数量表示匹配资源与总资源，筛选不能被理解为删除项目资源。
    ImGui::TextDisabled("%zu / %zu", m_tableRows.size(), m_items.size());
    if ( m_tableRows.empty() ) {
        // 无匹配与没有资源都不创建空试听控件，不启动音频加载。
        ImGui::TextDisabled("%s", m_translationCache.noSearchResults);
        return;
    }

    // 行高必须包含进度条和按钮组，否则裁剪器会跳过错误的资源区间。
    // 所有尺寸跟随 DPI，表格的纵向步长与实际控件高度保持一致。
    const auto& style           = ImGui::GetStyle();
    const float buttonSize      = ImGui::GetFrameHeight();
    const float progressHeight  = 5.0F * dpiScale;
    const float progressSpacing = 4.0F * dpiScale;
    const float contentHeight   = buttonSize + progressHeight + progressSpacing;
    const float rowHeight       = contentHeight + style.CellPadding.y * 2.0F;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    // 独立滚动区域占满剩余空间，窄窗口保留横向滚动而不压缩播放按钮。
    // 表头固定，行高固定，裁剪器可直接跳过不可见资源。
    // 使用实际裁剪区限制高度，停靠宿主超出平台视口时仍产生正确滚动范围。
    // 不能依赖内容驱动的可用区域，否则高宿主会误判离屏资源已经可见。
    const auto* window = ImGui::GetCurrentWindow();
    const float visibleHeight =
        std::max(1.0F,
                 std::min(window->InnerRect.Max.y, window->ClipRect.Max.y) -
                     window->WindowPadding.y - ImGui::GetCursorScreenPos().y);
    // 表格自身管理横纵滚动，不继承方块相机的滚动偏移或缩放倍率。
    if ( !ImGui::BeginTable("ProjectAudioToolTable",
                            5,
                            flags,
                            ImVec2(0.0F, visibleHeight),
                            820.0F * dpiScale) )
        return;
    ImGui::TableSetupColumn("文件名", ImGuiTableColumnFlags_WidthStretch, 2.0F);
    // 文件名和 ID 可伸缩，类型与按钮列保持可操作的固定宽度。
    // 不启用排序，行序继续对应项目原有布局顺序。
    ImGui::TableSetupColumn(
        "类型", ImGuiTableColumnFlags_WidthFixed, 70.0F * dpiScale);
    ImGui::TableSetupColumn(
        "资源 ID", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn(
        "试听 / 音量", ImGuiTableColumnFlags_WidthFixed, 210.0F * dpiScale);
    ImGui::TableSetupColumn(
        "操作", ImGuiTableColumnFlags_WidthFixed, 90.0F * dpiScale);
    ImGui::TableSetupScrollFreeze(0, 1);
    // 表头不占资源索引，裁剪器从第一条真实音频资源开始计数。
    ImGui::TableHeadersRow();

    // 裁剪只作用于资源行，表头已经提交并由冻结设置保持可见。
    // 不可见行不会查询预览状态或创建 ImGui 按钮。
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_tableRows.size()), rowHeight);
    while ( clipper.Step() ) {
        for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
              ++row ) {
            // 裁剪器下标属于筛选列表，先映射回原 Item 再执行操作。
            // 本帧重命名只排队弹窗，资源缓存不会在行循环中被重新构建。
            const auto index = m_tableRows[static_cast<std::size_t>(row)];
            auto&      item  = m_items[index];
            // ID 与名称解耦，同名音频也拥有独立选择和试听按钮。
            ImGui::PushID(item.audioResourceId.c_str());
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableSetColumnIndex(0);
            const bool selected =
                m_selectedAudioResourceId == item.audioResourceId;
            if ( FeedbackSelectable(item.label.c_str(),
                                    selected,
                                    ImGuiSelectableFlags_None,
                                    ImVec2(0.0F, contentHeight)) )
                selectRow(index);
            if ( ImGui::IsItemHovered() ) {
                // 长文件名完整内容只在悬浮时展示，行高不随名称换行改变。
                ImGui::SetTooltip("%s", item.label.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            // 类型直接来自缓存的资源属性，不在表格绘制时查询项目容器。
            ImGui::TextUnformatted(item.type == AudioTrackType::Main ? "MAIN"
                                                                     : "FX");
            ImGui::TableSetColumnIndex(2);
            // 显示稳定 ID 有助于区分同名资源，选择仍使用完整 ID。
            ImGui::TextUnformatted(item.audioResourceId.c_str());

            // 同一帧选择后重新判断，立即显示新画笔资源的音量按钮。
            // 非选中行允许试听，但不能误改当前画笔音量。
            // 横向滚动隐藏试听列时不提交绝对定位控件，防止越界命中。
            if ( ImGui::TableSetColumnIndex(3) ) {
                const bool active =
                    m_selectedAudioResourceId == item.audioResourceId;
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const ProjectAudioPreviewControlsLayout controls{
                    start,          ImGui::GetContentRegionAvail().x,
                    buttonSize,     style.ItemSpacing.x,
                    progressHeight, progressSpacing
                };
                // 复用已缓存的预览池键，不在每帧分配新的音频实例身份。
                // 公共入口负责播放、暂停、停止和已选资源的音量弹窗。
                // 单纯绘制进度不会调用首次解码或加载音频的操作入口。
                const auto result = renderProjectAudioPreviewControls(
                    item.audioResourceId.c_str(),
                    *project,
                    item.audioResourceId,
                    item.previewPoolKey,
                    active ? m_brushAudioVolume : 1.0F,
                    active ? &m_brushAudioVolume : nullptr,
                    controls);
                if ( result.volumeChanged ) {
                    // 共享入口返回实际变化时才同步画笔，音量弹窗不每帧保存项目。
                    // 试听倍率与后续放置物件的倍率保持一致，资源原音轨配置不变。
                    Logic::EditorEngine::instance().pushCommand(
                        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
                            item.audioResourceId,
                            item.type,
                            m_brushAudioVolume }));
                    persistWorkspace();
                }
                // 绝对布局的试听组不负责流式行高，恢复游标再声明占位。
                ImGui::SetCursorScreenPos(start);
                ImGui::Dummy(ImVec2(controls.width, contentHeight));
            }
            // 操作列也检查可见性，滚动出视口后按钮不能覆盖相邻列。
            if ( ImGui::TableSetColumnIndex(4) ) {
                // 文件名列已有选择入口，操作列专注复用已有重命名弹窗。
                if ( FeedbackSmallButton(m_translationCache.renameAction) )
                    requestItemRename(index);
            }
            ImGui::PopID();
            // 行级 ID 必须在下一行前恢复，不能让重复按钮嵌套累积。
        }
    }
    ImGui::EndTable();
    // 子窗口和资源级 ID 在所有成功路径均成对结束。
}
}  // namespace MMM::UI
