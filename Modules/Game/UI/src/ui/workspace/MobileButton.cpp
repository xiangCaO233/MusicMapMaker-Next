#include "ui/workspace/MobileButton.h"
#include "config/AppConfig.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <imgui_internal.h>

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
    const float  flash = std::clamp(1.0f - age / 0.38f, 0.0f, 1.0f);
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
}  // namespace MMM::UI
