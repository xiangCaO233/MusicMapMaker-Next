#include "canvas/AnnotationTableWindow.h"

#include "canvas/AuxiliaryWindowState.h"
#include "canvas/AuxiliaryWindowUi.h"
#include "config/AppConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <imgui_internal.h>

namespace MMM::Canvas
{
namespace
{
/// @brief 批注表读取逻辑缓存的最小间隔，单位秒。
///
/// UI 帧率可能远高于逻辑缓存更新频率。该间隔只限制加锁查询，不延迟
/// 已经复制到窗口内的数据绘制，也不参与批注逻辑命令的提交。
constexpr double ANNOTATION_TABLE_REFRESH_INTERVAL_SECONDS = 0.1;

/// @brief 获取批注目标类型对应的翻译键。
/// @param targetKind 批注目标类型。
/// @return 可交给翻译系统的稳定键。
const char* annotationTargetLabelKey(
    ::MMM::BeatmapAnnotationTargetKind targetKind)
{
    // 翻译键在绘制时再解析，使运行期间切换语言后不必重建批注缓存。
    // 未知枚举值与时间戳目标共享安全回退键，避免把内部数值直接暴露到 UI。
    switch ( targetKind ) {
    case ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT:
        return "ui.annotation.target.player_object";
    case ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE:
        return "ui.annotation.target.audio_sample";
    case ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP:
    default: return "ui.annotation.target.timestamp";
    }
}
}  // namespace

/// @brief 创建批注表辅助窗口并注册稳定的 UI 视图名称。
/// @param name UIManager 用于查找视图的稳定名称。
///
/// 可见性、数据缓存与选择状态均由本类独立持有，不继承时间线窗口的
/// 生命周期；构造时保持关闭，等待菜单命令显式激活。
AnnotationTableWindow::AnnotationTableWindow(const std::string& name)
    : UI::IUIView(name)
{
}

/// @brief 查询批注表窗口自身的可见状态。
/// @return 窗口应参与本帧绘制时返回 true。
bool AnnotationTableWindow::isWindowOpen() const
{
    return m_isWindowOpen;
}

/// @brief 直接设置窗口可见状态并初始化下一帧恢复动作。
/// @param open 是否打开批注表。
///
/// 这是菜单勾选等“设为某状态”的入口；与 activateWindow 的切换语义
/// 分开，避免外部调用方必须知道窗口当前是否聚焦或是否位于屏幕外。
void AnnotationTableWindow::setWindowOpen(bool open)
{
    // 新打开的窗口需要检查旧 ImGui 布局是否仍在显示器工作区内，但菜单
    // 勾选不应强抢焦点，焦点请求只由明确的激活命令发起。
    m_isWindowOpen          = open;
    m_shouldRecoverWindow   = open;
    m_shouldFocusWindow     = false;
    m_isFocusedAndReachable = false;
    m_nextDataRefreshTime   = 0.0;
    // 关闭时立即释放与项目绑定的数据，杜绝下次打开短暂显示旧谱面批注。
    if ( !open ) resetData();
}

/// @brief 按辅助窗口统一规则切换、恢复或聚焦批注表。
///
/// 已聚焦且可达时再次激活表示关闭；隐藏、失焦或落在屏幕外时则保持打开，
/// 并在下一帧请求恢复和聚焦。
void AnnotationTableWindow::activateWindow()
{
    // 公共解析器统一时间线表、批注表等辅助窗口的菜单交互语义。
    const auto activation = resolveAuxiliaryWindowActivation(
        isWindowOpen(), m_isFocusedAndReachable);
    m_isWindowOpen        = activation.open;
    m_shouldFocusWindow   = activation.requestFocus;
    m_shouldRecoverWindow = activation.requestRecovery;
    // 激活后立刻允许刷新，避免沿用关闭前的节流截止时间。
    m_nextDataRefreshTime = 0.0;
    if ( !activation.open ) {
        // 关闭分支同步清空可达状态和项目数据，保持与标题栏关闭行为一致。
        m_isFocusedAndReachable = false;
        resetData();
    }
}

/// @brief 数据刷新后按稳定 ID 恢复选择，而不是沿用可能失效的行号。
/// @param selectedId 刷新前选中批注的稳定 ID；空值表示没有历史选择。
///
/// 删除或插入批注会改变排序索引。通过 ID 重定位可以保持用户正在阅读的
/// 批注；目标已经被删除时选择第一行，为键盘与详情区提供确定起点。
void AnnotationTableWindow::restoreSelection(const std::string& selectedId)
{
    m_selectedRow.reset();
    const auto& rows = m_data.rows();
    if ( !selectedId.empty() ) {
        // 数据量通常较小，且只在版本变化时线性查找；无需维护额外索引。
        const auto selected =
            std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.id == selectedId;
            });
        if ( selected != rows.end() ) {
            m_selectedRow =
                static_cast<std::size_t>(std::distance(rows.begin(), selected));
        }
    }
    // 历史项消失后选中首行，避免详情区因悬空索引继续引用旧容器。
    if ( !m_selectedRow && !rows.empty() ) m_selectedRow = 0U;
}

