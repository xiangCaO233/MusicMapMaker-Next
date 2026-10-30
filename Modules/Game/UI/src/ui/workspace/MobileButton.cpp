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
    size.x        = std::max({ size.x, 48.0f * scale, text.x + 20.0f * scale });
    size.y        = std::max({ size.y, 48.0f * scale, text.y + 16.0f * scale });
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
    const float  flash = std::clamp(1.0f - age / 0.14f, 0.0f, 1.0f);
    const ImVec4 base  = ImGui::GetStyleColorVec4(
        selected ? ImGuiCol_ButtonActive : ImGuiCol_Button);
    // 从当前皮肤底色提亮，避免硬编码纯白遮掉浅色主题的按钮边界。
    // RGB 分量分别饱和，不把 HDR 范围外的颜色交给常规 UI 绘制。
    const ImVec4 bright{ std::min(1.0f, base.x + 0.35f),
                         std::min(1.0f, base.y + 0.35f),
                         std::min(1.0f, base.z + 0.35f),
                         1.0f };
    const ImVec4 color{ base.x + (bright.x - base.x) * flash,
                        base.y + (bright.y - base.y) * flash,
                        base.z + (bright.z - base.z) * flash,
                        1.0f };
    // 悬浮与常态相同；可见变化只来自真实按下或选中状态。
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bright);
    // 小圆角保持偏方形轮廓，最小触摸尺寸与视觉尺寸采用同一矩形。
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    const bool clicked = FeedbackButton(label, size);
    // 点击时间记录在本窗口存储，隐藏再显示不会创建全局动画任务。
    // 统一入口仍负责点击音效，移动控件不重复播放或修改音效池。
    // 反馈结束只回到基础色，选中工具继续保留选中底色。
    if ( clicked ) storage->SetFloat(key, static_cast<float>(ImGui::GetTime()));
    ImGui::PopStyleVar();
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
    const auto  labelSize = ImGui::CalcTextSize(label);
    const auto  start     = ImGui::GetItemRectMin();
    const auto  end       = ImGui::GetItemRectMax();
    auto*       draw      = ImGui::GetWindowDrawList();
    const auto  color     = ImGui::GetColorU32(ImGuiCol_Text);
    // 图标和中文分别使用自己的字体，不要求中文字体合并所有私用区字形。
    auto* iconFont = Config::SkinManager::instance().getFont("pure_icons");
    if ( !iconFont ) iconFont = ImGui::GetFont();
    const float iconSize   = 24.0f * scale;
    const auto  iconBounds = iconFont->CalcTextSizeA(
        iconSize, std::numeric_limits<float>::max(), 0, icon);
    // 组合块的尺寸只用于居中，不改变外层已经建立的命中矩形。
    // 上下排列留较小行距，横排则给不同字体的视觉边界留出空白。
    const float gap    = (stacked ? 4.0f : 8.0f) * scale;
    const float width  = stacked ? std::max(iconBounds.x, labelSize.x)
                                 : iconBounds.x + gap + labelSize.x;
    const float height = stacked ? iconBounds.y + gap + labelSize.y
                                 : std::max(iconBounds.y, labelSize.y);
    const float left   = start.x + (end.x - start.x - width) * .5f;
    const float top    = start.y + (end.y - start.y - height) * .5f;
    // 组合块整体居中，横排时再分别垂直居中，避免不同字体基线造成错位。
    draw->AddText(
        iconFont,
        iconSize,
        { stacked ? start.x + (end.x - start.x - iconBounds.x) * .5f : left,
          stacked ? top : top + (height - iconBounds.y) * .5f },
        color,
        icon);
    draw->AddText({ stacked ? start.x + (end.x - start.x - labelSize.x) * .5f
                            : left + iconBounds.x + gap,
                    stacked ? top + iconBounds.y + gap
                            : top + (height - labelSize.y) * .5f },
                  color,
                  label);
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
    size.x = std::max({ size.x, 64.0f * scale, labelSize.x + 16.0f * scale });
    size.y =
        std::max({ size.y, 72.0f * scale, labelSize.y * 2 + 16.0f * scale });
    ImGui::PushID(id);
    // 可见文字由 DrawList 绘制，隐藏标签避免 ImGui 再绘制一次默认文本。
    // ID 由调用方提供，因此本地化与选中状态变化不会生成另一个动画实例。
    const bool clicked = MobileButton("##navigation", size, selected);
    drawMobileButtonContents(icon, label, true);
    ImGui::PopID();
    return clicked;
}

/// @brief 带固定图标的移动操作按钮，可见状态文字不参与内部 ID。
/// @details 填满宽度之前扣除的留白由外壳决定，按钮不越过内容边界扩张。
/// 图标、文字和背景构成同一个触摸目标，不分别响应点击。
/// @return 仅松手激活时返回 true，颜色动画不延迟业务命令。
/// @warning 每帧只测量当前短标签，不创建后台动画或所有权拷贝。
bool MobileActionButton(const char* id, const char* icon, const char* label,
                        ImVec2 size, bool selected)
{
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    const auto  text  = ImGui::CalcTextSize(label);
    // 填满区域只读取当前内容宽度，不把父窗口的边框与滚动条算进按钮。
    if ( size.x < 0 ) size.x = ImGui::GetContentRegionAvail().x;
    // 横排需要为图标和图文间距留出空间，不能只沿用纯文字的最小宽度。
    size.x = std::max(size.x, text.x + 56.0f * scale);
    size.y = std::max({ size.y, 48.0f * scale, text.y + 16.0f * scale });
    ImGui::PushID(id);
    // 操作与导航使用不同内部标签，即使调用方复用 ID 也不混淆局部状态。
    const bool clicked = MobileButton("##action", size, selected);
    drawMobileButtonContents(icon, label, false);
    ImGui::PopID();
    return clicked;
}
}  // namespace MMM::UI
