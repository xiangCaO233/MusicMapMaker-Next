/// @file MobileWorkbench.cpp
/// @brief 移动工作台只负责入口布局，实际编辑由既有事务和菜单动作完成。
/// @details
/// 输入边界：
/// - 轻点释放才激活动作；从按钮起手的滑动由移动滚动器先仲裁。
/// - 复制、剪切和粘贴使用同一系统剪贴板协议，不能生成移动专用载荷。
/// - 撤销与重做归当前谱面，不能通过工具窗口的焦点推断历史归属。
/// - 低频动作执行时仍由公共处理器检查活动谱面和联机编辑权限。
/// 布局边界：
/// - 本文件只在移动外壳的工作台窗口内绘制，不创建额外停靠节点。
/// - 两列的宽度来自当前内容区，主题内边距和滚动条已经由父窗口扣除。
/// - 同行标签按当前语言预量高度，不能使用中文宽度估计英文按钮。
/// - 超出一行的文字在单元内换行，整列高度增加而不缩小用户设定字号。
/// 状态边界：
/// - 工具内容归主界面的 ToolbarView 所有，工作台仅借用稳定对象。
/// - 选中底色读权威标量，不能用按钮最后一次点击替代实际模式。
/// - 分拍线只发布 Always 或 Hidden，保留桌面 NearCursor 的独立实现。
/// - 布局和视觉配置在明确点击时复制；每帧只借用只读配置引用。
/// - 打开表格和谱面设置通过语义动作，不读写后台会话的可变容器。
#include "ui/workspace/MobileWorkbench.h"

#include "common/EditTool.h"
#include "common/LogicCommands.h"
#include "common/SelectionOperation.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/IEditorApplicationService.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/MobileButton.h"

#include <entt/entity/entity.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <optional>