/// @brief 清空项目相关数据及当前详情选择。
///
/// 窗口控制标志由调用方分别处理，本函数只负责数据生命周期，便于关闭、
/// 会话失效和显式隐藏三条路径共享同一清理规则。
void AnnotationTableWindow::resetData()
{
    m_data.reset();
    m_dataStatus = AnnotationTableDataStatus::Close;
    m_selectedRow.reset();
}

/// @brief 从数据层发现活动谱面失效时完整关闭窗口。
///
/// 与 resetData 相比，这里还重置所有一次性 UI 请求，防止关闭后的下一帧
/// 残留 SetNextWindowFocus 或位置恢复操作影响别的窗口。
void AnnotationTableWindow::closeWindow()
{
    m_isWindowOpen          = false;
    m_shouldRecoverWindow   = false;
    m_shouldFocusWindow     = false;
    m_isFocusedAndReachable = false;
    m_nextDataRefreshTime   = 0.0;
    resetData();
}

/// @brief 刷新并绘制独立的批注表窗口。
/// @param sourceManager 当前 UI 管理器；批注表暂不需要反向访问管理器。
///
/// 本函数依次处理可见性、低频数据刷新、窗口恢复、表格虚拟化和详情预览。
/// 数据只由 AnnotationTableData 提供，不依赖时间线画布是否打开或渲染。
///
/// @warning UI 热路径：每帧调用。关闭时必须保持常量级早退；打开时会绘制
/// 可见表格行，但会话加锁刷新受固定间隔与版本号共同限制。
void AnnotationTableWindow::update(UI::UIManager* sourceManager)
{
    (void)sourceManager;
    // UIManager 仍会更新已注册但隐藏的视图。这里尽早返回，既避免构造
    // ImGui 窗口，也确保关闭期间不持有任何活动项目的数据副本。
    if ( !isWindowOpen() ) {
        m_shouldRecoverWindow   = false;
        m_shouldFocusWindow     = false;
        m_isFocusedAndReachable = false;
        resetData();
        return;
    }

    const double now = ImGui::GetTime();
    // GetTime 单调递增且与系统墙钟无关，修改系统时间不会造成刷新长期停滞。
    if ( now >= m_nextDataRefreshTime ) {
        // 在刷新前复制稳定 ID，而不是保留行引用；refresh 可能交换整个容器。
        std::string selectedId;
        const auto& rowsBeforeRefresh = m_data.rows();
        // selectedRow 可能来自上一个数据版本，取值前必须同时验证容器边界。
        if ( m_selectedRow && *m_selectedRow < rowsBeforeRefresh.size() ) {
            selectedId = rowsBeforeRefresh[*m_selectedRow].id;
        }
        // 数据层负责持锁和版本判断，窗口只消费 Ready/Pending/Close 状态。
        const auto refreshResult = m_data.refresh();
        m_dataStatus             = refreshResult.status;
        // 即使本次没有版本变化也推进截止时间，避免每帧争用会话锁。
        m_nextDataRefreshTime = now + ANNOTATION_TABLE_REFRESH_INTERVAL_SECONDS;
        if ( m_dataStatus == AnnotationTableDataStatus::Close ) {
            // 没有活动谱面时关闭而不是展示空壳，保持菜单勾选状态真实。
            closeWindow();
            return;
        }
        // Pending 保留旧选择；只有行容器真正替换后才需要按 ID 重定位。
        if ( refreshResult.rowsChanged ) restoreSelection(selectedId);
    }

    // 辅助窗口直接遵循编辑器外观设置，所有像素量在此统一乘 DPI，避免
    // 高分屏下圆角与间距仍停留在逻辑像素尺寸。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    // 像素对齐后的圆角和间距在非整数 DPI 下更稳定，避免边框落在半像素上。
    const float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    const float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    const ImVec2 itemSpacing{
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
    };

    // 样式作用域覆盖窗口、表格子区域和弹出层；末尾必须严格弹出六项。
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    // Popup 使用 frameRound 而非 windowRound，与按钮、输入框的视觉层级一致。
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    // 仅首次出现时指定默认大小，之后尊重用户在 imgui.ini 中保存的布局。
    ImGui::SetNextWindowSize(ImVec2(860.0F, 560.0F), ImGuiCond_FirstUseEver);
    if ( m_shouldFocusWindow ) {
        // SetNextWindowFocus 必须位于 Begin 之前，并且只消费一次激活请求。
        ImGui::SetNextWindowFocus();
    }
    // 可见标题允许翻译，### 后的稳定 ID 保证切换语言不会破坏停靠关系。
    std::string windowTitle =
        TR("ui.annotation.table.title").toString() + "###AnnotationTableWindow";
    bool& windowOpen = m_isWindowOpen;
    // 将成员直接交给 Begin 的关闭参数，标题栏操作可在本帧同步回写可见性。
    const bool wasOpenBeforeBegin = windowOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &windowOpen);
    // 原生标题栏关闭按钮没有走业务按钮包装，统一入口在此补发反馈音效。
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin,
                                                &windowOpen);
    if ( windowOpen ) {
        // 恢复只在显式打开或激活后执行一次，避免用户拖动窗口时被持续拉回。
        recoverCurrentAuxiliaryWindow(m_shouldRecoverWindow, dpiScale);
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // RootAndChildWindows 使详情 Child 或表格滚动区域获得焦点时，辅助
        // 窗口整体仍被视为已激活。
        const bool reachable = isCurrentAuxiliaryWindowReachable(dpiScale);
        const bool popupOpen = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // 弹窗会暂时转移焦点，此时延续上一帧的可达状态，避免菜单误判为
        // 失焦并重复激活；真正不可达时仍允许恢复到当前显示器。
        m_isFocusedAndReachable = resolveAuxiliaryWindowFocusedAndReachable(
            m_isFocusedAndReachable, reachable, focused, popupOpen);
    } else {
        // Begin 后发现标题栏已关闭时不再保留上一帧的聚焦可达结论。
        m_isFocusedAndReachable = false;
    }
    // Begin 已消费本帧的一次性请求，无论窗口内容是否因折叠而实际绘制。
    m_shouldRecoverWindow = false;
    m_shouldFocusWindow   = false;

    if ( opened ) {
        // opened 为 false 只表示窗口折叠或被裁剪；Begin/End 和样式栈仍需配对。
        if ( m_dataStatus != AnnotationTableDataStatus::Ready ) {
            // Pending 时保留窗口结构并显示同步状态，不把旧缓存冒充最新数据。
            ImGui::TextDisabled("%s", TR("ui.annotation.table.syncing").data());
        } else {
            const auto& rows = m_data.rows();
            // 汇总文字与操作提示放在滚动表格外，滚动大量行时仍保持可见。
            ImGui::Text(
                "%s", TR_FMT("ui.annotation.table.count", rows.size()).c_str());
            // TR_FMT 临时字符串存活到完整表达式结束，ImGui 会在调用内复制文本。
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("ui.annotation.table.hint").data());

            // 详情区占可用高度的固定比例，并限制上下界；表格获得剩余空间，
            // 从而在矮窗口中仍保留至少一段可滚动的行区域。
            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float detailHeight =
                // 比例用于适配常规窗口，上下界分别保障详情可读性与表格容量。
                std::clamp(availableHeight * 0.38F, 150.0F, 260.0F);
            const float tableHeight =
                // 扣除详情、主题间距和少量边界余量，避免两个区域发生重叠。
                std::max(160.0F,
                         availableHeight - detailHeight -
                             ImGui::GetStyle().ItemSpacing.y - 4.0F);
            // 表格负责结构化浏览，详情负责完整阅读；两者共享同一份只读 rows。
            constexpr ImGuiTableFlags tableFlags =
                // 用户可以调整列宽，交替底色与边框提高长列表的横向可读性；
                // ScrollY 将内容限制在计算出的 tableHeight 内。
                ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if ( ImGui::BeginTable("AnnotationTableRows",
                                   6,
                                   tableFlags,
                                   ImVec2(0.0F, tableHeight)) ) {
                // 禁用 ImGui 默认列菜单，避免与业务窗口自身的右键语义冲突。
                if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                    table->DisableDefaultContextMenu = true;
                }
                // 表头固定在纵向滚动区域顶部；数据行仍由 clipper 虚拟化。
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(
                    TR("ui.annotation.table.index").data(),
                    // 序号列保持固定宽度，避免正文拉伸时漂移。
                    ImGuiTableColumnFlags_WidthFixed,
                    52.0F * dpiScale);
                ImGui::TableSetupColumn(
                    TR("ui.annotation.timestamp").data(),
                    // 时间格式长度可预测，固定宽度留给正文。
                    ImGuiTableColumnFlags_WidthFixed,
                    125.0F * dpiScale);
                ImGui::TableSetupColumn(TR("ui.annotation.target").data(),
                                        // 目标类型与轨道号共同使用此固定列。
                                        ImGuiTableColumnFlags_WidthFixed,
                                        140.0F * dpiScale);
                ImGui::TableSetupColumn(TR("ui.annotation.author").data(),
                                        // 作者和正文按相对权重分享剩余宽度。
                                        ImGuiTableColumnFlags_WidthStretch,
                                        0.75F);
                ImGui::TableSetupColumn(
                    TR("ui.annotation.table.content").data(),
                    // 正文摘要权重更高，优先减少长文本截断。
                    ImGuiTableColumnFlags_WidthStretch,
                    1.7F);
                // 两个操作按钮共享列宽。用翻译后较长标签计算，避免某种语言
                // 下按钮文字截断或挤压正文列。
                const ImGuiStyle& style = ImGui::GetStyle();
                const float       actionButtonWidth =
                    std::max(
                        ImGui::CalcTextSize(
                            TR("ui.annotation.table.jump").data())
                            .x,
                        ImGui::CalcTextSize(TR("ui.common.delete").data()).x) +
                    style.FramePadding.x * 2.0F;
                const float actionColumnWidth =
                    actionButtonWidth * 2.0F +
                    // 两按钮之间保留主题定义的间距，
                    // 额外留白避免右侧边框贴近控件。
                    style.ItemSpacing.x + 8.0F * dpiScale;
                ImGui::TableSetupColumn(
                    TR("ui.annotation.table.action").data(),
                    // 操作列不参与剩余宽度分配，始终容纳双按钮。
                    ImGuiTableColumnFlags_WidthFixed,
                    actionColumnWidth);
                ImGui::TableHeadersRow();

                // Clipper 只遍历当前可见行，批注正文很多时仍保持绘制开销稳定。
                ImGuiListClipper clipper;
                // rows 在整段绘制期间不变，因此 clipper 的总数与索引始终一致。
                clipper.Begin(static_cast<int>(rows.size()));
                while ( clipper.Step() ) {
                    // DisplayStart/DisplayEnd 已由当前滚动位置裁剪，循环中不得
                    // 再对全量 rows 排序或构造派生列表。
                    for ( int rowIndex = clipper.DisplayStart;
                          rowIndex < clipper.DisplayEnd;
                          ++rowIndex ) {
                        const auto index = static_cast<std::size_t>(rowIndex);
                        // Clipper 返回有符号范围，转换发生在确认位于 rows
                        // 范围后。
                        const auto& row      = rows[index];
                        const bool  selected = m_selectedRow == index;
                        ImGui::TableNextRow();
                        // 每行首先创建跨列
                        // Selectable，之后各列内容覆盖在同一行上。
                        ImGui::TableSetColumnIndex(0);
                        // 显示序号与稳定控件 ID 分离，排序或翻译不会造成 ID
                        // 冲突。
                        const std::string rowLabel = fmt::format(
                            "#{}###AnnotationTableRow_{}", index + 1U, index);
                        const bool rowClicked = ::MMM::UI::FeedbackSelectable(
                            rowLabel.c_str(),
                            selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0.0F, ImGui::GetFrameHeight()));
                        // AllowOverlap 允许同一行的操作按钮独立响应；只有点击
                        // 非按钮区域时 Selectable 才承担整行选择。
                        const bool rowDoubleClicked =
                            ImGui::IsItemHovered() &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        if ( rowClicked ) {
                            // 详情区引用索引只在当前 rows 生命周期内使用。
                            m_selectedRow = index;
                        }

                        // 单击仅选择供阅读，双击与“跳转”按钮共享同一定位逻辑。
                        // 视觉偏移在发命令时读取，确保运行时配置变更立即生效。
                        const auto seekToRow = [&row]() {
                            const float visualOffset =
                                Config::AppConfig::instance()
                                    .getVisualConfig()
                                    .getEffectiveVisualOffset();
                            Event::EventBus::instance().publish(
                                // Seek
                                // 进入逻辑命令队列，由会话线程统一修改时间； UI
                                // 不直接触碰播放时钟或画布相机。
                                Event::LogicCommandEvent(Logic::CmdSeek{
                                    row.timestamp - visualOffset }));
                        };
                        if ( rowDoubleClicked ) seekToRow();
                        // 跳转不会自动关闭窗口，用户可以连续核对多条批注位置。

                        // 时间文本由批注表自己的 Timing 上下文格式化，不读取
                        // 时间线窗口快照，因此时间线隐藏时仍能正常显示拍号位置。
                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        const auto timeText = MMM::UI::Utils::formatCanvasTime(
                            row.timestamp, m_data.timeFormatContext());
                        ImGui::TextUnformatted(timeText.c_str());
                        // timeText 生命周期覆盖本次调用，TextUnformatted
                        // 不保存指针。

                        // 目标轨仅对存在具体轨道的批注显示；内部零基索引转换为
                        // 面向用户的一基编号，丢失目标另以醒目标记提示。
                        ImGui::TableSetColumnIndex(2);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(
                            TR(annotationTargetLabelKey(row.targetKind))
                                .data());
                        // 轨道号仅作为目标类型的补充，不改变目标类型的翻译文本。
                        if ( row.track >= 0 ) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("#%d", row.track + 1);
                        }
                        if ( row.targetMissing ) {
                            // 单字符警告保持表格紧凑，完整说明在下方详情中展示。
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                                               "!");
                        }

                        // 空作者使用翻译后的占位文本，不把空字符串渲染成空格。
                        ImGui::TableSetColumnIndex(3);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(
                            // 占位键由翻译系统提供，避免在代码内固化界面语言。
                            row.author.empty()
                                ? TR("ui.annotation.unknown_author").data()
                                : row.author.c_str());

                        // 表格只显示 Markdown
                        // 首行作为摘要，完整正文留给详情区；
                        // 使用指针对避免为每个可见行创建临时子串。
                        ImGui::TableSetColumnIndex(4);
                        ImGui::AlignTextToFramePadding();
                        const auto firstLineEnd = row.content.find('\n');
                        // npos 分支使用完整长度，指针终点始终落在 string
                        // 存储范围内。
                        const std::size_t firstLineLength =
                            firstLineEnd == std::string::npos
                                ? row.content.size()
                                : firstLineEnd;
                        ImGui::TextUnformatted(
                            row.content.data(),
                            row.content.data() + firstLineLength);

                        // 操作列按当前单元格剩余宽度均分，窄窗口下仍保证正尺寸。
                        ImGui::TableSetColumnIndex(5);
                        const float rowActionWidth =
                            // 列被用户压缩到极窄时以 1 像素兜底，避免向 ImGui
                            // 传入负宽度后按钮反向占满可用区域。
                            std::max(1.0F,
                                     (ImGui::GetContentRegionAvail().x -
                                      ImGui::GetStyle().ItemSpacing.x) /
                                         2.0F);
                        const std::string jumpLabel =
                            // ## 后缀只参与 ImGui ID，不会显示给用户。
                            fmt::format("{}##AnnotationTableJump_{}",
                                        TR("ui.annotation.table.jump").view(),
                                        index);
                        if ( ::MMM::UI::FeedbackButton(
                                 jumpLabel.c_str(),
                                 ImVec2(rowActionWidth,
                                        ImGui::GetFrameHeight())) ) {
                            seekToRow();
                        }
                        ImGui::SameLine();
                        // 控件 ID 包含行号，使相同的翻译标签在每行仍保持唯一。
                        const std::string deleteLabel =
                            fmt::format("{}##AnnotationTableDelete_{}",
                                        TR("ui.common.delete").view(),
                                        index);
                        if ( ::MMM::UI::FeedbackButton(
                                 deleteLabel.c_str(),
                                 ImVec2(rowActionWidth,
                                        ImGui::GetFrameHeight())) ) {
                            // 删除通过逻辑命令排队；当前帧继续使用现有 rows，等
                            // 下次修订号变化后统一刷新，避免迭代中修改容器。
                            Event::EventBus::instance().publish(
                                // 仅传稳定 ID，不把表格行地址跨线程传给逻辑层。
                                Event::LogicCommandEvent(
                                    Logic::CmdRemoveBeatmapAnnotation{
                                        row.id }));
                        }
                        // 逻辑命令只会在后续缓存修订中反映到 rows，本循环继续
                        // 使用当前不可变快照，保证 clipper 的索引范围始终有效。
                    }
                }
                ImGui::EndTable();
                // EndTable 在 BeginTable 成功的同一分支内调用，维持 ImGui
                // 栈平衡。
            }

            if ( rows.empty() ) {
                // 空状态位于表格之后，保留列标题以帮助用户理解窗口用途。
                ImGui::TextDisabled("%s",
                                    TR("ui.annotation.table.empty").data());
            } else if ( m_selectedRow && *m_selectedRow < rows.size() ) {
                // 重新检查边界以防关闭或异步刷新后的过期选择进入详情绘制。
                const auto& selected = rows[*m_selectedRow];
                // selected 引用仅覆盖当前绘制分支，不会保存到下一帧。
                // 详情使用独立可滚动 Child，长 Markdown 不会挤压或扩张主窗口。
                ImGui::BeginChild("AnnotationTableDetail",
                                  ImVec2(0.0F, detailHeight),
                                  ImGuiChildFlags_Borders);
                // 固定 Child ID 让 ImGui 跨帧保存详情正文自身的滚动位置。
                // 标题行组合时间、作者与目标元数据，正文仍交给 Markdown
                // 渲染器。
                const auto timeText = MMM::UI::Utils::formatCanvasTime(
                    selected.timestamp, m_data.timeFormatContext());
                ImGui::Text("%s: %s",
                            TR("ui.annotation.timestamp").data(),
                            timeText.c_str());
                // 作者与时间放在同一视觉行，点号只承担元数据分隔作用。
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "· %s",
                    selected.author.empty()
                        ? TR("ui.annotation.unknown_author").data()
                        : selected.author.c_str());
                // 目标状态另起一行，避免作者较长时挤掉目标与轨道提示。
                ImGui::Text(
                    "%s: %s",
                    TR("ui.annotation.target").data(),
                    TR(annotationTargetLabelKey(selected.targetKind)).data());
                // 详情重复目标信息，使横向滚动表格后仍能独立理解正文上下文。
                if ( selected.track >= 0 ) {
                    // 与表格列一致，面向用户展示一基轨道编号。
                    ImGui::SameLine();
                    ImGui::TextDisabled("#%d", selected.track + 1);
                }
                if ( selected.targetMissing ) {
                    // 丢失目标不影响批注正文可读性，只附加明确的状态警告。
                    ImGui::SameLine();
                    ImGui::TextColored(
                        ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                        "%s",
                        TR("ui.annotation.target_missing").data());
                }
                // 即使目标已丢失，作者、时间和正文仍是有效的批注历史信息。
                ImGui::Separator();
                // 分隔线明确区分结构化元数据与用户编写的自由格式正文。
                // 正文保留完整 Markdown 语义，包括链接、图片和换行。
                UI::renderMarkdown(selected.content);
                // MarkdownRenderer 在当前 Child 的裁剪矩形内输出，滚动状态由
                // Child 自己保存，不会改变上方表格的滚动位置。
                ImGui::EndChild();
            }
        }
    }
    ImGui::End();
    // 与上方六次 PushStyleVar 成对，不能让批注表外观泄漏到其它停靠窗口。
    ImGui::PopStyleVar(6);

    // 标题栏关闭发生在 Begin 内；结束 ImGui 生命周期后再安全释放数据。
    if ( !windowOpen ) resetData();
    // 标题栏关闭时 m_isWindowOpen 已由 ImGui 写回；下一帧会命中早退路径，
    // 因此这里无需再调用 closeWindow 或重复操纵停靠状态。
}

}  // namespace MMM::Canvas
