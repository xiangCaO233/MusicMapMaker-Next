#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 音量统一使用整数百分数，磁盘与混音器仍使用线性倍率。
/// @param label 稳定的控件标识。
/// @param value 输入输出线性音量；显示上限由 maximumPercent 指定。
/// @param maximumPercent 整数百分数上限，采样为 200，其余默认 100。
/// @return 只有实际倍率改变时才返回 true。
/// @warning 每帧 UI 路径，仅操作当前控件状态，禁止文件写入和跨线程等待。
bool FeedbackVolumeSlider(const char* label, float* value, int maximumPercent)
{
    // 百分数只存在于交互层，避免改动资源格式和音频 API 的单位。
    // 不编辑时不量化原始值，旧谱面的高精度音量仍可无损保存。
    // 非有限输入显示为零，只有用户有效编辑才写回数据。
    // 上限限制在显示缓冲可容纳的三位数范围，方向步长仍固定为一个百分点。
    const int     maximum  = std::clamp(maximumPercent, 1, 200);
    const ImGuiID id       = ImGui::GetID(label);
    const int     previous = static_cast<int>(std::lround(
        std::clamp(
            std::isfinite(*value) ? *value : 0.0F, 0.0F, maximum * 0.01F) *
        100.0F));
    // 使用整数草稿让键盘一格准确表示一个百分点。
    // 相反方向同时按下时抵消，不能因处理顺序产生漂移。
    int percent = previous;
    // 文本编辑保留原生光标导航；只有滑块焦点独占四个调整方向。
    const bool editingText = ImGui::TempInputIsActive(id);
    const bool focused     = GImGui->ActiveId == id || GImGui->NavId == id;
    int        delta       = 0;
    if ( focused && !editingText ) {
        // 锁定方向键，防止调整音量时同时导航到相邻控件。
        for ( const auto key : { ImGuiKey_LeftArrow,
                                 ImGuiKey_DownArrow,
                                 ImGuiKey_RightArrow,
                                 ImGuiKey_UpArrow } ) {
            ImGui::SetKeyOwner(key, id, ImGuiInputFlags_LockThisFrame);
            if ( ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id) ) {
                delta += key == ImGuiKey_LeftArrow || key == ImGuiKey_DownArrow
                             ? -1
                             : 1;
            }
        }
    }
    // 保留标准反馈、焦点与 Ctrl 点击输入能力。
    // AlwaysClamp 也约束直接输入的数值，禁止负音量和超过一百。
    // 保存原生滑块框宽，不包含标签文字，否则居中文本会偏向标签。
    // 后续仅补绘数值，不创建额外 Item，因此激活 ID 与释放边沿保持一致。
    const float frameWidth = ImGui::CalcItemWidth();
    const bool  nativeText =
        editingText || (ImGui::GetIO().KeyCtrl &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    const bool changed = FeedbackSliderInt(label,
                                           &percent,
                                           0,
                                           maximum,
                                           nativeText ? "%d" : "",
                                           ImGuiSliderFlags_AlwaysClamp);
    // 鼠标定位取最近五点格；Ctrl 点击的整数输入不受五点格限制。
    if ( ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
         !ImGui::TempInputIsActive(id) ) {
        percent = std::clamp(
            static_cast<int>(std::lround(percent / 5.0F)) * 5, 0, maximum);
    }
    // 用交互前的值计算，覆盖原生 Slider 的方向步进，避免一次按键加两次。
    // 不采用原生 Slider 的百分比导航步长，用户定义的精度优先。
    if ( delta != 0 ) percent = std::clamp(previous + delta, 0, maximum);
    // 原生滑块先计算鼠标位置，之后才完成五点格量化。
    // 普通状态自行绘制最终整数，避免实际为 35 时仍显示未量化的 33。
    // Ctrl 文本编辑继续使用原生格式与输入框，不覆盖输入中的字符。
    if ( !nativeText && !ImGui::TempInputIsActive(id) &&
         ImGui::IsItemVisible() ) {
        char       text[4]{};
        const auto result = std::to_chars(text, text + 3, percent);
        *result.ptr       = '\0';
        // 数值绘制使用滑块框几何，不读取窗口历史位置或其他控件状态。
        // 这样缩放布局时仍能和原生输入框保持相同的中心。
        const ImVec2 start = ImGui::GetItemRectMin();
        const ImVec2 end{ start.x + frameWidth,
                          start.y + ImGui::GetFrameHeight() };
        // 使用当前字体裁剪到滑块框，不把文字画到旁边静音控件上。
        ImGui::RenderTextClipped(
            start, end, text, nullptr, nullptr, { 0.5F, 0.5F });
    }
    if ( (changed || delta != 0) && percent != previous ) {
        *value = percent * 0.01F;
        // 保留业务面板依赖的编辑结束边沿，键盘与鼠标都能触发持久化。
        ImGui::MarkItemEdited(id);
        return true;
    }
    // 五点格内的鼠标小幅移动不产生业务更新，避免重复保存相同数值。
    // 返回值描述最终值变化，而不是原始滑块内部是否发生了移动。
    return false;
}
}  // namespace MMM::UI
