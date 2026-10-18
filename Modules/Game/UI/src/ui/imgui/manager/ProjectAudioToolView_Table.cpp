#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/Utf8Path.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/manager/ProjectAudioToolSearch.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
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
/// - 每次点击 Effect 行都尊重项目试听开关，并立即从头重播。
/// - 每个采样独立试听和编辑倍率，编辑时同步对应画笔资源。
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
    // 保存筛选重建状态，稍后在取得表头规格后统一排序一次。
    const bool rowsRebuilt = m_tableRowsDirty;
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
        // 不移动 m_items 元素，正在绘制的行引用与后续行索引保持有效。
        // 画笔选择的持久化格式与方块模式一致，旧项目无需迁移。
        if ( auto active = activateItem(index, false) ) {
            // 重复点击当前行同样重播，不把选中状态当作试听冷却。
            previewEffectSelection(m_items[*active]);
            persistWorkspace();
        }
    };

    // 数量表示匹配资源与总资源，筛选不能被理解为删除项目资源。
    ImGui::TextDisabled("%zu / %zu", m_tableRows.size(), m_items.size());
    if ( m_tableRows.empty() ) {
        // 无匹配与没有资源都不创建空试听控件，不启动音频加载。
        ImGui::TextDisabled("%s", m_translationCache.noSearchResults);
        return;
    }

    // 音量列沿用单行控件高度，不把 Popup 高度计入资源行。
    // 列隐藏不会清空画笔，重新显示后仍可继续调节当前选择。
    // 独立列读写资源缓存的倍率，切换选择时同步到实际放置命令。
    // 试听组仍使用同一资源池键，调整显示方式不会重复创建播放实例。
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
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
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
    if ( !ImGui::BeginTable("ProjectAudioToolTableV2",
                            6,
                            flags,
                            ImVec2(0.0F, visibleHeight),
                            820.0F * dpiScale) )
        return;
    ImGui::TableSetupColumn(
        "文件名",
        ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort,
        2.0F);
    // 文件名和 ID 可伸缩，类型与按钮列保持可操作的固定宽度。
    // 排序仅改变筛选索引，不移动方块缓存或修改项目布局。
    ImGui::TableSetupColumn(
        "类型", ImGuiTableColumnFlags_WidthFixed, 70.0F * dpiScale);
    ImGui::TableSetupColumn(
        "资源 ID", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn(
        "试听",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
        150.0F * dpiScale);
    // 音量独立成列，避免第四个图标被列裁剪后难以发现。
    ImGui::TableSetupColumn(
        "音量",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
        150.0F * dpiScale);
    ImGui::TableSetupColumn(
        "操作",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
        90.0F * dpiScale);
    // TableHeadersRow 会触发表格布局并绘制默认菜单，必须在它之前关闭默认入口。
    // 自定义菜单沿用同一弹窗身份，但不能与默认列显隐项重复提交相同标签 ID。
    ImGui::GetCurrentTable()->DisableDefaultContextMenu = true;
    ImGui::TableSetupScrollFreeze(0, 1);
    // 表头不占资源索引，裁剪器从第一条真实音频资源开始计数。
    ImGui::TableHeadersRow();

    // 表格内部 ID 使用新版本，避免旧五列表格设置误映射到六列布局。
    // 后续窗口重开仍由 ImGui 保存列宽、顺序和隐藏状态。
    // 复用项目其他资源表的上下文弹窗入口，替换已经禁用的默认菜单。
    // 显隐和宽度修改由 ImGui 排队，不在当前行绘制期间重建布局。
    // 内部表格指针仅在 BeginTable/EndTable 之间借用，不跨帧保存。
    // 默认菜单在布局阶段就可能提交，不能依靠后绘制自定义内容将其覆盖。
    // 不额外叠加独立窗口，否则会留下两个互相竞争的右键菜单。
    auto* table = ImGui::GetCurrentTable();
    // 弹层拥有独立窗口，排序接口不能在里面读取 CurrentTable。
    // 只记录本帧请求，退出全部菜单后再由真实表格上下文执行。
    int                requestedSortColumn    = -1;
    ImGuiSortDirection requestedSortDirection = ImGuiSortDirection_Ascending;
    /// @brief 暂存菜单排序选择，不在弹层上下文中调用表格排序接口。
    const auto requestSort = [&](int column, ImGuiSortDirection direction) {
        requestedSortColumn    = column;
        requestedSortDirection = direction;
    };
    if ( ImGui::TableBeginContextMenuPopup(table) ) {
        /// @brief 将菜单中的列显隐请求排队到所属表格的下一次布局。
        /// @warning 只由菜单点击调用；弹层中不能依赖 ImGui 的当前表格上下文。
        const auto setColumnEnabled = [table](int key, bool enabled) {
            // Popup 拥有独立窗口，TableSetColumnEnabled 找不到外层表格。
            // 直接针对借用的目标表排队，与其他资源表保持相同处理方式。
            table->Columns[key].IsUserEnabledNextFrame = enabled;
        };
        // 右键可能落在表头后的空白区域，不能将该值用于列数组下标。
        // 当前列适配只有有效物理列才开放，全表适配始终可用。
        const int column = table->ContextPopupColumn;
        if ( column >= 0 && column < 6 && FeedbackMenuItem("适配当前列宽") )
            ImGui::TableSetColumnWidthAutoSingle(table, column);
        if ( FeedbackMenuItem("适配所有列宽") )
            ImGui::TableSetColumnWidthAutoAll(table);
        // 菜单使用物理列序号，用户拖动列后仍对应同一个资源字段。
        // 标签不保存到 Item，避免资源数量影响静态界面字符串开销。
        const std::array<const char*, 6> labels{ "文件名", "类型", "资源 ID",
                                                 "试听",   "音量", "操作" };
        if ( FeedbackBeginMenu("排序") ) {
            // 仅名称、类型和 ID 是资源属性；试听状态与画笔音量不能排序。
            for ( int key = 0; key < 3; ++key ) {
                // 相同的升降序标签位于不同字段子菜单，仍显式隔离 ID。
                // 排序规格交给 ImGui 更新，表头箭头与菜单操作保持同步。
                ImGui::PushID(key);
                if ( FeedbackBeginMenu(labels[key]) ) {
                    if ( FeedbackMenuItem("升序") )
                        requestSort(key, ImGuiSortDirection_Ascending);
                    if ( FeedbackMenuItem("降序") )
                        requestSort(key, ImGuiSortDirection_Descending);
                    FeedbackEndMenu();
                }
                ImGui::PopID();
            }
            FeedbackEndMenu();
        }
        // 重置入口与资源管理器一致，同时提供局部恢复和整体恢复。
        // 这些操作仅修改视图设置，不改项目资源及音频放置顺序。
        if ( FeedbackBeginMenu("重置") ) {
            // ImGui 的全量重置会恢复默认排序和初始列布局。
            // 排序缓存随后通过 SpecsDirty 分支消费新的规格。
            if ( FeedbackMenuItem("重置全部") )
                ImGui::TableResetSettings(table);
            // 列宽适配保留用户当前的列顺序与隐藏选择。
            // 不手动写像素宽度，字体和 DPI 变化仍使用控件测量。
            if ( FeedbackMenuItem("重置列宽") )
                ImGui::TableSetColumnWidthAutoAll(table);
            // 显隐请求下一轮布局时生效，不打断当前表头提交。
            // 即使试听或音量列被隐藏，也能从这里恢复访问入口。
            if ( FeedbackMenuItem("显示所有列") ) {
                for ( int key = 0; key < 6; ++key ) setColumnEnabled(key, true);
            }
            // 只恢复文件名升序，保留其余布局定制。
            // 单字段排序禁止累加旧字段，点击表头行为与菜单一致。
            if ( FeedbackMenuItem("重置排序") )
                requestSort(0, ImGuiSortDirection_Ascending);
            FeedbackEndMenu();
        }
        ImGui::Separator();
        // 不允许隐藏最后一个可见列；已经隐藏的列始终可重新开启。
        int enabledCount = 0;
        for ( int key = 0; key < 6; ++key )
            enabledCount += table->Columns[key].IsUserEnabledNextFrame ? 1 : 0;
        // 读取下一帧状态以反映本轮菜单中刚排队的显隐请求。
        // 不用当前可见位替代，避免菜单勾选比实际请求落后一帧。
        for ( int key = 0; key < 6; ++key ) {
            const bool enabled = table->Columns[key].IsUserEnabledNextFrame;
            if ( FeedbackMenuItem(labels[key],
                                  nullptr,
                                  enabled,
                                  !enabled || enabledCount > 1) )
                setColumnEnabled(key, !enabled);
        }
        ImGui::EndPopup();
    }
    // EndPopup 已恢复所属表格；此处与表头点击使用同一接口和排序规格。
    // 菜单选择在本帧生效，不等待下一轮输入，也不临时篡改 ImGui 全局上下文。
    if ( requestedSortColumn >= 0 )
        ImGui::TableSetColumnSortDirection(
            requestedSortColumn, requestedSortDirection, false);
    // 资源事件、搜索或用户排序操作才触发排序，常态帧仅访问可见行。
    // 同键时以稳定 ID 打破平局，避免重命名和资源刷新造成随机跳动。
    if ( auto* specs = ImGui::TableGetSortSpecs();
         specs && (rowsRebuilt || specs->SpecsDirty) ) {
        if ( specs->SpecsCount > 0 ) {
            const auto spec = specs->Specs[0];
            std::sort(
                m_tableRows.begin(),
                m_tableRows.end(),
                [&](std::size_t a, std::size_t b) {
                    const auto& left  = m_items[a];
                    const auto& right = m_items[b];
                    // 类型按固定枚举排序，其余字段比较缓存 UTF-8 文本。
                    // 比较器无资源查询、音频加载和所有权复制。
                    int order = 0;
                    if ( spec.ColumnIndex == 1 )
                        order = left.type < right.type   ? -1
                                : left.type > right.type ? 1
                                                         : 0;
                    else if ( spec.ColumnIndex == 2 )
                        order =
                            left.audioResourceId.compare(right.audioResourceId);
                    else
                        order = left.label.compare(right.label);
                    if ( order == 0 )
                        order =
                            left.audioResourceId.compare(right.audioResourceId);
                    return spec.SortDirection == ImGuiSortDirection_Descending
                               ? order > 0
                               : order < 0;
                });
        }
        // 本轮已完成缓存更新，避免每个 UI 帧再次全量排序。
        // 搜索和资源刷新即使规格不变，也通过 rowsRebuilt 重新排序。
        specs->SpecsDirty = false;
    }
    // Enter 按当前显示顺序选择首项，不能在排序前选择旧索引。
    if ( submitted && !m_tableRows.empty() ) selectRow(m_tableRows.front());

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
                                    ImGuiSelectableFlags_AllowDoubleClick,
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

            // 试听与音量分列，播放控制不再隐式承担音量弹窗入口。
            // 非选中行可直接试听；音量列发生实际编辑时再切换画笔目标。
            // 横向滚动隐藏试听列时不提交绝对定位控件，防止越界命中。
            if ( ImGui::TableSetColumnIndex(3) ) {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const ProjectAudioPreviewControlsLayout controls{
                    start,          ImGui::GetContentRegionAvail().x,
                    buttonSize,     style.ItemSpacing.x,
                    progressHeight, progressSpacing
                };
                // 复用已缓存的预览池键，不在每帧分配新的音频实例身份。
                // 公共入口负责播放、暂停、停止，此处不传可编辑音量指针。
                // 单纯绘制进度不会调用首次解码或加载音频的操作入口。
                static_cast<void>(renderProjectAudioPreviewControls(
                    item.audioResourceId.c_str(),
                    *project,
                    item.audioResourceId,
                    item.previewPoolKey,
                    item.audioVolume,
                    nullptr,
                    controls));
                // 绝对布局的试听组不负责流式行高，恢复游标再声明占位。
                ImGui::SetCursorScreenPos(start);
                ImGui::Dummy(ImVec2(controls.width, contentHeight));
            }
            // 操作列也检查可见性，滚动出视口后按钮不能覆盖相邻列。
            if ( ImGui::TableSetColumnIndex(4) ) {
                // 每行从自己的采样倍率开始编辑，不能读取共享画笔快照。
                // 倍率同时用于试听和后续放置，不重复叠加到资源基础配置。
                // 使用局部候选值，未操作行不能在绘制时抢走当前画笔资源。
                // 编辑不触发选行试听，连续拖动不会反复从头播放采样。
                float volume = item.audioVolume;
                ImGui::SetNextItemWidth(-1.0F);
                const bool volumeChanged =
                    FeedbackVolumeSlider("##TableAudioVolume", &volume, 200);
                if ( volumeChanged ) {
                    item.audioVolume = volume;
                    // 数值与画笔立即同步，拖动期间不保存项目或触发全部方块重建。
                    // 不提升层级，确保本帧行引用与表格排序继续有效。
                    activateItem(index, false);
                }
                // 松手提交一次最终状态；方向键等非激活编辑则按实际修改提交。
                // 即使释放帧数值未再变化，也要保存前面拖动产生的最终倍率。
                if ( ImGui::IsItemDeactivatedAfterEdit() ||
                     (volumeChanged && !ImGui::IsItemActive()) )
                    persistWorkspace();
            }
            if ( ImGui::TableSetColumnIndex(5) ) {
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
