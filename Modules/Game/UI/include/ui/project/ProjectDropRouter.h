#pragma once
#include <memory>

namespace MMM::UI
{
/// @brief 主窗口级文件夹与谱包拖放路由，与演练窗口和具体画布解耦。
/// @details GLFW 投放先进入内部队列，再由 UI
/// 更新阶段验证位置和类型后发布项目事件。
class ProjectDropRouter
{
public:
    /// @brief 订阅系统文件拖放事件。
    ProjectDropRouter();
    /// @brief 解除订阅。
    /// @warning 析构前必须保证事件总线仍然存活。
    ~ProjectDropRouter();
    /// @brief 处理本帧的投放，模态弹窗或阻塞操作期间丢弃投放。
    /// @warning 每帧只查看事件队列，实际文件系统检查仅在拖放完成时执行。
    void update(bool enabled);

private:
    struct Impl;  ///< 隔离事件类型和队列容器的私有实现。

    /// @brief 独占订阅令牌与待消费投放队列。
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::UI
