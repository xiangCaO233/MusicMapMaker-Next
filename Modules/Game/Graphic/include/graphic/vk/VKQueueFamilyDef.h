#pragma once

#include <cstdint>
#include <optional>

namespace MMM
{
namespace Graphic
{

/*
 * vk逻辑设备图形队列族索引
 * */
struct QueueFamilyIndices final {
    std::optional<uint32_t> graphicsQueueIndex;
    std::optional<uint32_t> presentQueueIndex;

    operator bool()
    {
        return graphicsQueueIndex.has_value() && presentQueueIndex.has_value();
    }
};

}  // namespace Graphic

}  // namespace MMM
