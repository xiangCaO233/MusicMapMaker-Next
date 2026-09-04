#pragma once

#include "canvas/AnnotationTableData.h"
#include "ui/IAuxiliaryWindowView.h"
#include "ui/IUIView.h"

#include <cstddef>
#include <optional>
#include <string>

namespace MMM::Canvas
{

/// @brief 独立管理谱面批注列表、详情和跳转操作的窗口。
class AnnotationTableWindow final : public UI::IUIView,
                                    public UI::IAuxiliaryWindowView
{
public:
    /// @brief 创建拥有独立数据生命周期的批注表窗口。
    /// @param name UIManager 中的稳定注册名。
    explicit AnnotationTableWindow(const std::string& name);

    /// @brief 每帧更新独立批注表窗口。
    /// @param sourceManager UI 管理器；当前实现不需要访问。
    /// @warning UI 热路径：窗口关闭时只检查布尔状态；窗口打开时按批注版本刷新。
    void update(UI::UIManager* sourceManager) override;

    /// @brief 暴露独立窗口能力接口。
    UI::IAuxiliaryWindowView* asAuxiliaryWindowView() override { return this; }

    /// @brief 查询批注表当前是否打开。
    [[nodiscard]] bool isWindowOpen() const override;

    /// @brief 设置批注表打开状态。
    /// @param open 是否打开窗口。
    void setWindowOpen(bool open) override;

    /// @brief 激活批注表；已聚焦可见时关闭，否则恢复并聚焦。
    void activateWindow() override;

private:
    /// @brief 在独立数据行替换后尽量保持当前选择。
    /// @param selectedId 刷新前选中批注的稳定 ID。
    void restoreSelection(const std::string& selectedId);

    /// @brief 清空独立数据和选择状态。
    void resetData();

    /// @brief 关闭窗口并清理所有瞬时状态。
    void closeWindow();

    /// @brief 批注表自己的可见状态，不与 Timeline 共享。
    bool m_isWindowOpen{ false };

    /// @brief 当前数据可用状态。
    AnnotationTableDataStatus m_dataStatus{ AnnotationTableDataStatus::Close };

    /// @brief 下次允许读取会话缓存的 ImGui 时间。
    double m_nextDataRefreshTime{ 0.0 };

    /// @brief 下一次绘制时是否检查并恢复窗口位置。
    bool m_shouldRecoverWindow{ false };

    /// @brief 下一次绘制时是否聚焦窗口。
    bool m_shouldFocusWindow{ false };

    /// @brief 上一帧窗口是否同时聚焦且可从显示器工作区访问。
    bool m_isFocusedAndReachable{ false };

    /// @brief 与 Timeline 快照和窗口生命周期无关的批注表数据。
    AnnotationTableData m_data;

    /// @brief 当前详情区选中的批注行。
    std::optional<std::size_t> m_selectedRow;
};

}  // namespace MMM::Canvas
