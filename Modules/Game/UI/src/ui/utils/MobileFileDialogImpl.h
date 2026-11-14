#pragma once
#include <ImGuiFileDialog.h>
#include <string>

namespace MMM::UI
{
/// @brief 在应用侧扩展内置选择器，高键盘期间保留文件名和固定操作行。
/// @details 目录、过滤器及输出结果继续归上游对象，不复制文件系统实现。
/// 工作区提供键盘安全区，选择器不跨 JNI 或根据机型猜测遮挡尺寸。
/// 隐藏浏览区不会销毁窗口，文件名输入身份必须跨键盘动画保持不变。
class MobileFileDialog : public IGFD::FileDialog
{
public:
    /// @brief 新事务清除上次键盘模式，但不改变原有配置或过滤器协议。
    void OpenDialog(const std::string& key, const std::string& title,
                    const char*                   filters,
                    const IGFD::FileDialogConfig& config = {}) override;

protected:
    /// @brief 根据前帧输入归属锁存紧凑模式；路径和搜索输入仍由原控件处理。
    /// @warning 每个可见帧固定几何判断，不访问文件或等待系统线程。
    void m_DrawHeader() override;
    /// @brief 键盘占用空间时暂停浏览绘制，收起后恢复原目录和滚动位置。
    /// @warning 每帧只选择绘制路径，不扫描目录或重建选择器。
    void m_DrawContent() override;
    /// @brief 文件名沿用原控件和缓冲，临时扩大行高后恢复所有样式。
    /// @warning 每帧只压栈固定样式，不创建输入副本或持有控件引用。
    bool m_DrawFooter() override;
    /// @brief 两枚反馈按钮固定触摸高度，保持原确认、取消和覆盖事务协议。
    /// @warning 每帧只提交控件，真正写出仍在调用方确认分支执行。
    bool m_DrawValidationButtons() override;

private:
    bool m_filenameFocused{};  ///< 前帧真实文件名控件状态，不能把搜索当文件名。
    bool m_compact{};  ///< 同一次键盘展开保持模式，按按钮时不能瞬间恢复浏览。
};
}  // namespace MMM::UI
