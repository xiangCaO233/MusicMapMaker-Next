#pragma once

namespace MMM::UI
{

/// @brief 可由菜单激活、但需要长期保留在 UIManager 中的独立窗口能力接口。
/// @warning UI 热路径接口：实现只允许读写窗口本地状态，不得执行阻塞操作。
class IAuxiliaryWindowView
{
public:
    virtual ~IAuxiliaryWindowView() = default;

    /// @brief 查询独立窗口当前是否打开。
    [[nodiscard]] virtual bool isWindowOpen() const = 0;

    /// @brief 设置独立窗口打开状态。
    /// @param open 是否打开窗口。
    virtual void setWindowOpen(bool open) = 0;

    /// @brief 激活窗口；已聚焦可见时关闭，否则恢复并聚焦。
    virtual void activateWindow() = 0;
};

}  // namespace MMM::UI
