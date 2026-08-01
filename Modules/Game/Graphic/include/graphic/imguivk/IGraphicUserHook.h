#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class VKSwapchain;

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
    /// @brief 获取当前帧可拆分的离屏录制任务数量。
    /// @return 可独立录制的离屏任务数量。
    /// @warning 渲染热路径：每帧命令录制前调用，只允许读取已稳定的任务序列。
    virtual uint32_t getOffscreenRecordTaskCount() const { return 1; }

    /// @brief 录制指定索引的离屏任务。
    /// @param cmd 当前任务独占的命令缓冲。
    /// @param frameIndex 当前并发帧索引。
    /// @param taskIndex 当前钩子内部的任务索引。
    /// @warning 渲染热路径：可能在渲染线程池中执行，禁止访问 ImGui 状态或共享
    /// Vulkan command pool。
    virtual void onRecordOffscreenTask(vk::CommandBuffer& cmd,
                                       uint32_t frameIndex, uint32_t taskIndex)
    {
        if ( taskIndex == 0 ) {
            onRecordOffscreen(cmd, frameIndex);
        }
    }
};

}  // namespace MMM::Graphic
