#pragma once

#include <cstdint>
#include <optional>

namespace MMM::Graphic
{

/**
 * @brief Vulkan 逻辑设备队列族索引结构体
 *
 * 用于存储从物理设备查询到的支持特定功能（如图形、呈现）的队列族索引。
 */
struct QueueFamilyIndices final {

    /// @brief 图形队列族索引 (支持 Graphics Bit)
    std::optional<uint32_t> graphicsQueueIndex;

    /// @brief 呈现队列族索引 (支持 Present 表面)
    std::optional<uint32_t> presentQueueIndex;

    /**
     * @brief 检查是否找到了所有必需的队列族索引
     * @return true 如果图形和呈现队列索引都已找到
     * @return false 如果缺失任何一个必需的队列索引
     */
    operator bool() const
    {
        return graphicsQueueIndex.has_value() && presentQueueIndex.has_value();
    }
};

} // namespace MMM::Graphic


