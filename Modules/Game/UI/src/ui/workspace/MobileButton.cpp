#include "ui/workspace/MobileButton.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <imgui_internal.h>
#include <limits>

namespace MMM::UI
{
/// @brief 绘制只对触摸按下作出视觉反馈的移动按钮。
/// @param label 稳定的可见标签及可选隐藏 ID，不以文字位置充当身份。
/// @param size 调用方给出的区域；负宽度表示使用当前内容宽度。
/// @param selected 用持久底色表达工具或开关状态，不依赖悬停。
/// @return 沿用统一按钮入口的激活语义，不在按下阶段执行重复命令。
/// @warning 每帧只读时钟和窗口局部状态，禁止引入线程、等待或全局动画列表。
bool MobileButton(const char* label, ImVec2 size, bool selected)
{
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    // 显式尺寸也必须容纳手指与当前字号；调用方据返回的实际控件尺寸排布。
    const auto text = ImGui::CalcTextSize(label, nullptr, true);
    // 内容区已由外壳扣除配置留白，填充宽度不能再次伸入该留白。
    if ( size.x < 0 ) size.x = ImGui::GetContentRegionAvail().x;
    size.x        = std::max({ size.x, 44.0f * scale, text.x + 16.0f * scale });
    size.y        = std::max({ size.y, 44.0f * scale, text.y + 14.0f * scale });
    auto* storage = ImGui::GetStateStorage();
    // 动画身份与按钮身份绑定，不同窗口或 PushID 中的同名按钮互不影响。
    const auto key = ImGui::GetID(label) ^ 0x71ab49f1u;
    // 持续按住时维持全亮，释放帧从之前的亮度平滑衰减，不先跳暗再闪回。
    if ( ImGui::GetActiveID() == ImGui::GetID(label) && ImGui::IsMouseDown(0) )
        storage->SetFloat(key, static_cast<float>(ImGui::GetTime()));
    const float age =
        static_cast<float>(ImGui::GetTime()) - storage->GetFloat(key, -100.0f);
    // 插值只改变颜色，不推迟命令提交；动画期间主线程继续正常更新。
    // 有界区间使首次绘制和长时间返回前台后的状态直接回到常态。
    // 松手后在 140ms 内回归常态，保留短促反馈而不拖慢连续触摸的观感。
    const float  flash     = std::clamp(1.0f - age / 0.14f, 0.0f, 1.0f);
    const ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 buttonCol = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 frameCol  = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    // 未选中按钮使用与桌面控件帧协调的通透底色，避免大面积实心色块造成沉重感；
    // 选中按钮保持高亮主题色，使工具与开关状态在无悬停时一目了然。
    const ImVec4 base =
        selected ? activeCol
                 : ImVec4{ frameCol.x * 0.55f + buttonCol.x * 0.45f,
                           frameCol.y * 0.55f + buttonCol.y * 0.45f,
                           frameCol.z * 0.55f + buttonCol.z * 0.45f,
                           std::clamp(buttonCol.w * 0.72f, 0.45f, 0.85f) };
    // 从当前皮肤底色提亮，避免硬编码纯白遮掉浅色主题的按钮边界。
    // RGB 分量分别饱和，不把 HDR 范围外的颜色交给常规 UI 绘制。
    const ImVec4 bright{ std::min(1.0f, base.x + 0.30f),
                         std::min(1.0f, base.y + 0.30f),
                         std::min(1.0f, base.z + 0.30f),
                         1.0f };
    const ImVec4 color{ base.x + (bright.x - base.x) * flash,
                        base.y + (bright.y - base.y) * flash,
                        base.z + (bright.z - base.z) * flash,
                        base.w + (1.0f - base.w) * flash };
    // 悬浮与常态相同；可见变化只来自真实按下或选中状态。
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bright);
    // 圆角沿用当前美化设置；按钮的触摸尺寸不参与主题参数覆盖。
    const bool clicked = FeedbackButton(label, size);
    // 叠加与桌面皮肤一致的 1px 细边框，选中态用主题激活色强化轮廓边界。
    // 边框绘制在按钮命中矩形内部边缘，不扩大布局占位，也兼容深浅两套内置主题。
    // 圆角半径直接读取当前样式的
    // FrameRounding，确保与输入框和桌面按钮视觉统一。 线宽随 DPI
    // 缩放保持至少一像素，在高分平板与普通手机上均维持清晰锐利的卡片边界。
    const auto   rectMin   = ImGui::GetItemRectMin();
    const auto   rectMax   = ImGui::GetItemRectMax();
    const float  rounding  = ImGui::GetStyle().FrameRounding;
    const ImVec4 borderVec = ImGui::GetStyleColorVec4(
        selected ? ImGuiCol_ButtonActive : ImGuiCol_Border);
    // 选中按钮提高边框不透明度以凸显激活态，常态按钮保留适度半透明避免喧宾夺主。
    const ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(
        ImVec4{ borderVec.x,
                borderVec.y,
                borderVec.z,
                selected ? 0.90f : std::max(0.32f, borderVec.w) });
    ImGui::GetWindowDrawList()->AddRect(
        rectMin, rectMax, borderCol, rounding, 0, std::max(1.0f, 1.0f * scale));
    // 点击时间记录在本窗口存储，隐藏再显示不会创建全局动画任务。
    // 统一入口仍负责点击音效，移动控件不重复播放或修改音效池。
    // 反馈结束只回到基础色，选中工具继续保留选中底色。
    if ( clicked ) storage->SetFloat(key, static_cast<float>(ImGui::GetTime()));
    ImGui::PopStyleColor(3);
    return clicked;
}

