#pragma once

#include <imgui.h>

#include <cstddef>

namespace MMM::UI
{
/// @brief 绘制批注正文，将配置的 Enter 组合解释为提交意图，其余组合保留换行。
/// @param label 调用方提供的稳定输入控件 ID。
/// @param buffer 跨帧保存的空字符结尾正文，不由此控件分配或释放。
/// @param capacity 包含末尾空字符的缓冲容量。
/// @param size 本帧文本框尺寸，不影响正文容量。
/// @param submitWithShiftEnter 为真时 Shift+Enter 提交，否则普通 Enter 提交。
/// @param enableKeyboardSubmit 触屏软键盘关闭隐式提交，始终由显式按钮保存。
/// @return 正文拥有焦点且指定组合产生非重复按下边沿时返回 true。
/// @details 这里只报告意图，正文、作者、权限和目标有效性仍由调用方验证。
/// 字符过滤只屏蔽本次提交按键生成的换行，不修改全局修饰键或平台输入队列。
/// @warning UI 热路径：每个可见正文框每帧调用，不分配状态、不获取会话锁。
inline bool annotationTextInput(const char* label, char* buffer,
                                std::size_t capacity, const ImVec2& size,
                                bool submitWithShiftEnter,
                                bool enableKeyboardSubmit = true)
{
    const auto& io = ImGui::GetIO();
    // 不把 Ctrl／Alt／Super 组合当作提交；Shift 的含义完全由当前偏好决定。
    const bool submitModifiers = enableKeyboardSubmit && !io.KeyCtrl &&
                                 !io.KeyAlt && !io.KeySuper &&
                                 io.KeyShift == submitWithShiftEnter;
    const bool submitPressed =
        submitModifiers && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    // 按住确认键产生的重复也不能插入换行；真正提交只接受上面的首个边沿。
    // 普通粘贴时没有物理确认键按下，粘贴文本里的换行必须完整保留。
    bool filterNewline =
        submitModifiers && (ImGui::IsKeyDown(ImGuiKey_Enter) ||
                            ImGui::IsKeyDown(ImGuiKey_KeypadEnter));
    // 回调只在同步 InputText 调用内借用栈上标志，不跨帧保留用户数据指针。
    const auto filter = [](ImGuiInputTextCallbackData* data) -> int {
        return data->EventChar == '\n' &&
                       *static_cast<const bool*>(data->UserData)
                   ? 1
                   : 0;
    };
    // 多行控件的普通返回值代表正文变更，不能拿来当作“添加批注”。
    ImGui::InputTextMultiline(label,
                              buffer,
                              capacity,
                              size,
                              ImGuiInputTextFlags_CallbackCharFilter,
                              filter,
                              &filterNewline);
    // 确认键在其他控件或窗口按下时，不能提交这份未聚焦的批注草稿。
    return submitPressed && ImGui::IsItemActive();
}
}  // namespace MMM::UI
