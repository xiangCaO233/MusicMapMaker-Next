#pragma once

#include "canvas/AnnotationExport.h"
#include "canvas/AnnotationTableData.h"
#include "ui/IAuxiliaryWindowView.h"
#include "ui/IUIView.h"

#include <cstddef>
#include <filesystem>
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

    /// @brief 打开当前格式对应的保存对话框。
    /// @warning 只在点击导出时调用，原生对话框可能阻塞 UI。
    void openExportFilePicker();

    /// @brief 消费内置文件选择器的确认或取消操作。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧仅检查对话框状态，实际写文件只在确认时进行。
    void renderExportFileDialog(float dpiScale);

    /// @brief 将当前批注快照导出到选定路径。
    /// @param path 文件选择器返回的路径。
    /// @warning 仅用户确认导出时执行全量格式化和磁盘写入。
    void exportToPath(const std::filesystem::path& path);

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

    /// @brief 导出文件格式选择，按界面顺序存储。
    int m_exportFormat{ 0 };

    /// @brief 位置格式选择，零为时间戳，一为拍号。
    int m_exportPosition{ 0 };

    /// @brief 文件选择器打开时锁定的格式，避免弹窗期间修改选项造成扩展名错配。
    AnnotationExportFormat m_dialogExportFormat{ AnnotationExportFormat::Txt };

    /// @brief 最近一次导出结果的翻译键；空值不显示反馈。
    std::string m_exportStatusKey;

    /// @brief 最近一次导出是否成功，用于反馈文字颜色。
    bool m_exportSucceeded{ false };
};

}  // namespace MMM::Canvas