namespace MMM::UI
{
namespace
{
/// @brief 少量非菜单操作仍通过原有配置和命令通道执行。
/// @details 分拍线只在 Always 与 Hidden 间切换，不提供鼠标附近显示模式。
enum class Operation {
    Action,
    /// @brief 只切换相机姿态启用状态，用户保存的位置与旋转不重置。
    Camera,
    /// @brief 两态显示开关，开启固定输出 Always。
    BeatLines,
    /// @brief 非线性时序映射与底层线性标志互为反义。
    Mapping,
    /// @brief 配色工具仍受原有专业模式能力约束。
    ColorBrush,
    ColorEraser
};
/// @brief 固定入口只保存稳定身份与翻译键，不持有菜单处理器所有权。
/// @details 表的生命周期覆盖绘制调用，字符串均为静态字面量。
/// 入口身份不能从翻译结果生成，否则切换语言会改变按钮反馈和动作路由。
/// 操作枚举只标记不经过菜单的少量配置开关，不替代原动作注册表。
struct Entry {
    const char* m_id;     ///< 语义动作或局部按钮的稳定身份。
    const char* m_label;  ///< 可见短标题随当前语言解析。
    const char* m_icon;   ///< 使用皮肤已经加载的专用图标字形。
    Operation m_operation{ Operation::Action };  ///< 普通编辑优先复用菜单动作。
};
/// @brief 固定网格单元内显示完整图文，长文字换行而不向邻列扩张。
/// @details 外层预先测量同一行两个标签，保证背景、命中区和下一行起点对齐。
/// @param id 与可见文案独立的稳定按钮身份。
/// @param icon 图标字形使用 pure_icons，缺失字体时使用当前字体兜底。
/// @param label 经过当前语言解析的纯文字，不包含图标私用区编码。
/// @param size 整行预量后确定的单元宽高，不允许长文字反向扩宽。
/// @param scale 当前内容倍率，只控制触摸尺寸与图文间隔。
/// @param selected 表达持久状态，普通动作按钮保持非选中。
/// @return 统一反馈入口确认释放激活时返回真。
/// 背景和图文共享一个 ID，点击图标不能绕过移动滚动仲裁。
/// 绘图使用当前窗口裁剪，滚动到标签上方也不会覆盖中央页签。
/// @warning 每个按钮每帧固定测量与绘图，不加载资源或保存跨帧字体指针。
bool gridButton(const char* id, const char* icon, const char* label,
                ImVec2 size, float scale, bool selected)
{
    ImGui::PushID(id);
    // 隐藏标签保持反馈身份稳定，显示文字和图标共用同一个触摸目标。
    // 不把正文传给基础按钮，防止其按文字单行宽度反向扩大网格单元。
    // 正文只参与外层行高和当前局部绘图，不参与 ImGui 命中身份。
    // 按钮反馈时间保存在本窗口，滚动离开后不会产生全局动画任务。
    const bool  clicked = MobileButton("##grid", size, selected);
    const auto  min     = ImGui::GetItemRectMin();
    const auto  max     = ImGui::GetItemRectMax();
    const float width   = std::max(1.0f, max.x - min.x - 12 * scale);
    // 与外层行高使用同一个换行宽度，文本测量和最终绘图才能一致。
    // 宽度下限只用于防止极端窄窗口向字体测量传入零或负值。
    const auto text = ImGui::CalcTextSize(label, nullptr, false, width);
    auto*      font = Config::SkinManager::instance().getFont("pure_icons");
    if ( !font ) font = ImGui::GetFont();
    // 图标与正文各用自身字体，增大字号时按整行高度布局而不缩小文字。
    const float iconSize   = 18 * scale;
    const auto  iconBounds = font->CalcTextSizeA(iconSize, 10000, 0, icon);
    const float top =
        min.y + (max.y - min.y - iconSize - text.y - 4 * scale) * .5f;
    auto* draw = ImGui::GetWindowDrawList();
    // 禁用样式已经由外层生效，图文颜色必须从当前样式读取，不能硬编码高亮。
    // 裁剪栈只在本次绘制中增加一层，返回前立即恢复父窗口的滚动裁剪。
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    // 裁剪限定在本按钮内，极端窄宽度也不会盖住右列或滚动条。
    // 不修改全局字体缩放；两种字体只在自己的 AddText 调用中生效。
    // 文字块可有多行，但整个块以按钮中心定位，图标固定排在块的上方。
    draw->PushClipRect(min, max, true);
    draw->AddText(font,
                  iconSize,
                  { min.x + (max.x - min.x - iconBounds.x) * .5f, top },
                  color,
                  icon);
    draw->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        { min.x + (max.x - min.x - text.x) * .5f, top + iconSize + 4 * scale },
        color,
        label,
        nullptr,
        width);
    draw->PopClipRect();
    ImGui::PopID();
    return clicked;
}

/// @brief 绘制工具的触控意图显式二选一，避免手指偏移把普通长条变成折线。
/// @param width 已扣除主题间距的单列宽度。
/// @param gap 两列之间的主题间距。
/// @param scale 与其它移动按钮一致的触摸尺寸倍率。
/// @warning 每帧只读方案和配置标量，点击时只改 UI 线程的后续起笔意图。
void renderTouchDrawingControls(float width, float gap, float scale)
{
    auto&       profile  = Interaction::activeProfile();
    const auto* simple   = TR("ui.mobile.draw.note_hold").data();
    const auto* polyline = TR("ui.mobile.draw.polyline").data();
    const float wrap     = std::max(1.0f, width - 12 * scale);
    // 两列按当前语言共同量高；图标和正文不会挤到下一行或覆盖相邻单元。
    const float textHeight =
        std::max(ImGui::CalcTextSize(simple, nullptr, false, wrap).y,
                 ImGui::CalcTextSize(polyline, nullptr, false, wrap).y);
    const ImVec2 size{ width, std::max(48 * scale, textHeight + 28 * scale) };
    // 显示态从方案读取，切换工具再返回仍保留最近一次意图。
    // 简单模式不写入桌面配置，也不修改真实 Shift 或 Ctrl 状态。
    // 用户需要明确选择折线，普通长条的手指漂移不会自动切换模式。
    // 起笔时会锁存结果，笔画不依赖工作台下一帧是否仍然可见。
    const bool simpleMode =
        profile.noteHoldBrush(Interaction::Surface::MainCanvas) &&
        !profile.objectEraser();
    if ( gridButton(
             "TouchNoteHold", ICON_MMM_PEN, simple, size, scale, simpleMode) ) {
        profile.setObjectEraser(false);
        profile.setNoteHoldBrush(true);
    }
    ImGui::SameLine(0, gap);
    const bool enabled = Config::AppConfig::instance()
                             .getEditorConfig()
                             .settings.enablePolylineEditing;
    // 既有折线能力开关继续生效；这里不暗中开启专业模式或更改谱面配置。
    ImGui::BeginDisabled(!enabled);
    if ( gridButton("TouchPolyline",
                    ICON_MMM_POLYLINE,
                    polyline,
                    size,
                    scale,
                    !profile.noteHoldBrush(Interaction::Surface::MainCanvas) &&
                        !profile.objectEraser()) ) {
        profile.setObjectEraser(false);
        profile.setNoteHoldBrush(false);
    }
    ImGui::EndDisabled();
    // 禁用原因使用正文，手机没有悬浮鼠标也能知道从哪里开启能力。
    // 说明遵循当前列宽换行，不缩小用户设定字号或截断英文。
    // 此处仅解释全局门禁，不在绘制按钮上暗中改变谱面编辑权限。
    if ( !enabled )
        ImGui::TextWrapped("%s", TR("ui.mobile.draw.enable_polyline").data());
    // 独立物件擦除不能与配色橡皮混为一谈，正文说明整条折线删除范围。
    // 与绘制意图放在同一组，开关不创建删除或选择历史。
    // 英文标题按按钮宽度量高，说明按整个可用区域换行。
    // 不靠缩小字号容纳长句，物件擦除和配色橡皮的职责明确分开。
    // 开关本身既不删除当前选择，也不产生任何撤销节点。
    const auto* erase = TR("ui.mobile.erase.objects").data();
    const float eraseHeight =
        ImGui::CalcTextSize(erase, nullptr, false, wrap).y;
    if ( gridButton("TouchObjectEraser",
                    ICON_MMM_ERASER,
                    erase,
                    { width, std::max(48 * scale, eraseHeight + 28 * scale) },
                    scale,
                    profile.objectEraser()) )
        profile.setObjectEraser(!profile.objectEraser());
    // 说明常驻可见，无需手机缺失的 Hover 才知道折线为整条删除。
    // 退出擦除后仍沿用最近的简单或折线绘制偏好。
    if ( profile.objectEraser() )
        ImGui::TextWrapped("%s", TR("ui.mobile.erase.hint").data());
}

/// @brief 触控选择工具使用独立两列布局，集合运算仍由公共命令解释。
/// @param width 单列的可用宽度，已经扣除主题间距。
/// @param gap 当前主题的同行间距，不重设全局样式。
/// @param scale 移动外壳提供的逻辑尺寸倍率。
/// @warning 每帧只读取方案标量和活动谱面标志，点击时才发布选择命令。
void renderTouchSelectionControls(float width, float gap, float scale)
{
    auto& profile = Interaction::activeProfile();
    // 只由移动工作台调用，桌面菜单和资源选择面板的布局保持独立。
    // 四个稳定 ID 与翻译显示文字分离，切换语言不会改变触摸目标身份。
    /// @brief 固定入口描述，不保存窗口或字体等跨帧资源的指针。
    struct Choice {
        /// @brief 当前按钮的稳定身份。
        const char* m_id;
        /// @brief 复用公共选择语义的翻译键。
        const char* m_label;
        /// @brief 文字之外的触控识别图标。
        const char* m_icon;
        /// @brief 空值表示立即清空，其余值切换后续选择意图。
        std::optional<Logic::SelectionOperation> m_operation;
    };
    // 描述表是静态常量，不随每帧读取配置重新建立动态文本或容器。
    static constexpr std::array choices{
        Choice{ "TouchReplace",
                "ui.mobile.selection.replace",
                ICON_MMM_SQUARE_SELECT,
                Logic::SelectionOperation::Replace },
        Choice{ "TouchAdd",
                "ui.mobile.selection.add",
                ICON_MMM_PLUS,
                Logic::SelectionOperation::Add },
        Choice{ "TouchSubtract",
                "ui.mobile.selection.subtract",
                ICON_MMM_ERASER,
                Logic::SelectionOperation::Subtract },
        Choice{ "TouchClear",
                "ui.mobile.selection.clear",
                ICON_MMM_CLOSE,
                std::nullopt }
    };
    // 先测量同行文案，英文换行时一起增高，不能截断右列或侵占下一行。
    // 选中态来自方案的实际值，关闭工作台或重新打开仍能看见当前意图。
    for ( std::size_t i = 0; i < choices.size(); i += 2 ) {
        const float wrap       = std::max(1.0f, width - 12 * scale);
        const float textHeight = std::max(
            ImGui::CalcTextSize(
                TR(choices[i].m_label).data(), nullptr, false, wrap)
                .y,
            ImGui::CalcTextSize(
                TR(choices[i + 1].m_label).data(), nullptr, false, wrap)
                .y);
        // 行高只增加到容纳当前语言所需的程度，触控下限仍为 48dp。
        // 两列共用同一高度，长文案不能覆盖同行按钮或下一行。
        const ImVec2 size{ width,
                           std::max(48 * scale, textHeight + 28 * scale) };
        for ( std::size_t j = i; j < i + 2; ++j ) {
            const auto& choice = choices[j];
            // 模式可以预先选择；清空只对已有活动谱面开放，避免跨窗口误操作。
            const bool clear = !choice.m_operation;
            ImGui::BeginDisabled(clear && !MenuUtil::hasActiveBeatmap(true));
            if ( gridButton(
                     choice.m_id,
                     choice.m_icon,
                     TR(choice.m_label).data(),
                     size,
                     scale,
                     choice.m_operation &&
                         profile.selectionOperation() == choice.m_operation) ) {
                if ( choice.m_operation ) {
                    // 点选与框选后续都读取这个值，不模拟任何键盘修饰键。
                    // 切换不清理现有选区，追加/减选必须以当前集合为起点。
                    profile.setSelectionOperation(*choice.m_operation);
                } else {
                    // 显式清空不受追加/减选模式影响，也不切换当前工具。
                    Interaction::dispatchCommand(Event::LogicCommandEvent(
                        Logic::CmdSelectEntity{ entt::null, true }));
                }
            }
            // 禁用域只覆盖当前按钮，不影响下一列可预先切换的模式。
            // 按钮复用移动反馈，滑动取消与点击动画仍由外壳处理。
            ImGui::EndDisabled();
            if ( j == i ) ImGui::SameLine(0, gap);
        }
    }
    ImGui::Separator();
}

/// @brief 从权威配置计算持久状态，按钮高亮不依赖上次点击或悬浮。
/// @param entry 固定入口描述，普通动作没有持久选中态。
/// @param visual 当前 UI 配置只读引用，不访问逻辑会话。
/// @param tool 应用服务发布的当前工具枚举，非本窗口上次点击值。
/// @return 布尔开关或工具当前生效时返回真。
/// NearCursor 从旧配置读入时视为可见；首次关闭后只能再开启 Always。
/// 这只限定移动按钮的输出，不主动改写用户桌面模式偏好。
/// @warning 每帧仅读取几个标量，不复制 EditorConfig 或取得会话锁。
bool selected(const Entry& entry, const Config::VisualConfig& visual,
              Logic::EditTool tool)
{
    // 本查询没有副作用，渲染两列时也不会改变随后动作看到的配置。
    // 未列出的普通动作返回非选中，不用历史状态伪造开关状态。
    switch ( entry.m_operation ) {
    case Operation::Camera: return visual.canvasCameraPoseEnabled;
    case Operation::BeatLines:
        return visual.beatLineDisplayMode !=
               Config::BeatLineDisplayMode::Hidden;
    case Operation::Mapping: return !visual.enableLinearScrollMapping;
    case Operation::ColorBrush: return tool == Logic::EditTool::ColorBrush;
    case Operation::ColorEraser: return tool == Logic::EditTool::ColorEraser;
    default: return false;
    }
}

/// @brief 只在真实触摸激活时执行，配置更新及历史事务沿用现有入口。
/// @param entry 已由统一按钮确认的操作意图，不读取任何键盘修饰状态。
/// @param manager 当前 UI 管理器用于建立动作和应用服务上下文。
/// @details 动作处理器拥有确认弹窗、批量事务及权限判断，工作台不复制实现。
/// 配置开关先从最新完整值复制，避免同帧其它 UI 的设置被旧草稿覆盖。
/// 布局持久化只发生在离散激活路径，普通绘制不会提交保存任务。
void activate(const Entry& entry, UIManager& manager)
{
    if ( entry.m_operation == Operation::Action ) {
        // 未注册或不可执行的动作由桥返回失败，不退化为模拟快捷键。
        // 相同身份继续触发已有教程确认，不制造另一套移动教程完成事件。
        Interaction::executeAction(entry.m_id, &manager);
        return;
    }
    // 配色工具走逻辑命令通道，由会话继续校验联机编辑权限与专业模式能力。
    if ( entry.m_operation == Operation::ColorBrush ||
         entry.m_operation == Operation::ColorEraser ) {
        MenuUtil::dispatchCommand(
            Logic::CmdChangeTool{ entry.m_operation == Operation::ColorBrush
                                      ? Logic::EditTool::ColorBrush
                                      : Logic::EditTool::ColorEraser });
        return;
    }
    auto& app    = Config::AppConfig::instance();
    auto  config = app.getEditorConfig();
    // 复制发生在离散点击路径，保留配置中与此次开关无关的字段及用户相机姿态。
    switch ( entry.m_operation ) {
    case Operation::Camera:
        config.visual.canvasCameraPoseEnabled =
            !config.visual.canvasCameraPoseEnabled;
        break;
    case Operation::BeatLines:
        config.visual.beatLineDisplayMode =
            config.visual.beatLineDisplayMode ==
                    Config::BeatLineDisplayMode::Hidden
                ? Config::BeatLineDisplayMode::Always
                : Config::BeatLineDisplayMode::Hidden;
        break;
    case Operation::Mapping:
        config.visual.enableLinearScrollMapping =
            !config.visual.enableLinearScrollMapping;
        break;
    default: return;
    }
    // 应用服务统一广播与保存布局，移动端不维护另一套渲染开关副本。
    // 服务暂不可用时忽略激活，不能仅改 UI 值造成画面与逻辑状态分离。
    // 完整广播与保存顺序沿用其它工具，重启后不会回退到此次点击之前。
    if ( auto* service = manager.getEditorApplicationService() ) {
        service->updateEditorConfig(config);
        app.saveLayoutChanges();
        app.save();
    }
}
}  // namespace

