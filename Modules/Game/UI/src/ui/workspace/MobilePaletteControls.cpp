#include "common/LogicCommands.h"
#include "common/NoteColor.h"
#include "config/skin/translation/Translation.h"
#include "ui/IEditorApplicationService.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 独立移动调色页：大槽位入口、明确选区操作与无 Hover 的取色。
/// @param scale 内容缩放，窗口始终夹在实际视口内。
/// @details 与桌面共用颜色值和方案服务，不复制调色盘配置库。
/// 打开或调整颜色从不自动给已有选区上色；只有两个确认按钮修改物件。
/// 样式与取色引用仅在当前帧借用，离开后不保存外部组件地址。
/// 高级页保留既有方案导入、导出和分拍线编辑，重新打开回到本页。
/// @warning 可见时每帧绘制；只借用本地数组，不访问谱面或会话长锁。
void ToolbarView::renderMobileColorPalette(float scale)
{
    // 本页只响应工作台入口，不创建第二套长期颜色库。
    // 关闭时不测量内容，也不执行任何颜色同步命令。
    if ( !m_showColorPopup ) return;
    const auto display = ImGui::GetIO().DisplaySize;
    // 视口约束先于内容布局，窄屏和大字号通过正常纵向滚动容纳。
    // 不使用桌面上一次浮层测量值，避免窗口被推出手机边界。
    const ImVec2 size{ std::min(380 * scale, display.x - 16 * scale),
                       std::min(620 * scale, display.y - 16 * scale) };
    // 位置随当前视口重新计算，横竖屏切换不依赖桌面停靠方向。
    // 保留系统可用区域，窗口内容与皮肤全局内边距保持一致。
    ImGui::SetNextWindowPos(
        { (display.x - size.x) * .5f, (display.y - size.y) * .5f });
    ImGui::SetNextWindowSize(size);
    if ( ImGui::Begin("##ColorPalettePopup",
                      nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings) ) {
        // 工具层高于停靠宿主，下拉选项仍保持原生弹层优先级。
        if ( !ImGui::IsPopupOpen(
                 nullptr,
                 ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.title").data());
        ImGui::TextWrapped("%s", TR("ui.mobile.color.palette_hint").data());
        // 固定槽位顺序与 NoteColorSlot ABI 一致，翻译只影响显示文字。
        // 不把当前语言的标题用作持久 ID，语言切换不会改变编辑对象。
        static constexpr std::array slots{
            "ui.toolbar.note_palette.note",
            "ui.toolbar.note_palette.note_head",
            "ui.toolbar.note_palette.note_hold",
            "ui.toolbar.note_palette.note_end",
            "ui.toolbar.note_palette.note_flick_arrow",
            "ui.toolbar.note_palette.note_node"
        };
        const auto active = static_cast<std::size_t>(m_activeColorSlot);
        // 槽位名称始终显示，不要求用户对六个小色块做悬浮辨认。
        // 下拉项保留完整触控高度，切换只改变编辑焦点。
        // 下拉入口与选项都保留最小手指高度，不改变主题横向留白或圆角。
        // 样式仅覆盖本下拉框，后续取色器和相邻窗口立即恢复。
        // 大字号超出触控下限时沿用主题，不用负内边距压缩文字。
        // 上下留白成对计算，使可见边框与实际可点击区域一致。
        const auto padding = ImGui::GetStyle().FramePadding;
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(padding.x,
                   std::max(padding.y,
                            (48 * scale - ImGui::GetFontSize()) * .5f)));
        ImGui::SetNextItemWidth(-1);
        if ( FeedbackBeginCombo("##TouchColorSlot",
                                TR(slots[active]).data()) ) {
            for ( std::size_t i = 0; i < slots.size(); ++i ) {
                ImGui::PushID(static_cast<int>(i));
                if ( FeedbackSelectable(TR(slots[i]).data(),
                                        i == active,
                                        0,
                                        { 0, 48 * scale }) )
                    m_activeColorSlot = static_cast<Logic::NoteColorSlot>(i);
                ImGui::PopID();
            }
            FeedbackEndCombo();
        }
        ImGui::PopStyleVar();
        // 选区操作放在取色前，低高度手机不必滚动到底才能确认范围。
        // 许可读应用公开标量，不借用可变 SessionContext 查询选择集合。
        // 无选区由业务动作返回空结果；按钮不扫描所有可见或隐藏实体。
        // 选中谱面和真正提交之间仍可能变化，后台会再次校验业务状态。
        // 选区填色与桌面保留相同的类型权限，普通物件不额外要求专业模式。
        // 草稿等受限类型仍由业务层的原有可编辑性检查决定。
        const bool editable = MenuUtil::hasActiveBeatmap(false) &&
                              m_editorApplicationService &&
                              !m_editorApplicationService->isPlaybackPlaying();
        // 两个按钮明确分开填色与恢复继承，不能靠长按或右键暗中区分。
        // 命令只在有效释放激活时入队，手指滚过按钮不应触发确认。
        ImGui::BeginDisabled(!editable);
        if ( FeedbackButton(TR("ui.toolbar.note_palette.apply_selected").data(),
                            { -1, 48 * scale }) )
            MenuUtil::dispatchCommand(Logic::CmdApplyNotePaletteToSelection{
                m_paletteColors, false, true });
        if ( FeedbackButton(TR("ui.mobile.color.clear_selection").data(),
                            { -1, 48 * scale }) )
            MenuUtil::dispatchCommand(Logic::CmdApplyNotePaletteToSelection{
                m_paletteColors, true, true });
        // 每次确认只产生一个批量动作；清除不是逐槽发送六个可撤销命令。
        // 默认桌面命令仍可沿用原行为，移动附加标志只约束本页提交。
        ImGui::EndDisabled();
        // 颜色选择器独占自己的无滚动子域，二维取色不能被外层页面滚动夺走。
        // 子域外的标题、说明和空白仍允许正常拖动滚动。
        // 图形取色域固定上限防止平板产生过大控件，小屏则按可用宽度缩小。
        // 高度与宽度一起计算，不能只压低子域而裁掉 Alpha 和色相条。
        const float pickerWidth =
            std::min(ImGui::GetContentRegionAvail().x, 220 * scale);
        ImGui::BeginChild(
            "TouchColorPicker",
            { 0, pickerWidth },
            0,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetNextItemWidth(pickerWidth);
        // 槽位在下拉选择后重新取引用，只在当前帧使用，不缓存数组地址。
        // 拖动更新的是共享画笔值，不在界面线程修改谱面元数据。
        auto& color =
            m_paletteColors[static_cast<std::size_t>(m_activeColorSlot)];
        // 无侧栏和数字输入，所有通道均通过图形拖动；Alpha 同样保留。
        // 仅同步画笔，释放也不隐式调用选区填色。
        // 禁止隐式颜色选项菜单，手机不要求鼠标右键才能选择取色模式。
        // 连续取色按正常帧率即时反馈，不增加等待、睡眠或定时线程。
        if ( ImGui::ColorPicker4("##TouchPalettePicker",
                                 &color.r,
                                 ImGuiColorEditFlags_NoInputs |
                                     ImGuiColorEditFlags_NoSidePreview |
                                     ImGuiColorEditFlags_NoLabel |
                                     ImGuiColorEditFlags_NoOptions |
                                     ImGuiColorEditFlags_AlphaBar) )
            pushColorCommands(m_activeColorSlot, color, false);
        // 子域结束即归还输入归属，页面空白滑动仍由普通滚动器解释。
        // 不创建悬浮提示、定时任务或跨窗口的取色器指针。
        ImGui::EndChild();
        // 方案管理单独进入，避免管理按钮把手指调色页挤成桌面密度。
        // 原方案服务继续负责保存、命名校验和项目继承。
        // 高级管理是显式进入的第二页，不把所有桌面按钮堆在主取色域。
        // 点外关闭后再次打开仍回到快捷页，不继承过期浮层大小。
        if ( FeedbackButton(TR("ui.mobile.color.details").data(),
                            { -1, 48 * scale }) )
            m_mobilePaletteDetails = true;
        // 关闭只改变本浮层显示标志，不关闭画布或清理用户当前选区。
        // 取色已即时同步画笔，因此不需要在关闭时补交任何选区操作。
        if ( FeedbackButton(TR("ui.common.close").data(), { -1, 48 * scale }) )
            m_showColorPopup = false;
    }
    ImGui::End();
}
}  // namespace MMM::UI
