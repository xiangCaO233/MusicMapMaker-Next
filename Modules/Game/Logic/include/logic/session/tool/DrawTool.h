#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 绘制工具，负责在谱面上放置/创建新的音符。
class DrawTool : public IEditTool
{
public:
    /// @brief 开始绘制笔刷并记录初始吸附位置。
    /// @warning 逻辑热路径：鼠标按下绘制时执行；只能使用已缓存 BPM
    /// 事件，禁止完整 timing 排序进入普通绘制路径。
    void handleStartBrush(SessionContext&      ctx,
                          const CmdStartBrush& cmd) override;

    /// @brief 更新绘制笔刷的当前物件形态与吸附位置。
    /// @warning 逻辑热路径：绘制拖动时频繁执行；禁止完整 ECS 遍历、完整排序和
    /// 文件系统访问。
    void handleUpdateBrush(SessionContext&       ctx,
                           const CmdUpdateBrush& cmd) override;

    /// @brief 结束绘制笔刷并提交创建/合并 action。
    /// @warning 逻辑热路径低频分支：鼠标松开时执行；允许提交 action，但应避免
    /// 额外的全量谱面同步。
    void handleEndBrush(SessionContext& ctx, const CmdEndBrush& cmd) override;

    /// @brief 开始橡皮擦笔刷并记录首个目标实体。
    /// @warning 逻辑热路径：鼠标按下擦除时执行；禁止完整 note ECS 遍历。
    void handleStartErase(SessionContext&      ctx,
                          const CmdStartErase& cmd) override;

    /// @brief 更新橡皮擦当前目标实体。
    /// @warning
    /// 逻辑热路径：擦除拖动时频繁执行；只允许读取悬停状态和常量级组件。
    void handleUpdateErase(SessionContext&       ctx,
                           const CmdUpdateErase& cmd) override;

    /// @brief 结束橡皮擦笔刷并提交删除/分裂 action。
    /// @warning
    /// 逻辑热路径低频分支：鼠标松开时执行；折线子物件只能批量收集，禁止
    /// 对每个父实体重复完整扫描。
    void handleEndErase(SessionContext& ctx, const CmdEndErase& cmd) override;
};


}  // namespace MMM::Logic
