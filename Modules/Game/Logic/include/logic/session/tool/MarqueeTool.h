#pragma once

#include "logic/session/tool/GrabTool.h"
#include <memory>

namespace MMM::Logic
{

/// @brief 选择工具，复用物件及子部件拖动事务，并处理空白区域框选。
/// @details GrabTool 仅保留为内部拖动实现，不再注册为独立可选工具。
class MarqueeTool : public GrabTool
{
public:
    /// @brief 检查点随工具分配一次，普通桌面框选不复制选择集合。
    MarqueeTool();
    /// @brief 在实现文件中销毁私有检查点，公开头不传播会话容器。
    ~MarqueeTool() override;
    /// @brief 开始创建框选区域，并在非加选模式下清空旧选择。
    /// @param ctx 会话上下文
    /// @param cmd 开始框选指令
    void handleStartMarquee(SessionContext&        ctx,
                            const CmdStartMarquee& cmd) override;

    /// @brief 更新当前框选区域，并标记选择结果需要重算。
    /// @param ctx 会话上下文
    /// @param cmd 框选更新指令
    void handleUpdateMarquee(SessionContext&         ctx,
                             const CmdUpdateMarquee& cmd) override;

    /// @brief 结束框选；取消恢复检查点，正常释放处理无效的小尺寸框选。
    /// @param ctx 会话上下文
    /// @param cmd 结束框选指令
    void handleEndMarquee(SessionContext&      ctx,
                          const CmdEndMarquee& cmd) override;

    /// @brief 移除鼠标位置命中的框选区域，并标记选择结果需要重算。
    /// @param ctx 会话上下文
    /// @param cmd 移除框选框指令
    void handleRemoveMarqueeAt(SessionContext&           ctx,
                               const CmdRemoveMarqueeAt& cmd) override;

private:
    /// @brief 仅保存本次可取消框选开始前的选择，不拥有会话或实体。
    struct SelectionCheckpoint;
    std::unique_ptr<SelectionCheckpoint>
        m_checkpoint;  ///< 容器容量跨手势复用。
};

}  // namespace MMM::Logic