/// @brief 常用编辑与工具入口按两列排列，整个工作台只保留一层纵向滚动。
/// @details 业务能力与确认窗口由既有动作负责；这里不模拟键盘或桌面菜单点击。
/// @param manager 工作台所在 UI 管理器，不能跨帧保存其视图观察指针。
/// @param scale 当前移动倍率，与按钮最小命中高度使用相同单位。
/// 同一固定表只在两列间排布，不因剩余高度不足改变按钮顺序。
/// 工具浮层在外壳帧末提交，网格本身不能调用桌面工具栏完整 update。
/// 工作台显隐和宽度归移动停靠树，正文仅消费已经扣好边距的内容区域。
/// @warning 每帧只访问固定表、本地配置和已发布的工具状态，不加会话长锁。
void drawMobileWorkbench(UIManager& manager, float scale)
{
    // 高频物件编辑与历史动作已收拢至画布长按快捷菜单，工作台仅常驻视图开关与辅助工具。
    // 辅助表由既有视图能力打开，不能因为时间线面板隐藏而创建第二个实例。
    // 谱面设置使用设置页路由；打开后由外壳自然释放工作台占用空间。
    const std::array entries{
        Entry{ "OpenTimingPointsTableAction",
               "ui.mobile.timing_table",
               ICON_MMM_CLOCK },
        Entry{ "OpenAnnotationTableAction",
               "ui.mobile.annotation_table",
               ICON_MMM_BARS },
        Entry{ "OpenBeatmapSettingsAction",
               "ui.edit.beatmap_settings",
               ICON_MMM_COG },
        Entry{ "Camera", "ui.mobile.camera", ICON_MMM_EYE, Operation::Camera },
        Entry{ "BeatLines",
               "ui.mobile.beat_lines",
               ICON_MMM_BARS,
               Operation::BeatLines },
        Entry{ "Mapping",
               "ui.toolbar.short.scroll_timing_mapping",
               ICON_MMM_EYE,
               Operation::Mapping },
        Entry{ "ColorBrush",
               "ui.toolbar.short.color_brush",
               ICON_MMM_PEN,
               Operation::ColorBrush },
        Entry{ "ColorEraser",
               "ui.toolbar.short.color_eraser",
               ICON_MMM_ERASER,
               Operation::ColorEraser }
    };
    // 工具内容独立于入口网格，只有真正打开时才绘制原工具设置。
    // 先绘制视图与显示模式开关，再集中提供布局、磁铁、音效和调色盘。
    auto* toolbar = [&]() -> ToolbarView* {
        auto* dock = manager.getView<MainDockSpaceUI>("MainDockSpaceUI");
        return dock ? &dock->toolbarView() : nullptr;
    }();
    const auto* service = manager.getEditorApplicationService();
    const auto  tool =
        service ? service->currentTool() : Logic::EditTool::Marquee;
    const auto& app    = Config::AppConfig::instance();
    const auto& config = app.getEditorConfig();
    // 主题间距只扣一次；两列加间距必须恰好位于父窗口的内容宽度内。
    // 最小宽度保留指尖目标，屏幕变窄时不能把一行偷偷改成三列或更密集排列。
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width =
        std::max(44 * scale, (ImGui::GetContentRegionAvail().x - gap) * .5f);
    // 保存直接复用正式动作，文件格式偏好、冲突提示与 SAF 回写仍由原处理器负责。
    // 始终占据工作台第一行，不要求用户使用键盘或展开长按菜单。
    ImGui::BeginDisabled(!MenuUtil::hasActiveBeatmap(true));
    if ( MobileActionButton("MobileSave",
                            ICON_MMM_SAVE,
                            TR("ui.file.save").data(),
                            { -1.0f, 48 * scale }) )
        Interaction::executeAction("SaveBeatmapAction", &manager);
    ImGui::EndDisabled();
    // 选择方式紧邻保存入口，只在触控选择工具下出现，不挤占绘制工具布局。
    if ( tool == Logic::EditTool::Marquee &&
         Interaction::activeProfile().touchGestures() )
        renderTouchSelectionControls(width, gap, scale);
    // 绘制意图属于移动工作台，公共画布只接收轻量命令，不包含移动布局头。
    if ( tool == Logic::EditTool::Draw &&
         Interaction::activeProfile().touchGestures() )
        renderTouchDrawingControls(width, gap, scale);
    // 配色手势的提交与取消范围常驻解释，手机没有 Hover 入口。
    // 选区填色在调色页明确确认，不由经过物件或改调色盘触发。
    if ( tool == Logic::EditTool::ColorBrush ||
         tool == Logic::EditTool::ColorEraser )
        ImGui::TextWrapped("%s", TR("ui.mobile.color.stroke_hint").data());
    // 固定两列；紧凑高度使全部六行十二个常驻按钮在手机横屏下无需滚动即可完整展示。
    for ( std::size_t i = 0; i < entries.size(); i += 2 ) {
        // 先量同行的两种文案，再提交任何按钮，避免后量到的长文案压住下一行。
        // 当前行内不切换语言，翻译系统提供的只读指针只借用至本轮绘制结束。
        const auto* left  = TR(entries[i].m_label).data();
        const auto* right = TR(entries[i + 1].m_label).data();
        const float wrap  = std::max(1.0f, width - 12 * scale);
        const float textHeight =
            std::max(ImGui::CalcTextSize(left, nullptr, false, wrap).y,
                     ImGui::CalcTextSize(right, nullptr, false, wrap).y);
        const ImVec2 size{ width,
                           std::max(48 * scale, textHeight + 28 * scale) };
        for ( std::size_t j = i; j < i + 2; ++j ) {
            const auto& entry = entries[j];
            // 配色工具沿用专业模式门禁，禁用时不提交会被后台拒绝的命令。
            const bool enabled = config.settings.professionalMode ||
                                 (entry.m_operation != Operation::ColorBrush &&
                                  entry.m_operation != Operation::ColorEraser);
            ImGui::BeginDisabled(!enabled);
            if ( gridButton(entry.m_id,
                            entry.m_icon,
                            TR(entry.m_label).data(),
                            size,
                            scale,
                            selected(entry, config.visual, tool)) )
                activate(entry, manager);
            // 禁用只包围当前单元，不泄漏到同行另一个可用功能。
            // SameLine
            // 明确使用主题间距，不能依据先前文字的实际绘图宽度排下一列。
            ImGui::EndDisabled();
            if ( j == i ) ImGui::SameLine(0, gap);
        }
    }
    // 主界面尚未创建时保留普通菜单动作入口，工具内容缺失则停止本组绘制。
    // 不为补齐入口临时创建工具对象，以免丢失皮肤切换和项目调色偏好的状态。
    if ( !toolbar ) return;
    // 浮层入口在独立一组，活动底色明确表示哪一工具正在打开。
    const std::array tools{ ToolbarView::MobileTool::Layout,
                            ToolbarView::MobileTool::Magnet,
                            ToolbarView::MobileTool::SoundEffect,
                            ToolbarView::MobileTool::Palette };
    const std::array labels{ "ui.toolbar.short.layout",
                             "ui.toolbar.short.magnet_tool",
                             "ui.toolbar.short.key_sound_tool",
                             "ui.toolbar.short.note_palette" };
    const std::array icons{ ICON_MMM_TRACK_LAYOUT,
                            ICON_MMM_MAGNET,
                            ICON_MMM_HIT_SFX,
                            ICON_MMM_PAINT_BRUSH };
    // 分组分隔使用当前主题颜色与厚度，不覆盖全局样式。
    // 工具种类、标签与图标以相同固定顺序配对，不依赖本地化后的字符串匹配。
    ImGui::Separator();
    for ( std::size_t i = 0; i < tools.size(); i += 2 ) {
        const float wrap       = std::max(1.0f, width - 12 * scale);
        const float textHeight = std::max(
            ImGui::CalcTextSize(TR(labels[i]).data(), nullptr, false, wrap).y,
            ImGui::CalcTextSize(TR(labels[i + 1]).data(), nullptr, false, wrap)
                .y);
        for ( std::size_t j = i; j < i + 2; ++j ) {
            ImGui::PushID(static_cast<int>(j));
            if ( gridButton(
                     "Tool",
                     icons[j],
                     TR(labels[j]).data(),
                     { width, std::max(48 * scale, textHeight + 28 * scale) },
                     scale,
                     toolbar->mobileToolOpen(tools[j])) ) {
                // 必须在按钮提交后取得锚点，滚动后的屏幕位置才与用户触点一致。
                // 工具内容会再按安全区夹取，按钮靠近底部时不会把浮层推到屏幕外。
                const auto anchor = ImGui::GetItemRectMin();
                toolbar->toggleMobileTool(tools[j], { anchor.x, anchor.y });
            }
            // 工具种类建立独立作用域，同名局部按钮不共享按下动画。
            // 弹层开关不更改主区标签选择，工作台不会因自身按钮抢焦点而隐藏。
            ImGui::PopID();
            if ( j == i ) ImGui::SameLine(0, gap);
        }
    }
}
}  // namespace MMM::UI
