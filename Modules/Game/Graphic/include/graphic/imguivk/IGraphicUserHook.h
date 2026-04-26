#pragma once

#include "graphic/imguivk/VKSwapchain.h"
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class IGraphicUserHook
{
public:
    virtual void onPrepareResources(vk::PhysicalDevice& physicalDevice,
                                    vk::Device&         logicalDevice,
                                    VKSwapchain&        swapchain,
                                    vk::CommandPool&    cmdPool,
                                    vk::Queue&          queue) = 0;
    virtual void onUpdateUI()                                  = 0;
    virtual void onRecordOffscreen(vk::CommandBuffer& cmd, uint32_t frameIndex) = 0;
};

}  // namespace MMM::Graphic