namespace
{
/// @brief 在上一按钮的命中矩形内绘制图文，不产生额外的交互 Item。
/// @param icon 图标 UTF-8 字形，由皮肤的专用图标字体解释。
/// @param label 常驻可见文字，使用当前 UI 字体而非图标字体绘制。
/// @param stacked 为 true 时上下排列，为 false 时图标与文字横排。
/// @details 两种按钮共用字体来源与测量方式，皮肤切换不会留下旧字体指针。
/// DrawList 仍使用当前窗口裁剪，抽屉滑出屏幕时不会绘制到别的面板。
/// @warning 每个可见按钮每帧调用，只借用已缓存字体并提交固定数量图元。
void drawMobileButtonContents(const char* icon, const char* label, bool stacked)
{
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    const auto  start = ImGui::GetItemRectMin();
    const auto  end   = ImGui::GetItemRectMax();
    // 固定列宽优先，长翻译在图标右侧换行，不扩张命中区或侵入下一列。
    // 测量与 AddText 使用同一个换行宽度，避免只增高背景却仍画出单行文字。
    // 控件裁剪也限制字形边缘，滚动抽屉时不能污染旁边的列。
    const float wrap =
        stacked ? 0.0f : std::max(1.0f, end.x - start.x - 44.0f * scale);
    const auto labelSize = ImGui::CalcTextSize(label, nullptr, false, wrap);
    auto*      draw      = ImGui::GetWindowDrawList();
    const auto color     = ImGui::GetColorU32(ImGuiCol_Text);
    // 图标和中文分别使用自己的字体，不要求中文字体合并所有私用区字形。
    auto* iconFont = Config::SkinManager::instance().getFont("pure_icons");
    if ( !iconFont ) iconFont = ImGui::GetFont();
    const float iconSize   = 20.0f * scale;
    const auto  iconBounds = iconFont->CalcTextSizeA(
        iconSize, std::numeric_limits<float>::max(), 0, icon);
    // 组合块的尺寸只用于居中，不改变外层已经建立的命中矩形。
    // 上下排列留较小行距，横排则给不同字体的视觉边界留出空白。
    const float gap    = (stacked ? 3.0f : 8.0f) * scale;
    const float width  = stacked ? std::max(iconBounds.x, labelSize.x)
                                 : iconBounds.x + gap + labelSize.x;
    const float height = stacked ? iconBounds.y + gap + labelSize.y
                                 : std::max(iconBounds.y, labelSize.y);
    const float left   = start.x + (end.x - start.x - width) * .5f;
    const float top    = start.y + (end.y - start.y - height) * .5f;
    // 组合块整体居中，横排时再分别垂直居中，避免不同字体基线造成错位。
    draw->PushClipRect(start, end, true);
    draw->AddText(
        iconFont,
        iconSize,
        { stacked ? start.x + (end.x - start.x - iconBounds.x) * .5f : left,
          stacked ? top : top + (height - iconBounds.y) * .5f },
        color,
        icon);
    draw->AddText(ImGui::GetFont(),
                  ImGui::GetFontSize(),
                  { stacked ? start.x + (end.x - start.x - labelSize.x) * .5f
                            : left + iconBounds.x + gap,
                    stacked ? top + iconBounds.y + gap
                            : top + (height - labelSize.y) * .5f },
                  color,
                  label,
                  nullptr,
                  wrap);
    draw->PopClipRect();
}
}  // namespace

