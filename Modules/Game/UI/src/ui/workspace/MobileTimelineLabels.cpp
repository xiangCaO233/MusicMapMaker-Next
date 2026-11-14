#include "ui/workspace/MobileTimelineLabels.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace MMM::UI
{
/// @brief 泳道标签维持横向文字，窄栏沿用桌面的往复滚动节奏。
/// @details 只叠加文字，不改变 Timing 标记的投影与拾取几何。
/// @param origin 时间线内容区的屏幕原点，不包含页签高度。
/// @param size 原时间线的可见尺寸，标题区只占底部一行。
/// @note 标签不是触控按钮，不添加 Item 或改变画布输入归属。
/// @note 由平台策略调用，桌面继续使用它的原覆盖层。
/// @note 往复与两端停留节奏保持一致，不另起计时器或状态缓存。
/// @note 背板高度与文字裁剪高度一致，避免字体下缘落在拍线上。
/// @note 用户更大的上下留白继续保留，不用默认值覆盖样式。
/// @warning 每帧固定绘制五个短标识，不分配、访问文件或查询会话。
void drawMobileTimelineLaneLabels(ImVec2 origin, ImVec2 size)
{
    constexpr std::array<std::string_view, 5> labels{
        "BPM", "Scroll", "Jump", "HS", "STOP"
    };
    if ( size.x <= 1 || size.y <= 1 ) return;
    auto&       draw    = *ImGui::GetWindowDrawList();
    const float lane    = size.x / labels.size();
    const auto  padding = ImGui::GetStyle().FramePadding;
    // 字号已包含设备与用户倍率，不能再用固定三十二像素裁掉字形上下缘。
    // 标签区域只需要完整一行及主题上下留白，不拆字、不旋转、不缩小字体。
    const float height =
        std::min(size.y, ImGui::GetTextLineHeight() + 2 * padding.y);
    const ImVec2 end{ origin.x + size.x, origin.y + size.y };
    const float  top = end.y - height;
    // 整体裁剪先限制在画布内，滚动或调整分隔条时不覆盖其他窗口。
    // 背板继承当前主题，不使用写死的浅色字或固定圆角。
    draw.PushClipRect(origin, end, true);
    draw.AddRectFilled(
        { origin.x, top }, end, ImGui::GetColorU32(ImGuiCol_WindowBg));
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    for ( std::size_t i = 0; i < labels.size(); ++i ) {
        // 最后一列吸收浮点等分误差，水平留白不能吞掉整个窄泳道。
        const float left  = origin.x + lane * i;
        const float right = i + 1 == labels.size() ? end.x : left + lane;
        const float inset = std::min(padding.x, (right - left) * 0.1f);
        // 短标签居中静止，长标签保持横排，滚动无需鼠标悬浮。
        // 桌面尾部四十像素余量大于窄泳道宽度时会整行消失，因此只滚实际溢出距离。
        const auto  text      = ImGui::CalcTextSize(labels[i].data());
        const float available = std::max(0.0f, right - left - 2 * inset);
        const float overflow  = std::max(0.0f, text.x - available);
        float       offset{};
        if ( overflow > 0 ) {
            // 仅超宽名称查询时钟，能放下的 BPM、HS 等短标签完全静止。
            // 最大偏移等于实际溢出量，结束停留时正好完整展示名称末尾。
            // 最小偏移为零，开始停留时正好展示名称开头，不引入空白前缀。
            // 横向留白已从可见宽度扣除，不能再把内边距加到移动距离上。
            // 本地独立距离规则不修改公共滚动 helper，桌面其他控件保持原样。
            // 沿用相同正弦往复与端点停留，距离有界，最后一段始终留在视口内。
            // 时间只影响视觉偏移，不缓存窗口状态或延迟任何编辑命令。
            const float phase =
                std::sin(static_cast<float>(ImGui::GetTime()) * 0.5f - 1.57f) *
                    0.5f +
                0.5f;
            offset = overflow * std::clamp((phase - 0.1f) / 0.8f, 0.0f, 1.0f);
        }
        draw.PushClipRect(
            { left + inset, top }, { right - inset, end.y }, true);
        const float center = overflow > 0 ? 0.0f : (available - text.x) * 0.5f;
        draw.AddText({ left + inset + center - offset,
                       top + (height - ImGui::GetFontSize()) * 0.5f },
                     color,
                     labels[i].data());
        // 各泳道独立裁剪，横向滚动不会覆盖相邻类型；字号与主题色保持不变。
        draw.PopClipRect();
    }
    // 完整恢复父窗口的裁剪，之后的工具和抽屉不继承泳道文字区域。
    draw.PopClipRect();
}
}  // namespace MMM::UI
