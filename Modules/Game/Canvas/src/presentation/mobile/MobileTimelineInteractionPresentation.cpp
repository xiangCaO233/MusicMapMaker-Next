#include "MobileTimelineInteractionPresentation.h"
#include "MobileAuxiliaryWindow.h"
#include "MobileCanvasGesture.h"
#include "MobileNavigationRouting.h"
#include "MobilePointerInputScope.h"
#include "MobilePointerOwnership.h"
#include "MobileReadPress.h"
#include "MobileTimingCurveEditor.h"
#include "MobileTimingRange.h"
#include "common/SelectionOperation.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "presentation/TimelineInteractionPresentation.h"
#include "ui/Icons.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <string>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief Timing 触点仲裁与菜单仅在移动编译单元实现，不改变桌面状态机。
/// @details 点按在松手后回放，拖动先回放原点，再交给已有点与区间逻辑。
/// 多指接管时取消未提交预览，原始代际锁住残留单指，防止导航后误画。
/// @par 单指事务边界
/// 触点刚按下不等于用户已经选择绘制，也可能准备长按或双指导航。
/// 只有松手或超过移动容差后，才向业务控制器交付第一次按下。
/// 控制器继续决定命中、类型、吸附与创建合法性，本类不复制这些算法。
/// 轻点的释放坐标固定为最初落点，避免手指自然抖动制造小段落。
/// 拖动先交原点再交当前位置，快速手势不能丢失起拍或最终范围。
/// 点、插值、移动都在原有确认边界提交，不在候选阶段写入模型。
/// @par 多指与系统接管
/// 原始触点峰值比鼠标合成更早可见，必须先于回放进行仲裁。
/// 第二指只接管视图，不能成为原单指事务的提交终点。
/// 接管后即使第二指先松开，残留第一指也不能重新开始编辑。
/// 系统 Cancel 同样丢弃编辑预览，不能误作抬手提交或历史轻触。
/// 原始代际直到下一次真实按下才允许重新分配给其他窗口。
/// @par 会话与弹层
/// 选区基线仅在起笔复制，常态帧不完整遍历实体或构造临时集合。
/// 同路径重新加载会更换实例号，旧实体编号不得恢复到新实例。
/// 菜单打开冻结目标和时间，随后移动到按钮不会改变粘贴锚点。
/// 参数编辑沿用公共窗口，只有菜单呈现和输入分类归移动端所有。
/// @par 局部范围
/// 首尾候选拥有独立事务，不复用正文的整组选区移动标志。
/// 把手只属于单选段落，标记中央和相邻泳道继续交给原选择器。
/// 正常释放将时间与完整插值定义交给原命令，逻辑层仍是最终合法性来源。
/// 局部附加绘制由中立虚接口调用，公共画布头不包含移动候选类型。
/// 桌面派生退路不读取端点状态，原快捷键与鼠标拖动不经过移动夹取规则。
class MobileTimelineInteractionPresentation final
    : public TimelineInteractionPresentation
{
public:
    /// @brief 触屏表格使用外壳安全区域，常驻工具栏不遮住标题和关闭按钮。
    /// @warning 每帧只提交本地几何，不改动时间线模型或系统显示设置。
    void prepareTableWindow() const override
    {
        if ( touchTable() ) prepareMobileAuxiliaryWindow();
    }
    /// @brief 安卓接鼠标后沿用原桌面表格，触控才启用只读单元格与显式编辑。
    bool touchTable() const override
    {
        return UI::Interaction::activeProfile().touchGestures();
    }
    /// @brief 点按选择，静止长按进入原参数窗；滑动与第二指只交给页面仲裁。
    /// @warning 每个可见行只分类固定触点，不复制选区或等待逻辑线程。
    // 返回值只有离散意图，表格自身仍负责原有集合操作与参数草稿。
    // 页面滚动控制器可能屏蔽合成鼠标按下，因此直接观察原始触点。
    // 行命中同时要求原始起点与当前窗口归属，滑入邻窗不会被接管。
    // 不向原始跟踪器写入事件，也不以修饰键模拟多选。
    int tableRow(std::uint32_t id, const ImVec2& min,
                 const ImVec2& max) override
    {
        if ( !touchTable() ) return 0;
        return static_cast<int>(m_tablePress.update(
            id,
            MobileReadPress::originInside(min, max) &&
                ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
            true));
    }
    /// @brief 窗口隐藏时拒绝残留触点，不重放过期轻点。
    void resetTableInput() override { m_tablePress.cancel(); }

    /// @brief 移动图表交互独立保存，经典方案仍使用默认指针控件。
    /// @warning 模态帧只消费窗口副本，不访问谱面注册表。
    // 移动图表的候选与热区均由私有成员管理，不进入 TimelineCanvas 的公开定义。
    // 经典方案直接转交基类，数字字段与原有快捷键不经过触点夹取。
    bool bezierControls(TimingInterpolation& curve, const ImVec2& size) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::bezierControls(curve, size);
        return m_curveEditor.bezier(
            curve, size, Config::AppConfig::instance().getWindowContentScale());
    }
    /// @brief 触屏图表阻止父页抢笔；不在桌面函数中嵌入平台条件宏。
    /// @warning 每帧建立固定子窗，调用者负责配对结束。
    // 子窗只作为局部输入边界，不能把整个参数表单标记为不可滚动。
    // 配对义务由返回值明确表达，调用方不依赖子窗的首次隐藏测量结果。
    bool beginCurveSurface(const char* id, const ImVec2& size) override
    {
        return UI::Interaction::activeProfile().touchGestures() &&
               m_curveEditor.beginSurface(id, size);
    }
    /// @brief 自由绘制及取消只影响参数窗口副本。
    /// @warning 每帧固定 129 格点预算，不拟合或发布逻辑命令。
    // 手绘图共享原来的 129 个格点，平台层不改变拟合算法、权重或输出语义。
    // 第二指和 Cancel 只丢弃本笔草稿，不借用全局撤销栈恢复。
    bool sketchInput(TimingFunctionEditorState& s, const ImVec2& o,
                     const ImVec2& size) override
    {
        return UI::Interaction::activeProfile().touchGestures() &&
               m_curveEditor.sketch(s, o, size);
    }
    /// @brief 预览使用原双指导航器；谱面历史不接收图内二指轻触。
    /// @warning 每帧只改本地视野，不复制数学缓存。
    // 图内导航不消费双指撤销或三指重做标志，避免修改背景谱面。
    // 预览倍率归局部视野，不覆盖主画布或 Timing 的缩放配置。
    bool previewInput(TimingInterpolationPreview& p, const ImVec2& o,
                      const ImVec2& size) override
    {
        return UI::Interaction::activeProfile().touchGestures() &&
               m_curveEditor.preview(
                   p,
                   o,
                   size,
                   Config::AppConfig::instance().getWindowContentScale());
    }
    /// @brief 重置与放大缩小按钮放在图外，避免点击落成一次绘制。
    // 显式入口满足单手操作与发现性，手势不是唯一的缩放或重置方式。
    // 按钮在图外排版，沿用统一反馈入口及当前主题的圆角和间距。
    void previewButtons(TimingInterpolationPreview& p) override
    {
        if ( UI::Interaction::activeProfile().touchGestures() )
            m_curveEditor.previewButtons(
                p, Config::AppConfig::instance().getWindowContentScale());
    }
    /// @brief 高度按触摸倍率扩展，宽度继续由当前正文区域决定。
    /// @warning 每帧常量计算，不改变桌面尺寸。
    // 内容宽度由调用者测量，此处只提供可触摸的纵向下限。
    // 不向配置写回临时尺寸，也不改变桌面全局字号或缩放。
    float curveGraphHeight(float h, float scale) const override
    {
        return UI::Interaction::activeProfile().touchGestures()
                   ? std::max(h, 220 * scale)
                   : h;
    }
    /// @brief 新参数副本不能继承旧触点身份。
    void resetCurveInput() override { m_curveEditor.reset(); }
    /// @brief 最近符号帮助由移动面板显式显示，桌面 Hover 保持原有入口。
    // 符号只插入原有数学模板，触屏帮助不是第二个解析器或独立表达式副本。
    // 帮助文本来自静态翻译键，不能把用户公式或字符内容当作键名。
    bool symbolHelp(const char* input, const char* help, bool clicked) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() ) return false;
        m_curveEditor.symbolHelp(input, help, clicked);
        return true;
    }
    /// @brief 只在符号网格外显示最近说明，不抢占公式输入焦点。
    void finishSymbols() override
    {
        if ( UI::Interaction::activeProfile().touchGestures() )
            m_curveEditor.finishSymbols();
    }
    /// @brief 活动图未提交布局时取消笔画，不能因折叠保留半笔候选。
    /// @warning 每帧只处理窗口副本，不访问会话或历史。
    // 仅取消本窗口遗漏的活动图，不扫描其它画布或联机实体。
    // 新副本 reset 已清理旧触点，不能恢复到另一段落的备份值。
    void finishCurveFrame(TimingInterpolation&       c,
                          TimingFunctionEditorState& s) override
    {
        if ( UI::Interaction::activeProfile().touchGestures() )
            m_curveEditor.finish(c, s);
    }
    /// @brief 触屏数值标签独占一行，英文标签不能挤掉可编辑区域。
    /// @warning 每帧只调整布局，数值与完成时机仍由原 InputDouble 管理。
    // 标签独立换行是移动排版策略，数值合法性仍由原参数窗口与模型校验。
    // 隐藏 ID 使用标签的局部栈，多个全宽字段不能共享同一个活动输入框。
    bool curveDouble(const char* label, double* value,
                     const char* format) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::curveDouble(
                label, value, format);
        ImGui::PushID(label);
        ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(-1);
        const bool changed = ImGui::InputDouble("##value", value, 0, 0, format);
        ImGui::PopID();
        return changed;
    }
    /// @brief 分拍输入全宽，避免触屏英文排版产生横向滚动。
    /// @warning 每帧只提交一个标准整数控件。
    bool curveInt(const char* label, int* value) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::curveInt(label, value);
        ImGui::PushID(label);
        ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(-1);
        const bool changed = ImGui::InputInt("##value", value, 0, 0);
        ImGui::PopID();
        return changed;
    }
    /// @brief 完整标签与下拉分行，共用列表选择和保存语义。
    /// @warning 每帧借用选项指针，不复制动态列表。
    // 传入列表按领域枚举次序排列，界面宽度调整不会交换选项身份。
    // 只有触屏方案使用全宽下拉，桌面和纯鼠标保留原生横向布局。
    bool curveCombo(const char* label, int* value, const char* const* items,
                    int count) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::curveCombo(
                label, value, items, count);
        ImGui::PushID(label);
        ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(-1);
        const bool changed = ImGui::Combo("##value", value, items, count);
        ImGui::PopID();
        return changed;
    }
    /// @brief 包括待定触点及双指导航，避免方案切换截断手势。
    /// @warning 每帧只读固定大小本地状态，不等待逻辑线程。
    bool hasGesture() const override
    {
        return m_range.pending() || m_gesture.active() ||
               m_navigation.active() ||
               m_ownership.active(Graphic::mobileTouchSnapshot());
    }
    /// @brief 只为单独选中的段落绘制端点，普通点不借用段落热区。
    /// @details 此层位于原曲线之后，不改变曲线缓存、采样或播放值。
    /// 未选对象没有端点热区，复选也不提供可能与整组平移冲突的局部入口。
    /// 活动范围使用本地候选，原曲线保留为参考，不等待逻辑线程确认。
    /// 参数弹窗、抽屉和播放拥有更高输入优先级，把手同步隐藏。
    /// @warning 每帧借用快照和选择 ID，不复制定义或访问 Registry。
    void renderOverlay(TimelineCanvas& c, const ImVec2& origin,
                       const ImVec2& size) override
    {
        const auto* s = snapshot(c);
        if ( !UI::Interaction::activeProfile().touchGestures() || !s ||
             !s->hasBeatmap || s->isPlaying ||
             s->currentTool != Logic::EditTool::Marquee ||
             UI::workspaceBlocksCanvasInput() ||
             ImGui::IsPopupOpen(
                 nullptr,
                 ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
            return;
        const auto* segment =
            m_range.pending() ? m_range.preview() : selectedRange(c, *s);
        if ( !segment ) return;
        // 正反投影共用已有时间轴，离屏端点不夹成屏幕边缘的伪把手。
        m_range.render(
            *segment,
            origin,
            size,
            Config::AppConfig::instance().getEditorSettings().professionalMode,
            Config::AppConfig::instance().getWindowContentScale(),
            origin.y + rangeY(c, size, segment->time),
            origin.y +
                rangeY(
                    c, size, segment->time + segment->interpolation.m_duration),
            !m_range.pending() || m_range.valid(*s));
    }
    /// @brief 触屏没有离开后悬浮，参数说明由菜单编辑入口承担。
    /// @warning 每帧仅读取当前方案能力，不执行动作或获取会话锁。
    bool showHoverDetails() const override
    {
        return !UI::Interaction::activeProfile().touchGestures();
    }
    /// @brief 参数确认与取消保持至少 44dp，桌面方案仍使用默认高度。
    /// @warning 弹窗每帧固定数学计算，不改写主题或用户配置。
    float parameterButtonHeight(float scale) const override
    {
        return UI::Interaction::activeProfile().touchGestures() ? 44 * scale
                                                                : 0;
    }
    /// @brief 触屏窗口与字体使用同一内容倍率，字段标签不能被桌面固定宽度截断。
    /// @details 最小宽度保留一行控件及其标签，实际仍由视口上限约束。
    /// 不逐帧强制窗口大小，用户较大的窗口和调整后的位置继续由 ImGui 保存。
    /// @warning 仅在参数窗口打开时查询，固定数学且不修改主题或配置。
    ImVec2 parameterWindowSize(bool minimum, float scale) const override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::parameterWindowSize(minimum,
                                                                        scale);
        return { 620 * scale, (minimum ? 320 : 820) * scale };
    }
    /// @brief 只有参数正文滚动，保存与取消始终留在弹窗底部。
    /// @details 子窗不使用 AutoResize，避免长表单将底部按钮继续挤出屏幕。
    /// 预留空间来自实际按钮高度和主题间距，不写死手机或平板分辨率。
    /// BeginChild 即使返回不可见也必须配对 EndChild，因此返回的是包装状态。
    /// 草稿和值校验仍归共同编辑器，不复制移动版模型或创建第二套命令。
    /// 本函数只在已打开的参数窗口调用，不影响常态画布更新。
    /// @warning 每帧只创建 ImGui 布局，不读取文件或访问逻辑会话。
    bool beginParameterBody(float scale) override
    {
        const float footer = parameterButtonHeight(scale);
        if ( footer == 0 ) return false;
        // 正文独占剩余高度，触摸滚动不会把底部操作一起带走。
        // 最小值仅保证 BeginChild 的有效几何，不改变用户保存的窗口尺寸。
        ImGui::BeginChild(
            "TimingTouchParameters",
            { 0,
              std::max(1.0f,
                       ImGui::GetContentRegionAvail().y - footer -
                           ImGui::GetStyle().ItemSpacing.y * 2) });
        return true;
    }
    /// @brief 触摸位置使用中性文案，经典和纯鼠标仍返回原有翻译键。
    /// @warning 每帧只选静态键；吸附状态来自已经锁存的起笔结果。
    const char* pointPositionKey(bool snapped) const override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() )
            return TimelineInteractionPresentation::pointPositionKey(snapped);
        return snapped ? "ui.mobile.timing.touch_position_snapped"
                       : "ui.mobile.timing.touch_position";
    }
    /// @brief 触屏说明不要求 Shift、Esc、滚轮或悬浮，保留桌面原说明。
    /// @warning 每帧仅查询能力，不检测硬件或创建文本副本。
    const char* rangeHintKey(bool preview) const override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() ) return nullptr;
        return preview ? "ui.mobile.timing.preview_hint"
                       : "ui.mobile.timing.range_hint";
    }
    /// @brief 视图隐藏即结束其预览和导航；不删除已经打开的参数草稿。
    void suspend(TimelineCanvas& c) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() ) return;
        rollback(c, true);
        if ( m_navigation.active() ) {
            // 让公共导航正常发布最终倍率，不丢弃此前已显示的视图位置。
            const auto* s = snapshot(c);
            if ( s )
                routeMobileNavigation(m_navigation,
                                      m_navigationZoom,
                                      Graphic::mobileTouchSnapshot(),
                                      false,
                                      "Timeline",
                                      {},
                                      *s,
                                      1,
                                      1);
        }
    }
    /// @brief 先完成输入归属，再推进原业务，弹层始终读取真实触摸边沿。
    /// @warning 每帧常量状态计算；选区只在起笔复制，命令异步提交不等待。
    void update(TimelineCanvas& c, const ImVec2& origin, const ImVec2& size,
                bool hovered, bool focused, bool checked) override
    {
        // 方案只在安全边界切换，但普通弹层本身不是编辑事务。
        // 代际变化时主动清理旧移动菜单，防止切到桌面后留下无人绘制的弹层。
        // 清理一次即可，常态帧不能反复关闭新方案自己打开的菜单。
        // 菜单身份仍按本画布隔离，不能关闭另一时间线或主画布弹层。
        // 这里在触控能力分支之前执行，确保离开触控的那一帧也能完成清理。
        const auto generation = UI::Interaction::profileGeneration();
        if ( m_generation != generation ) {
            m_generation = generation;
            rollback(c, true);
            ImGui::PushID(&c);
            if ( ImGui::BeginPopup("TouchTimingMenu") ) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            m_menuWasOpen = m_waitRelease = false;
        }
        // 安卓也允许经典或纯鼠标方案，此时不延迟物理按下，不参与手势仲裁。
        // 默认基类就是原桌面业务入口，不同时运行两套状态机。
        // 没有临时构造桌面解释器，切换方案不会引入逐帧分配。
        if ( !UI::Interaction::activeProfile().touchGestures() ) {
            TimelineInteractionPresentation::update(
                c, origin, size, hovered, focused, checked);
            return;
        }
        // 快照只在当前 UI 调用中借用，不复制共享所有权或保存跨线程指针。
        // 暂时没有快照时丢弃候选，不能在数据恢复后补交一次过期点击。
        const auto* s = snapshot(c);
        if ( !s ) {
            // 尚无可解释的时间域，候选不能留到之后的另一份快照。
            rollback(c, false);
            return;
        }
        const bool same =
            m_scene == s->beatmapPathKey && m_instance == s->beatmapInstanceId;
        if ( !same ) {
            // 同路径重载也不可恢复旧实体集合；身份字符串只在会话切换复制。
            rollback(c, false);
            m_scene    = s->beatmapPathKey;
            m_instance = s->beatmapInstanceId;
        }
        // 画布地址在其生命周期中稳定，多个 Timeline 实例不能共享弹层 ID。
        // 查询和绘制使用相同 ID 栈，菜单关闭边沿也由同一实例消费。
        ImGui::PushID(&c);
        const bool menuOpen = ImGui::IsPopupOpen("TouchTimingMenu");
        ImGui::PopID();
        // 通用参数窗也拥有输入，不能只检测本类创建的长按菜单。
        // 外部弹层中断后需要新触点，关闭弹层不会复活旧单指预览。
        const bool overlay = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const auto  point   = ImGui::GetMousePos();
        const auto  display = ImGui::GetIO().DisplaySize;
        const auto& raw     = Graphic::mobileTouchSnapshot();
        const bool  blocked = UI::workspaceBlocksCanvasInput();
        // 矩形使用窗口逻辑像素，不能混用 framebuffer 像素与屏幕坐标。
        // 右上角保留既有工具入口，触摸那里不应同时绘制时间点。
        // 连续拖动允许越出矩形；此检查仅决定新手势能否从这里开始。
        const auto inside = [&](float x, float y) {
            return x >= origin.x && y >= origin.y && x < origin.x + size.x &&
                   y < origin.y + size.y &&
                   !(x >= origin.x + size.x - 56 && y <= origin.y + 56);
        };
        const bool allowed     = s->hasBeatmap && !s->isPlaying && !blocked;
        const bool interrupted = !allowed || overlay || !same;
        // 归属采用原始第一指落点，第二指进入此视图不会从另一画布抢手势。
        // 窗口悬浮同时排除覆盖在相同矩形上的独立音频和属性窗口。
        // 播放、抽屉和弹层都属于外部接管，不在解除遮罩时自动恢复。
        const bool ownsOrigin =
            allowed && !overlay && hovered &&
            inside(raw.m_originX * display.x, raw.m_originY * display.y);
        // 仲裁先于任何合成按下，帧内已经全部抬手的快速多指也能被屏蔽。
        // 接管边沿只回退这一笔本地预览，不能调用 Undo 删除上一笔历史。
        // 普通越界不触发取消，保持拖动越过窗口边缘时的连续性。
        const bool taken = m_ownership.update(raw, ownsOrigin, interrupted);
        if ( taken || interrupted ||
             (m_ownership.blocksSingle() && m_gesture.active()) )
            rollback(c, same);
        // 协作修改或工具切换使冻结定义失效，取消不能覆盖新段落。
        // 原始触点尚未结束时不从当前指尖抓取另一个目标。
        if ( m_range.pending() && (s->currentTool != Logic::EditTool::Marquee ||
                                   !m_range.matches(*s)) )
            rollback(c, same);
        // 取消只清理本笔，导航仍可使用第二指即时更新视图。
        // 接管不转写成鼠标释放，避免范围在中断时落盘。
        // 两指共用主画布的非阻塞平移和缩放协议，但 Timeline 忽略横向平移。
        // 此帧边界与普通 Timing 输入完全一致，导航不读取另一画布的相机矩阵。
        // 二维时间线没有横向摄像机自由度，逻辑入口只接受其直接导航协议。
        // renderScaleY 随同当前快照传入，不能误用主画布预览压缩比例。
        // 已完成的导航保留显示结果，单指编辑取消不反向撤销视野变换。
        // 导航与编辑共享手势所有权，但各自保留独立的状态与收尾边界。
        UI::Interaction::SurfaceFrame frame{ UI::Interaction::Surface::Timeline,
                                             "Timeline",
                                             s,
                                             origin.x,
                                             origin.y + contentYOffset(c),
                                             size.x,
                                             size.y,
                                             hovered,
                                             focused };
        routeMobileNavigation(m_navigation,
                              m_navigationZoom,
                              raw,
                              m_ownership.ownsGesture() && !interrupted,
                              "Timeline",
                              frame,
                              *s,
                              size.x,
                              size.y);
        // 待定状态每帧推进，不使用定时线程、阻塞等待或毫秒同步窗口。
        // 只有选择工具允许长按菜单，绘制工具静止按住继续等待移动或松手。
        // 菜单存在或刚关闭时不允许新回放，避免关闭菜单的点击穿透画布。
        auto       output = MobileCanvasGesture::Output::Suppress;
        const bool eligible =
            allowed && hovered && inside(point.x, point.y) && !overlay;
        if ( eligible && !m_ownership.blocksSingle() &&
             ImGui::IsMouseClicked(0) && !m_gesture.active() &&
             s->currentTool == Logic::EditTool::Marquee ) {
            // 身份锁存在真实按下，不在越过移动门槛后从正文重新抓目标。
            // 抓取偏置包含当前显示补偿，热区边缘起手也不跳动。
            // 单选只查集合大小和缓存 ID，不复制全部时间线目标。
            if ( const auto* segment = selectedRange(c, *s) ) {
                const float first = origin.y + rangeY(c, size, segment->time);
                const float last =
                    origin.y +
                    rangeY(c,
                           size,
                           segment->time + segment->interpolation.m_duration);
                const auto part = MobileTimingRange::hit(
                    *segment,
                    point,
                    origin,
                    size,
                    Config::AppConfig::instance()
                        .getEditorSettings()
                        .professionalMode,
                    Config::AppConfig::instance().getWindowContentScale(),
                    first,
                    last);
                m_range.latch(
                    *segment,
                    part,
                    (part == MobileTimingRange::Part::Start ? first : last) -
                        point.y,
                    point.y);
            }
        }
        // 把手关闭长按菜单分类；静止按住只等待移动或释放。
        // 正文未锁存端点，仍保持原有菜单与整体拖动能力。
        if ( !menuOpen && !m_menuWasOpen && !m_ownership.blocksSingle() ) {
            if ( ImGui::IsMouseClicked(0) ) m_origin = point;
            output = m_gesture.update(
                ImGui::IsMouseClicked(0),
                ImGui::IsMouseDown(0),
                ImGui::IsMouseReleased(0),
                point,
                ImGui::GetTime(),
                10 * Config::AppConfig::instance().getWindowContentScale(),
                eligible || (m_gesture.active() && !interrupted),
                s->currentTool == Logic::EditTool::Marquee &&
                    !m_range.pending());
        }
        if ( !menuOpen && !m_menuWasOpen ) {
            // 一次轻点分为 Begin 和 Release 两帧回放，以复用原控制器生命周期。
            // 已排队释放的分类来自旧手势，不受下一触点已经按下的影响。
            // 已认定拖动即使返回原点也仍由原业务判断最终区间是否退化。
            const bool tap = (output == MobileCanvasGesture::Output::Begin &&
                              !m_gesture.dragging()) ||
                             (output == MobileCanvasGesture::Output::Release &&
                              !m_gesture.finishedDrag());
            // 轻点抖动不会改变起拍；释放使用原点，拖动则使用真实最后坐标。
            const auto replay = tap ? m_origin : m_gesture.replayPoint();
            // 本地选择会在拖动起笔时改变，因此接管前需要保留原集合。
            // 此复制只发生在用户开始一笔的边沿，不处于空闲逐帧路径。
            // 模型尚未变化，取消只需恢复 UI 集合并清除预览标志。
            if ( output == MobileCanvasGesture::Output::Begin &&
                 !m_range.pending() ) {
                m_beforeSelection = selection(c);
                m_hasBaseline     = true;
            }
            if ( m_range.pending() ) {
                // 不调用旧指针状态机，同一手指不能同时提交整组选区的平移。
                // 相反端点和原函数已固定，连续帧只更新两个范围标量。
                // 快速释放采用分类器保存的最终坐标，不用 Begin 的旧起点。
                // 端点不回放正文整体拖动，连续预览只更新本地范围。
                // 松手沿用原吸附助手，避免每帧创建它的临时拍点容器。
                const bool released =
                    output == MobileCanvasGesture::Output::Release ||
                    (output == MobileCanvasGesture::Output::Pass &&
                     !ImGui::IsMouseDown(0));
                const bool moved =
                    m_gesture.dragging() || m_gesture.finishedDrag();
                const float y = output == MobileCanvasGesture::Output::Release
                                    ? replay.y
                                    : point.y;
                if ( m_range.update(
                         output,
                         y,
                         moved,
                         released,
                         *s,
                         [&](float absoluteY, bool snap) {
                             return rangeTime(
                                 c, size, absoluteY - origin.y, snap);
                         }) )
                    commitRange(c, *m_range.preview());
                if ( released ) m_range.cancel();
                // 冲突或无变化也结束候选，不能留到下一次触摸时补交。
                // 系统与多指中断已在上游清理，此处只可能消费正常释放。
            } else {
                MobilePointerInputScope input(
                    output,
                    replay,
                    1,
                    raw.m_epoch != 0 &&
                        (m_ownership.blocksSingle() ||
                         (raw.m_count == 0 && !m_gesture.active())));
                route(c,
                      origin,
                      size,
                      hovered,
                      focused,
                      checked,
                      tap,
                      m_gesture.finishedDrag());
                if ( output == MobileCanvasGesture::Output::Begin &&
                     m_gesture.dragging() ) {
                    // 快速拖动在起笔当帧补交当前位置，不能丢掉首次越过容差的末端。
                    MobilePointerInputScope motion(
                        MobileCanvasGesture::Output::Pass,
                        point,
                        1,
                        false,
                        true);
                    route(
                        c, origin, size, hovered, focused, true, false, false);
                }
            }
            // 正常结束后立即释放基线内容，下一笔不能恢复上一笔的选择。
            // 容器可保留容量，避免连续编辑反复重新分配桶。
            // 已提交的草稿属于原参数窗口，本类不尝试关闭或重建它。
            // 排队回放尚未结束时不能提前清理基线，否则第二指将无法恢复原选择。
            // 真正空闲后才消费副本，区别于只看原始设备已经全部抬手。
            if ( !m_gesture.active() ) {
                m_hasBaseline = false;
                m_beforeSelection.clear();
            }
        }
        // 参数窗口的创建不能处于菜单子窗口内部，先保存唯一的请求位。
        // 同一帧只有一个触摸按钮激活，不存在多次编辑草稿覆盖。
        // 返回值不包含模型指针，退出弹层后由当前快照重新解析目标。
        bool edit = false;
        // 长按从未向业务状态机发送 Begin，原指释放不会触发编辑或删除。
        ImGui::PushID(&c);
        if ( output == MobileCanvasGesture::Output::Menu ) {
            prepareMenu(c, origin, size);
            m_waitRelease = true;
            ImGui::OpenPopup("TouchTimingMenu");
        }
        // 菜单每帧重校验身份，外部重载后即便按钮还绘制也必须同步禁用。
        // CloseCurrentPopup 是关闭请求，不能将其当作本帧已经退出绘制。
        // 独立的参数入口延迟到 PopID 之后，避免成为菜单的子弹层。
        if ( ImGui::BeginPopup("TouchTimingMenu") ) {
            if ( !validMenu(c) ) ImGui::CloseCurrentPopup();
            ImGui::BeginDisabled(!validMenu(c));
            edit = renderMenu(c);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        // 保存打开状态供下一帧消费关闭点击，不能仅依赖本帧 BeginPopup 返回值。
        // 点击外部使弹层在 NewFrame 就关闭时，这个门闩仍保护画布一次。
        // 随后的全新 Down 才重新参与单指分类，旧菜单释放不进入绘制。
        m_menuWasOpen = ImGui::IsPopupOpen("TouchTimingMenu");
        ImGui::PopID();
        // 已退出菜单 ID 栈再打开普通参数窗口，保持弹层的父级和关闭规则。
        if ( edit && validMenu(c) ) perform(c, TimingMenuAction::Edit);
    }

private:
    MobileReadPress
        m_tablePress;  ///< 表格与画布各自锁定原始代际，不共享编辑状态。
    /// @brief 单选才提供局部边界，复选继续沿用正文整组平移。
    /// @details 普通点或失效句柄返回空，不用屏幕最近段落猜测用户选中对象。
    /// 非法时间或短于最小范围的段落不提供端点，避免构造无序夹取边界。
    /// @warning 每帧只匹配缓存 ID，不构造目标列表或复制插值缓存。
    const Common::Render::TimingInterpolationElement* selectedRange(
        TimelineCanvas& c, const Common::Render::RenderSnapshot& s) const
    {
        if ( selection(c).size() != 1 ) return nullptr;
        // 本地集合只包含句柄；实际定义必须重新从本帧快照取得。
        // 同号的普通点不会命中插值描述，因此不会误出现范围把手。
        const auto entity = *selection(c).begin();
        for ( const auto& range : s.timingInterpolations )
            if ( range.entity == entity && range.time >= 0 &&
                 std::isfinite(range.time) &&
                 std::isfinite(range.interpolation.m_duration) &&
                 std::isfinite(range.time + range.interpolation.m_duration) &&
                 range.interpolation.m_duration >= .001 )
                return &range;
        return nullptr;
    }
    /// @brief 接管回退本笔的 UI 选择及预览，不回滚任何此前已提交的修改。
    /// @details 只有同实例可以恢复基线；另一谱面不能复用实体编号。
    /// @pre 调用者已经判定第二指、系统取消、弹层或会话切换的接管原因。
    /// @post 未提交的点、区间和移动预览均结束，已完成历史保持不动。
    /// @note 恢复基线只适用于相同实例，新实例即便复用实体编号也不能套用。
    /// 同一个取消可在多个遮罩帧经过，但只有首次拥有基线时执行恢复。
    /// 使用 swap 消费基线，避免取消期间重新分配与完整复制集合。
    /// 后续 clear 保留容量，连续多次编辑不会重复创建解释器或桶数组。
    void rollback(TimelineCanvas& c, bool same)
    {
        // 本地端点从未改模型，丢弃 optional 不需要反向命令或历史补偿。
        // 首次取消后即为空，后续遮罩帧重复调用不再增减缓存所有权。
        m_range.cancel();
        if ( m_hasBaseline && same ) selection(c).swap(m_beforeSelection);
        if ( m_gesture.active() || m_hasBaseline ) cancel(c);
        m_gesture.cancel();
        m_hasBaseline = false;
        m_beforeSelection.clear();
    }
    /// @brief 双列长按菜单使用统一移动按钮，长文本换行，不依赖悬浮说明。
    /// @return 编辑参数延迟到菜单作用域退出后执行，其他动作在激活边沿提交。
    /// @warning 每帧固定按钮数量，不读取剪贴板，不遍历逻辑 ECS。
    bool renderMenu(TimelineCanvas& c)
    {
        // 原长按手指仍停在菜单位置时所有项均禁用，包括它松开的那一帧。
        // 只有下一次独立点击才能激活，避免弹层出现时直接执行删除。
        // 按钮继续读真实 IO，不在 MobilePointerInputScope 内绘制。
        if ( !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0) )
            m_waitRelease = false;
        ImGui::BeginDisabled(m_waitRelease);
        // 缩放取当前窗口内容比例，不另设与用户配置脱节的设备固定倍率。
        // 菜单字体和图标继续来自统一工作区外观，不新增平台专用资源。
        // 相同按钮尺寸只约束触摸命中区域，不改写全局 ImGui 样式。
        const float scale = std::max(
            1.0f, Config::AppConfig::instance().getWindowContentScale());
        // 七行菜单限制在当前屏幕高度内；缩放后仍保留最小可触摸高度。
        // 宽度只由移动按钮网格决定，不改变桌面菜单的内容或排版。
        // 长标签由移动统一按钮内部换行，不裁掉文字或退回 Hover 提示。
        // 先扣除实际主题间距，不能用估算留白抵消七行间的六条间隙。
        // 高密度实体机的间距也随 DPI 增大，否则最后一行会被屏幕裁切。
        const auto& style = ImGui::GetStyle();
        const float available =
            ImGui::GetMainViewport()->WorkSize.y - style.WindowPadding.y * 2 -
            style.ItemSpacing.y * 6 - style.DisplaySafeAreaPadding.y * 2;
        const float  height = std::clamp(available / 7, 44 * scale, 64 * scale);
        const ImVec2 cell{ 136 * scale, height };
        using A = TimingMenuAction;
        /// @brief 固定菜单项只保存语义与翻译键，不缓存当前谱面数据。
        struct Item {
            A           m_action;
            const char* m_label;
            const char* m_icon;
        };
        // 固定表保存语义和翻译键，不在每帧查询动作注册表或复制谱面。
        // 复制与剪切遵循相同内部 Timing 剪贴板格式，不借主画布音符数据。
        // 追加和减选影响下一笔；不在切换时重新解释已经完成的选择。
        // 定位使用冻结菜单落点，不使用当前播放头或按钮所在坐标。
        // 编辑入口包含普通点与插值正文，类型由公共目标解析决定。
        // 忽略吸附是触控方案临时状态，不写用户桌面配置或谱面历史。
        // 选择模式复用已有移动短标题，不能把桌面完整说明塞进两列按钮。
        // 时间线全选只操作自身目标，避免沿用桌面“当前轨道区”的误导文案。
        // 参数和吸附提供中英文短译文，说明性长句仍留在原参数窗口中。
        // 更换标题不改变动作枚举或菜单锚点，语言切换不会重新定义编辑行为。
        const std::array<Item, 14> items{
            { { A::Copy, "ui.edit.copy", UI::ICON_MMM_COPY },
              { A::Paste, "ui.edit.paste", UI::ICON_MMM_PASTE },
              { A::Cut, "ui.edit.cut", UI::ICON_MMM_SCISSORS },
              { A::Delete,
                "ui.settings.shortcut.delete_selected",
                UI::ICON_MMM_ERASER },
              { A::Undo, "ui.edit.undo", UI::ICON_MMM_UNDO },
              { A::Redo, "ui.edit.redo", UI::ICON_MMM_REDO },
              { A::SelectAll,
                "ui.mobile.timing.select_all",
                UI::ICON_MMM_SELECT_ALL },
              { A::Clear, "ui.mobile.selection.clear", UI::ICON_MMM_CLOSE },
              { A::Replace,
                "ui.mobile.selection.replace",
                UI::ICON_MMM_SELECT_ALL },
              { A::Add, "ui.mobile.selection.add", UI::ICON_MMM_PLUS },
              { A::Subtract,
                "ui.mobile.selection.subtract",
                UI::ICON_MMM_ERASER },
              { A::Edit, "ui.mobile.timing.edit_target", UI::ICON_MMM_PEN },
              { A::Snap, "ui.mobile.timing.bypass_snap", UI::ICON_MMM_MAGNET },
              { A::Seek, "ui.selection.seek_here", UI::ICON_MMM_CLOCK } }
        };
        bool edit = false;
        // 固定数组保持行顺序，Copy/Paste 等成对动作位于同一行。
        // 本地化文字参与按钮布局，不能以文本长度决定语义索引。
        // 每项稳定整数 ID 避免翻译切换让长按菜单误复用其他按钮状态。
        // 选择模式的可用性与是否存在选区无关，可在空白处先设置后绘制框选。
        // 除参数编辑外，动作只由一次按钮激活执行，不使用按住重复触发。
        for ( std::size_t i = 0; i < items.size(); ++i ) {
            if ( i % 2 ) ImGui::SameLine();
            const auto& item = items[i];
            ImGui::PushID(static_cast<int>(i));
            // 仅在激活边沿读取剪贴板或发布命令，绘制菜单本身没有模型副作用。
            // 空选择只禁用依赖选择的项，粘贴、全选和历史操作依然可用。
            // 与整个菜单的身份门禁叠加，不能靠一个局部 enabled 绕过播放限制。
            ImGui::BeginDisabled(!enabled(c, item.m_action));
            // 勾选明确表示下一笔的集合模式，触屏用户无需悬浮才能识别当前状态。
            // 模式属于方案本地值，显示菜单不会修改它，也不写入谱面数据。
            // 忽略吸附独立于集合模式，可与替换、追加或减选同时启用。
            // 图标替换只影响呈现，动作仍以固定语义枚举分派，不能靠翻译反查。
            // 非模式项继续使用原动作图标，避免把按钮存在误读为状态已开启。
            // 多个菜单实例只读同一个方案模式，不能各自保存过期的高亮副本。
            // 勾选不会消费额外触摸事件，外壳仍负责按下到释放的唯一激活。
            const auto operation =
                UI::Interaction::activeProfile().selectionOperation();
            const bool marked =
                (item.m_action == A::Replace &&
                 operation == Logic::SelectionOperation::Replace) ||
                (item.m_action == A::Add &&
                 operation == Logic::SelectionOperation::Add) ||
                (item.m_action == A::Subtract &&
                 operation == Logic::SelectionOperation::Subtract) ||
                (item.m_action == A::Snap &&
                 UI::Interaction::activeProfile().bypassSnapping());
            if ( UI::workspaceActionButton(
                     TR(item.m_label).data(),
                     marked ? UI::ICON_MMM_CHECK : item.m_icon,
                     cell) ) {
                // 关闭请求不能使本帧触摸穿透；菜单关闭帧仍由外壳消费。
                if ( item.m_action == A::Edit )
                    edit = true;
                else
                    perform(c, item.m_action);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        return edit;
    }
    MobileTimingCurveEditor
                      m_curveEditor;  ///< 参数图表私有输入，不暴露到公开头。
    MobileTimingRange m_range;  ///< 移动端局部首尾候选，公共画布不包含其定义。
    MobileCanvasGesture    m_gesture;     ///< 轻点、拖动和长按的非阻塞分类器。
    MobilePointerOwnership m_ownership;   ///< 原始代际归属及多指接管门闩。
    MobileCanvasNavigation m_navigation;  ///< 双指平移缩放及历史轻触状态。
    float m_navigationZoom{ 1 };  ///< 一次导航的即时倍率，避免滞后快照累乘。
    std::unordered_set<entt::entity>
                   m_beforeSelection;  ///< 起笔基线，仅取消时恢复。
    std::string    m_scene;         ///< 会话身份只在变化时更新，常态帧不复制。
    std::uint64_t  m_generation{};  ///< 安全方案切换代际，普通帧不重建解释器。
    std::uintptr_t m_instance{};    ///< 同路径重载的实例校验。
    ImVec2         m_origin{};      ///< 轻点抖动仍使用首次落点建立时间点。
    bool           m_hasBaseline{}, m_waitRelease{},
        m_menuWasOpen{};  ///< 本笔事务与弹层释放保护。
};
}  // namespace
/// @brief 构造一次移动解释器，私有状态不出现在公共时间线头中。
std::unique_ptr<TimelineInteractionPresentation>
createMobileTimelineInteractionPresentation()
{
    return std::make_unique<MobileTimelineInteractionPresentation>();
}
}  // namespace MMM::Canvas
