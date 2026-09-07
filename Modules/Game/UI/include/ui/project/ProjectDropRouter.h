#pragma once
#include <memory>

namespace MMM::UI
{
/// @brief 主窗口级文件夹与谱包拖放路由，与演练窗口和具体画布解耦。
class ProjectDropRouter
{
public:
    /// @brief 订阅系统文件拖放事件。
    ProjectDropRouter();
    /// @brief 解除订阅。
    ~ProjectDropRouter();
    /// @brief 处理本帧的投放，模态弹窗或阻塞操作期间丢弃投放。
    /// @warning 每帧只查看事件队列，实际文件系统检查仅在拖放完成时执行。
    void update(bool enabled);

private:
    struct Impl;                   ///< 订阅与待投放队列。
    std::unique_ptr<Impl> m_impl;  ///< 独立的路由状态。
};
}  // namespace MMM::UI
