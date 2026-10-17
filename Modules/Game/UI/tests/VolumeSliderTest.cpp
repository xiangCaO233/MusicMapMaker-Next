#include "ui/utils/UIWidgetUtils.h"
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
/// @brief 在固定几何窗口中绘制音量滑块，并赋予键盘焦点。
/// @param value 输入输出音量；测试检查真实控件提交的结果。
/// @details 不使用平台后端，只注入 ImGui 自身接收的鼠标与按键事件。
/// @warning 仅测试渲染路径；无文件操作、网络或真实设备访问。
/// 测试固定窗口坐标，确保鼠标目标落在滑块轨道而不是文字标签。
/// @param maximumPercent 本轮真实滑块范围；默认总线与采样分别为 100 和 200。
void frame(float& value, int maximumPercent = 100)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 320, 100 });
    ImGui::Begin("Volume",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    // 模拟 Tab 已进入滑块；不依赖窗口创建时的默认导航顺序。
    ImGui::SetFocusID(ImGui::GetID("gain"), ImGui::GetCurrentWindow());
    // 控件宽度与显示尺寸独立，输入区不会因自动布局变化而漂移。
    ImGui::SetNextItemWidth(250);
    MMM::UI::FeedbackVolumeSlider("gain", &value, maximumPercent);
    // 完整结束并渲染这一帧，使下一次注入具有真实的跨帧激活状态。
    ImGui::End();
    ImGui::Render();
}
/// @brief 模拟一次完整按下/释放，避免重复计时或残留键状态干扰步长。
/// @param key 需要验证的方向键。
/// @param value 被控音量。
/// @details 每次按键都包含完整释放帧，四个方向不共享上一键的状态。
/// 不使用 sleep；按键重复时间由固定的 ImGui 帧间隔推进。
/// @param maximumPercent 按下和释放两帧必须使用同一范围，避免切换坐标映射。
void press(ImGuiKey key, float& value, int maximumPercent = 100)
{
    ImGui::GetIO().AddKeyEvent(key, true);
    frame(value, maximumPercent);
    ImGui::GetIO().AddKeyEvent(key, false);
    frame(value, maximumPercent);
}
}  // namespace
/// @brief 验证四方向一步一点、边界夹取以及鼠标五点格。
/// @return 控件满足交互约定时为零。
/// @details 验证直接调用业务控件，不用复制步进公式代替真实输入。
/// 默认字体预先构建，满足无图形后端 ImGui 的最小上下文要求。
/// 浮点比较使用远小于一个百分点的容差，不能掩盖多走一步的问题。
/// 鼠标断言同时验证非端点和五点格，避免控件未响应却落在零值通过。
int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 禁止测试写入用户布局；无后端上下文仍需有效帧时间与字体图集。
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 320, 100 };
    io.DeltaTime          = 1.0F / 60;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    float value = 0.5F;
    frame(value);
    // 上下也必须调整音量，而不是把焦点移出当前控件。
    press(ImGuiKey_UpArrow, value);
    // 累积所有阶段结果，后续成功不会覆盖前面已经发现的失败。
    bool valid = std::abs(value - 0.51F) < 0.0001F;
    // 前一键已经释放，本次增加必须来自右键自己的事件。
    press(ImGuiKey_RightArrow, value);
    valid &= std::abs(value - 0.52F) < 0.0001F;
    press(ImGuiKey_LeftArrow, value);
    press(ImGuiKey_DownArrow, value);
    valid &= std::abs(value - 0.5F) < 0.0001F;
    // 在上界继续增加不能溢出到一百以上。
    value = 1.0F;
    press(ImGuiKey_RightArrow, value);
    valid &= value == 1.0F;
    // 鼠标位置刻意不落在整五点上，结果必须吸附到相邻五点格。
    io.AddMousePosEvent(103, 15);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(value);
    valid &= value > 0 && value < 1 &&
             static_cast<int>(std::lround(value * 100)) % 5 == 0;
    // 释放结束交互，检查销毁上下文前不存在仍按下的输入。
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(value);
    // 采样音量使用相同控件但允许 200，100 不再被误当成上界。
    value = 1.0F;
    frame(value, 200);
    press(ImGuiKey_RightArrow, value, 200);
    valid &= std::abs(value - 1.01F) < 0.0001F;
    // 无论上界为何，四方向键的实际精度仍是一个百分点。
    press(ImGuiKey_DownArrow, value, 200);
    valid &= std::abs(value - 1.0F) < 0.0001F;
    // 二倍增益是采样上限，继续按增加键不得写出超过 200 的整数百分数。
    value = 2.0F;
    press(ImGuiKey_UpArrow, value, 200);
    valid &= value == 2.0F;
    // 采样轨道右侧的鼠标位置应产生大于一的增益，且仍量化到五点格。
    io.AddMousePosEvent(205, 15);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(value, 200);
    // 高增益范围的鼠标量化保持五点格，不能沿用旧的零到一百映射。
    valid &= value > 1 && value <= 2 &&
             static_cast<int>(std::lround(value * 100)) % 5 == 0;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(value, 200);
    // 前面的默认范围上界断言仍保留，证明扩大采样范围不会改变混音器默认行为。
    // 上下文销毁前完成所有状态读取，不保留窗口或控件观察指针。
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
