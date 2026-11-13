#pragma once
#include <imgui.h>
namespace MMM::UI
{
/// @brief 为刚提交的单值控件补充触屏长按精调，桌面原路径不变。
/// @details 仅支持 float、double、int；源指针只在本次调用使用，跨帧只保存值。
/// 应用时返回 true 并写回；取消、返回、隐藏及失焦都不写回草稿。
/// @warning 每个可见控件固定次数判定；文本解析只在草稿变化时执行。
bool mobileScalarPrecisionItem(ImGuiDataType type, void* value,
                               const void* minimum, const void* maximum,
                               double step);
/// @brief 系统收起键盘时结束文本焦点，防止后端因活动字段立即重开输入法。
/// @details 只清除活动输入，不提交草稿或关闭业务弹窗；出现与动画中不抢焦点。
/// @warning 每帧只检查两个布尔值和 ImGui 状态，不查询平台或业务对象。
void releaseMobileTextFocusAfterKeyboardDismiss(bool wasVisible, bool visible);
/// @brief 紧跟输入控件调用，用其真实矩形保持活动字段可见。
/// @details 只滚动移动正文子窗，不依据复合控件的导航矩形，也不移动确认行。
/// @warning 每帧固定状态与几何检查，无分配、平台调用或业务锁。
void keepMobileInputItemVisible();
/// @brief 键盘出现时把较长表单内容变成滚动区，底部确认行继续留在父窗口。
/// @details 移动表单始终保留同一子窗口，键盘切换只改高度，避免输入身份变化。
/// 桌面不创建子窗口，保留原有布局与控件 ID。
class MobileInputBodyScope final
{
public:
    /// @brief 在当前弹窗内为一行触摸确认按钮预留高度。
    explicit MobileInputBodyScope(const char* id);
    /// @brief 自动配对；调用 end 后析构不再重复结束。
    ~MobileInputBodyScope();
    /// @brief 在提交底部操作按钮前结束正文滚动区。
    void end();
    /// @brief 子窗口不拥有业务状态，禁止复制或移动配对对象。
    MobileInputBodyScope(const MobileInputBodyScope&)            = delete;
    MobileInputBodyScope& operator=(const MobileInputBodyScope&) = delete;

private:
    bool m_open{};  ///< 仅记录本次 BeginChild 是否已配对，不借用业务指针。
};
}  // namespace MMM::UI