/// @brief 用单一触摸命中区组合图标与标签，动画继续复用移动按钮入口。
/// @details 图标使用皮肤专用字体，文字保留当前业务字体和可读字号。
/// 两者分别测量，不能把 FontAwesome 字形与中文当作同一种字体排版。
/// @return 松手激活时返回 true，不把按住或滑过误作多次点击。
/// @warning 每帧固定数量文字测量与 DrawList 提交，不申请纹理资源。
bool MobileNavigationButton(const char* id, const char* icon, const char* label,
                            ImVec2 size, bool selected)
{
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    const auto  labelSize = ImGui::CalcTextSize(label);
    // 先计算最终尺寸再交给按钮，命中矩形和实际图文区域完全一致。
    // 文字宽度优先于建议宽度，避免用户增大字号后标签被裁掉。
    size.x = std::max({ size.x, 56.0f * scale, labelSize.x + 14.0f * scale });
    size.y =
        std::max({ size.y, 52.0f * scale, labelSize.y * 2 + 12.0f * scale });
    ImGui::PushID(id);
    // 可见文字由 DrawList 绘制，隐藏标签避免 ImGui 再绘制一次默认文本。
    // ID 由调用方提供，因此本地化与选中状态变化不会生成另一个动画实例。
    const bool clicked = MobileButton("##navigation", size, selected);
    drawMobileButtonContents(icon, label, true);
    ImGui::PopID();
    return clicked;
}

/// @brief 布局与绘制共用宽度约束，长翻译通过增加高度而非突破列边界显示。
/// @details 只有未指定宽度才按自然文字宽度计算，负宽度使用内容区域。
/// @warning 固定一次字体测量，不修改字体倍率或持久化样式。
ImVec2 mobileActionButtonSize(const char* label, ImVec2 size)
{
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    if ( size.x < 0 ) size.x = ImGui::GetContentRegionAvail().x;
    if ( size.x == 0 ) size.x = ImGui::CalcTextSize(label).x + 44.0f * scale;
    size.x          = std::max(size.x, 44.0f * scale);
    const auto text = ImGui::CalcTextSize(
        label, nullptr, false, std::max(1.0f, size.x - 44.0f * scale));
    // 触摸下限与用户字号共同决定高度，正宽度不再被英文长文案覆盖。
    size.y = std::max({ size.y, 44.0f * scale, text.y + 14.0f * scale });
    return size;
}

/// @brief 带固定图标的移动操作按钮，可见状态文字不参与内部 ID。
/// @details 填满宽度之前扣除的留白由外壳决定，按钮不越过内容边界扩张。
/// 图标、文字和背景构成同一个触摸目标，不分别响应点击。
/// @return 仅松手激活时返回 true，颜色动画不延迟业务命令。
/// @warning 每帧只测量当前短标签，不创建后台动画或所有权拷贝。
bool MobileActionButton(const char* id, const char* icon, const char* label,
                        ImVec2 size, bool selected)
{
    size = mobileActionButtonSize(label, size);
    ImGui::PushID(id);
    // 操作与导航使用不同内部标签，即使调用方复用 ID 也不混淆局部状态。
    const bool clicked = MobileButton("##action", size, selected);
    drawMobileButtonContents(icon, label, false);
    ImGui::PopID();
    return clicked;
}
}  // namespace MMM::UI
