#include <vulkan/vulkan.hpp>

// 保留真实 SDK 类型定义，再模拟导出宏不存在，覆盖反向包含顺序。
// 该宏只用于 SDK 的模块导出，不属于本项目头文件的前置条件。
#ifdef VULKAN_HPP_EXPORT
#    undef VULKAN_HPP_EXPORT
#endif

#include "graphic/imguivk/IGraphicUserHook.h"

#include <type_traits>

/// @file
/// @brief 完整 Vulkan-Hpp 已包含时，图形回调的重声明必须仍使用真实句柄。
/// @note 这里只做类型检查，不引入全局对象、Vulkan 调用或图形设备初始化。
namespace
{
// 完整句柄的原生类型与 SDK 一致，排除误声明为其他包装类型的情况。
static_assert(std::is_same_v<vk::PhysicalDevice::CType, VkPhysicalDevice>);
static_assert(std::is_same_v<vk::Device::CType, VkDevice>);
static_assert(std::is_same_v<vk::CommandPool::CType, VkCommandPool>);
static_assert(std::is_same_v<vk::Queue::CType, VkQueue>);
static_assert(std::is_same_v<vk::CommandBuffer::CType, VkCommandBuffer>);
}  // namespace
