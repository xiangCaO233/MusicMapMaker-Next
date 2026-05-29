#pragma once

#include "graphic/imguivk/VKSwapchain.h"
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class IGraphicUserHook
{
public:
    /// @brief 在单帧渲染前准备用户层图形资源。
    /// @warning 热路径：每帧渲染准备阶段调用；只能检查脏位并触发低频重建。
    virtual void onPrepareResources(vk::PhysicalDevice& physicalDevice,
                                    vk::Device&         logicalDevice,
                                    VKSwapchain&        swapchain,
                                    vk::CommandPool&    cmdPool,
                                    vk::Queue&          queue) = 0;

    /// @brief 更新用户层 UI。
    /// @warning 热路径：每帧 ImGui 阶段调用；禁止文件系统访问、完整 ECS
    /// 遍历和完整排序。
    virtual void onUpdateUI() = 0;

    /// @brief 录制用户层离屏渲染命令。
    /// @warning
    /// 热路径：每帧命令录制阶段调用；禁止堆资源生命周期变更和阻塞等待。
    virtual void onRecordOffscreen(vk::CommandBuffer& cmd,
                                   uint32_t           frameIndex) = 0;
};

}  // namespace MMM::Graphic
